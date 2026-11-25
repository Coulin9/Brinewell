import AppKit
import BrinewellCore
import BrinewellDisplay
import CKrun
import Foundation
import IOSurface
import SwiftUI

/// The display backend offers the virtio-gpu scanout as a native Metal
/// texture when libkrun can export it; only resources that cannot be exported
/// (2D resources, guest-storage blobs, unsupported layouts) fall back to the
/// CPU framebuffer pool.
final class VMDisplay: @unchecked Sendable {
    private final class Slot: @unchecked Sendable {
        let surface: IOSurfaceRef
        let width: Int
        let height: Int
        let format: UInt32
        let size: Int
        let packed: UnsafeMutablePointer<UInt8>?
        var busy = false
        var locked = false

        init?(width: Int, height: Int, format: UInt32) {
            self.width = width
            self.height = height
            self.format = format
            self.size = width * height * 4
            let properties: [String: Any] = [
                kIOSurfaceWidth as String: width,
                kIOSurfaceHeight as String: height,
                kIOSurfaceBytesPerElement as String: 4,
                kIOSurfacePixelFormat as String: UInt32(0x42475241),
            ]
            guard let surface = IOSurfaceCreate(properties as CFDictionary) else { return nil }
            self.surface = surface
            if format == UInt32(KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM),
               IOSurfaceGetBytesPerRow(surface) == width * 4 {
                packed = nil
            } else {
                packed = UnsafeMutablePointer<UInt8>.allocate(capacity: size)
            }
        }

        deinit {
            if locked { IOSurfaceUnlock(surface, [], nil) }
            packed?.deallocate()
        }

        func beginWrite() -> UnsafeMutablePointer<UInt8>? {
            if let packed { return packed }
            guard IOSurfaceLock(surface, [], nil) == 0 else { return nil }
            locked = true
            let base = IOSurfaceGetBaseAddress(surface)
            return base.assumingMemoryBound(to: UInt8.self)
        }

        func finishWrite() -> Bool {
            if locked {
                IOSurfaceUnlock(surface, [], nil)
                locked = false
                return true
            }
            guard let source = packed,
                  IOSurfaceLock(surface, [], nil) == 0 else { return false }
            defer { IOSurfaceUnlock(surface, [], nil) }
            let base = IOSurfaceGetBaseAddress(surface)
            let destination = base.assumingMemoryBound(to: UInt8.self)
            let stride = IOSurfaceGetBytesPerRow(surface)
            let isR10 = format == UInt32(KRUN_DISPLAY_FORMAT_R10G10B10A2_UNORM) || format == UInt32(KRUN_DISPLAY_FORMAT_R10G10B10X2_UNORM)
            let isB10 = format == UInt32(KRUN_DISPLAY_FORMAT_B10G10R10A2_UNORM) || format == UInt32(KRUN_DISPLAY_FORMAT_B10G10R10X2_UNORM)
            if isR10 || isB10 {
                for y in 0..<height {
                    let src = source + y * width * 4
                    let dst = destination + y * stride
                    for x in 0..<width {
                        let i = x * 4
                        let word = UInt32(src[i]) | UInt32(src[i+1]) << 8 | UInt32(src[i+2]) << 16 | UInt32(src[i+3]) << 24
                        let low = UInt8(((word & 1023) * 255 + 511) / 1023)
                        let high = UInt8((((word >> 20) & UInt32(1023)) * UInt32(255) + UInt32(511)) / UInt32(1023))
                        dst[i] = isR10 ? high : low
                        dst[i+1] = UInt8((((word >> 10) & UInt32(1023)) * UInt32(255) + UInt32(511)) / UInt32(1023))
                        dst[i+2] = isR10 ? low : high
                        dst[i+3] = 255
                    }
                }
                return true
            }
            let offsets: (Int, Int, Int, Int?)
            switch Int32(format) {
            case KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM: offsets = (0, 1, 2, 3)
            case KRUN_DISPLAY_FORMAT_B8G8R8X8_UNORM: offsets = (0, 1, 2, nil)
            case KRUN_DISPLAY_FORMAT_A8R8G8B8_UNORM: offsets = (3, 2, 1, 0)
            case KRUN_DISPLAY_FORMAT_X8R8G8B8_UNORM: offsets = (3, 2, 1, nil)
            case KRUN_DISPLAY_FORMAT_R8G8B8A8_UNORM: offsets = (2, 1, 0, 3)
            case KRUN_DISPLAY_FORMAT_X8B8G8R8_UNORM: offsets = (1, 2, 3, nil)
            case KRUN_DISPLAY_FORMAT_A8B8G8R8_UNORM: offsets = (1, 2, 3, 0)
            case KRUN_DISPLAY_FORMAT_R8G8B8X8_UNORM: offsets = (2, 1, 0, nil)
            default: return false
            }
            for y in 0..<height {
                let src = source + y * width * 4
                let dst = destination + y * stride
                for x in 0..<width {
                    let i = x * 4
                    dst[i] = src[i + offsets.0]
                    dst[i + 1] = src[i + offsets.1]
                    dst[i + 2] = src[i + offsets.2]
                    dst[i + 3] = offsets.3.map { src[i + $0] } ?? 255
                }
            }
            return true
        }
    }

    private let lock = NSRecursiveLock()
    /// The scanout geometry from the last `configure_scanout`. CPU frame slots
    /// are allocated lazily so a session served entirely by native scanout
    /// never allocates IOSurfaces.
    private var configured: (width: Int, height: Int, format: UInt32)?
    private var slots: [Slot] = []
    // Allocations remain alive across reconfiguration until libkrun presents them.
    private var allocated: [UInt32: Slot] = [:]
    private var nextID: UInt32 = 0
    private var sequence: UInt64 = 0
    private var colorState: DisplayColorState = .sdr
    private var latest: DisplayFrame?
    private var deliveryScheduled = false
    private var retired = false
    @MainActor private let renderer: IOSurfaceRenderer
    @MainActor private var window: VMWindow?
    @MainActor private var inputView: VMInputView?
    @MainActor private var workContextWarning: String?
    @MainActor private var warningBanner: WorkContextBanner?
    @MainActor private var gameHUDState = GamePerformanceState()
    @MainActor private var gameHUD: GamePerformanceHUD?
    @MainActor private var gameHUDTimer: Timer?
    @MainActor private var gameHUDLevel: GameHUDLevel = .off
    @MainActor private var gameHUDPosition: GameHUDPosition = .topRight
    @MainActor private var gameHUDPlacement: [NSLayoutConstraint] = []
    @MainActor var hardwareSample: (() -> HostHardwareTelemetry.Snapshot?)?
    @MainActor var displayRefreshRate: Double?
    @MainActor private var reportedGameSource: GameFrameSample.Source?
    @MainActor private var windowPixelSize: NSSize = .zero
    @MainActor private var preserveWindowFrame = false
    @MainActor private var fittingWindow = false
    @MainActor private var refitWindow = false
    private var screenObserver: NSObjectProtocol?
    @MainActor private var hdrTimer: Timer?
    @MainActor private var sampledCapabilities: HostDisplayCapabilities?

    @MainActor init(existingWindow: VMWindow? = nil) throws {
        window = existingWindow
        renderer = try IOSurfaceRenderer()
    }

    func makeBackend() -> krun_display_backend {
        var backend = krun_display_backend()
        backend.features = UInt64(KRUN_DISPLAY_FEATURE_BASIC_FRAMEBUFFER)
            | UInt64(KRUN_DISPLAY_FEATURE_NATIVE_SCANOUT)
        backend.create_userdata = Unmanaged.passUnretained(self).toOpaque()
        backend.create = { instance, userdata, _ in
            guard let instance, let userdata else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            instance.pointee = UnsafeMutableRawPointer(mutating: userdata)
            return 0
        }
        backend.vtable.basic_framebuffer.destroy = { _ in 0 }
        backend.vtable.basic_framebuffer.configure_scanout = { instance, id, _, _, width, height, format in
            guard let instance else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            return Unmanaged<VMDisplay>.fromOpaque(instance).takeUnretainedValue()
                .configure(id: id, width: width, height: height, format: format)
        }
        backend.vtable.basic_framebuffer.disable_scanout = { instance, id in
            guard let instance else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            return Unmanaged<VMDisplay>.fromOpaque(instance).takeUnretainedValue().disable(id: id)
        }
        backend.vtable.basic_framebuffer.alloc_frame = { instance, id, buffer, size in
            guard let instance else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            return Unmanaged<VMDisplay>.fromOpaque(instance).takeUnretainedValue()
                .allocate(id: id, buffer: buffer, size: size)
        }
        backend.vtable.basic_framebuffer.present_frame = { instance, id, frame, _ in
            guard let instance else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            return Unmanaged<VMDisplay>.fromOpaque(instance).takeUnretainedValue().present(id: id, frame: frame)
        }
        backend.vtable.basic_framebuffer.present_native = { instance, frame in
            guard let instance, let frame else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            return Unmanaged<VMDisplay>.fromOpaque(instance).takeUnretainedValue()
                .presentNative(frame.pointee)
        }
        backend.vtable.basic_framebuffer.set_scanout_color = { instance, id, color in
            guard let instance, id == 0, let color else { return }
            let display = Unmanaged<VMDisplay>.fromOpaque(instance).takeUnretainedValue()
            display.lock.lock()
            display.colorState = VMDisplay.colorState(from: color.pointee)
            display.lock.unlock()
        }
        return backend
    }

    private func configure(id: UInt32, width: UInt32, height: UInt32, format: UInt32) -> Int32 {
        guard id == 0 else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        guard (1...VMDisplayConfiguration.maximumDimension).contains(Int(width)),
              (1...VMDisplayConfiguration.maximumDimension).contains(Int(height)) else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
        switch Int32(bitPattern: format) {
        case KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM, KRUN_DISPLAY_FORMAT_B8G8R8X8_UNORM,
             KRUN_DISPLAY_FORMAT_A8R8G8B8_UNORM, KRUN_DISPLAY_FORMAT_X8R8G8B8_UNORM,
             KRUN_DISPLAY_FORMAT_R8G8B8A8_UNORM, KRUN_DISPLAY_FORMAT_X8B8G8R8_UNORM,
             KRUN_DISPLAY_FORMAT_A8B8G8R8_UNORM, KRUN_DISPLAY_FORMAT_R8G8B8X8_UNORM,
             KRUN_DISPLAY_FORMAT_R10G10B10A2_UNORM, KRUN_DISPLAY_FORMAT_B10G10R10A2_UNORM,
             KRUN_DISPLAY_FORMAT_B10G10R10X2_UNORM, KRUN_DISPLAY_FORMAT_R10G10B10X2_UNORM: break
        default: return KRUN_DISPLAY_ERR_METHOD_UNSUPPORTED
        }
        lock.lock()
        let unchanged = configured.map {
            $0.width == Int(width) && $0.height == Int(height) && $0.format == format
        } ?? false
        if unchanged {
            lock.unlock()
            return 0
        }
        configured = (Int(width), Int(height), format)
        slots = []
        latest = nil
        scheduleDeliveryLocked()
        lock.unlock()
        return 0
    }

    private func disable(id: UInt32) -> Int32 {
        guard id == 0 else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        lock.lock()
        configured = nil
        slots = []
        latest = nil
        scheduleDeliveryLocked()
        lock.unlock()
        return 0
    }

    private func allocate(id: UInt32, buffer: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?, size: UnsafeMutablePointer<Int>?) -> Int32 {
        guard id == 0 else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        guard let buffer, let size else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
        lock.lock()
        defer { lock.unlock() }
        if slots.isEmpty {
            guard let configured else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
            var newSlots: [Slot] = []
            for _ in 0..<3 {
                guard let slot = Slot(width: configured.width, height: configured.height,
                                      format: configured.format) else {
                    return KRUN_DISPLAY_ERR_OUT_OF_BUFFERS
                }
                newSlots.append(slot)
            }
            slots = newSlots
        }
        guard let slot = slots.first(where: { !$0.busy }) else { return KRUN_DISPLAY_ERR_OUT_OF_BUFFERS }
        guard let pointer = slot.beginWrite() else { return KRUN_DISPLAY_ERR_INTERNAL }
        repeat { nextID = (nextID % UInt32(Int32.max)) + 1 } while allocated[nextID] != nil
        slot.busy = true
        allocated[nextID] = slot
        buffer.pointee = pointer
        size.pointee = slot.size
        return Int32(nextID)
    }

    private func present(id: UInt32, frame: UInt32) -> Int32 {
        guard id == 0 else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        lock.lock()
        guard let slot = allocated.removeValue(forKey: frame) else {
            lock.unlock()
            return KRUN_DISPLAY_ERR_INVALID_PARAM
        }
        guard slots.contains(where: { $0 === slot }) else {
            if slot.locked { IOSurfaceUnlock(slot.surface, [], nil); slot.locked = false }
            slot.busy = false
            lock.unlock()
            return KRUN_DISPLAY_ERR_INVALID_PARAM
        }
        lock.unlock()
        guard slot.finishWrite() else {
            lock.lock(); slot.busy = false; lock.unlock()
            return KRUN_DISPLAY_ERR_INTERNAL
        }
        lock.lock()
        defer { lock.unlock() }
        guard slots.contains(where: { $0 === slot }) else {
            slot.busy = false
            return KRUN_DISPLAY_ERR_INVALID_PARAM
        }
        sequence &+= 1
        do {
            let frame = try IOSurfaceFrame(surface: slot.surface, sequenceNumber: sequence, colorState: colorState) { [weak self, slot] in
                guard let self else { return }
                self.lock.lock()
                slot.busy = false
                self.lock.unlock()
            }
            latest = .surface(frame)
            scheduleDeliveryLocked()
            return 0
        } catch {
            slot.busy = false
            return KRUN_DISPLAY_ERR_INTERNAL
        }
    }

    private static func colorState(from color: krun_display_color_state) -> DisplayColorState {
        DisplayColorState(colorspace: color.colorspace, eotf: color.eotf, flags: color.flags,
            maxDisplayMasteringLuminance: color.max_display_mastering_luminance,
            minDisplayMasteringLuminance: color.min_display_mastering_luminance,
            maxCll: color.max_cll, maxFall: color.max_fall)
    }

    /// Called by libkrun on the GPU worker thread with a retained native
    /// texture. The frame's lease is released by the renderer once the last
    /// command buffer that samples it has completed.
    private func presentNative(_ frame: krun_display_native_frame) -> Int32 {
        guard frame.handle_type == UInt32(KRUN_DISPLAY_NATIVE_HANDLE_METAL_TEXTURE),
              let handle = frame.handle,
              frame.width > 0, frame.height > 0 else {
            frame.release?(frame.release_userdata)
            return KRUN_DISPLAY_ERR_INVALID_PARAM
        }
        let object = Unmanaged<AnyObject>.fromOpaque(handle).takeUnretainedValue()
        guard let texture = object as? any MTLTexture else {
            frame.release?(frame.release_userdata)
            return KRUN_DISPLAY_ERR_METHOD_UNSUPPORTED
        }
        let release = frame.release
        // A raw address is Sendable; the lease keeps the pointer valid until
        // the callback fires.
        let userdata = UInt(bitPattern: frame.release_userdata)
        let metal = MetalFrame(
            texture: texture,
            sequenceNumber: frame.sequence,
            width: Int(frame.width),
            height: Int(frame.height),
            colorState: Self.colorState(from: frame.color)
        ) {
            release?(UnsafeMutableRawPointer(bitPattern: userdata))
        }
        lock.lock()
        latest = .metal(metal)
        scheduleDeliveryLocked()
        lock.unlock()
        return 0
    }

    private func scheduleDeliveryLocked() {
        guard !retired, !deliveryScheduled else { return }
        deliveryScheduled = true
        DispatchQueue.main.async { [weak self] in self?.deliver() }
    }

    @MainActor private func deliver() {
        lock.lock()
        defer { lock.unlock() }
        deliveryScheduled = false
        guard !retired else { return }
        if let latest {
            let size = latest.pixelSize
            do {
                try renderer.present(latest)
                let pixels = NSSize(width: size.width, height: size.height)
                inputView?.frameSize = pixels
                if pixels != windowPixelSize {
                    windowPixelSize = pixels
                    fitWindowToContent()
                }
            } catch {
                renderer.clear()
                inputView?.frameSize = .zero
            }
        } else {
            renderer.clear()
            inputView?.frameSize = .zero
        }
    }

    @MainActor func show(input: VMInput, mode: VMDisplayConfiguration, screenID: UInt32,
                         onScreenChange: @escaping @MainActor @Sendable (HostDisplayCapabilities) -> Void,
                         onActivity: @escaping (Bool) -> Void, onClose: @escaping () -> Void) {
        let reused = window != nil
        preserveWindowFrame = reused
        let screen = NSScreen.screens.first { HostDisplayCapabilities.screenID($0) == screenID }
        let window = window ?? VMWindow(contentRect: NSRect(x: 0, y: 0, width: 1024, height: 640), screen: screen)
        let view = VMInputView(renderer: renderer, input: input, onActivity: onActivity)
        inputView = view
        window.contentView = view
        window.onClose = { [weak self] in
            self?.stopInput()
            onClose()
        }
        window.onScreenChange = { [weak self, weak window] in
            guard let self, let window else { return }
            guard let screen = window.screen ?? NSScreen.main else { return }
            self.renderer.requestRedraw()
            guard let id = HostDisplayCapabilities.screenID(screen),
                  let capabilities = HostDisplayCapabilities.detect(displayID: id) else { return }
            self.sampledCapabilities = capabilities
            onScreenChange(capabilities)
            self.hdrTimer?.invalidate(); self.hdrTimer = nil
            if capabilities.hdrSupported {
                let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in
                    Task { @MainActor in
                        guard let self, !self.retired, let window = self.window, window.isVisible,
                              let screen = window.screen, let id = HostDisplayCapabilities.screenID(screen),
                              let next = HostDisplayCapabilities.detect(displayID: id), next != self.sampledCapabilities else { return }
                        self.sampledCapabilities = next
                        onScreenChange(next)
                    }
                }
                self.hdrTimer = timer
                RunLoop.main.add(timer, forMode: .common)
            }
            self.fitWindowToContent()
        }
        window.onExitFullScreen = { [weak self] in
            self?.fitWindowToContent()
        }
        windowPixelSize = NSSize(width: mode.width, height: mode.height)
        self.window = window
        setWorkContextWarning(workContextWarning)
        updateGameHUDVisibility()
        if !reused {
            fitWindowToContent()
            window.center()
            window.makeKeyAndOrderFront(nil)
        }
        window.makeFirstResponder(view)
        screenObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in
            DispatchQueue.main.async { self?.window?.onScreenChange?() }
        }
        window.onScreenChange?()
        view.updateActivity()
    }

    deinit {
        if let screenObserver { NotificationCenter.default.removeObserver(screenObserver) }
    }

    @MainActor private func fitWindowToContent() {
        guard !preserveWindowFrame, let window, !window.isFullScreen else { return }
        // Moving the window can synchronously change its screen, which calls back
        // here; refit once afterwards for the new screen instead of recursing.
        guard !fittingWindow else { refitWindow = true; return }
        fittingWindow = true
        VMWindowGeometry.fit(window, pixels: windowPixelSize)
        fittingWindow = false
        if refitWindow {
            refitWindow = false
            DispatchQueue.main.async { [weak self] in self?.fitWindowToContent() }
        }
        let size = window.contentRect(forFrameRect: window.frame).size
        report("BRINEWELL display window width=\(size.width) height=\(size.height) pixels=\(Int(windowPixelSize.width))x\(Int(windowPixelSize.height))")
    }

    @MainActor func stopInput() {
        hdrTimer?.invalidate(); hdrTimer = nil
        inputView?.stop()
    }

    @MainActor func setFullScreen(_ enabled: Bool) throws {
        guard let window else { throw RuntimeError("VM window unavailable") }
        window.setFullScreen(enabled)
    }

    @MainActor func setGameHUD(level: GameHUDLevel, position: GameHUDPosition) {
        gameHUDLevel = level
        gameHUDPosition = position
        gameHUDState.setEnabled(level != .off)
        updateGameHUDVisibility()
        placeGameHUD()
        refreshGameHUD()
    }

    @MainActor func receiveGameFrame(_ sample: GameFrameSample) {
        gameHUDState.receive(sample, at: DispatchTime.now().uptimeNanoseconds)
    }

    @MainActor private func updateGameHUDVisibility() {
        guard gameHUDState.enabled, inputView != nil, let content = window?.contentView else {
            gameHUDTimer?.invalidate()
            gameHUDTimer = nil
            gameHUD?.removeFromSuperview()
            gameHUD = nil
            NSLayoutConstraint.deactivate(gameHUDPlacement)
            gameHUDPlacement = []
            reportedGameSource = nil
            return
        }
        guard gameHUD == nil else { return }
        let hud = GamePerformanceHUD(frame: .zero)
        content.addSubview(hud)
        gameHUD = hud
        placeGameHUD()
        NSLayoutConstraint.activate([
            hud.widthAnchor.constraint(lessThanOrEqualTo: content.widthAnchor, constant: -32),
            hud.heightAnchor.constraint(lessThanOrEqualTo: content.heightAnchor, constant: -32),
        ])
        let timer = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshGameHUD() }
        }
        gameHUDTimer = timer
        RunLoop.main.add(timer, forMode: .common)
        refreshGameHUD()
    }

    @MainActor private func placeGameHUD() {
        NSLayoutConstraint.deactivate(gameHUDPlacement)
        gameHUDPlacement = []
        guard let hud = gameHUD, let content = window?.contentView else { return }
        let left = gameHUDPosition == .topLeft || gameHUDPosition == .bottomLeft
        let top = gameHUDPosition == .topLeft || gameHUDPosition == .topRight
        let bottom = warningBanner?.topAnchor ?? content.bottomAnchor
        let bottomInset: CGFloat = warningBanner == nil ? -16 : -12
        gameHUDPlacement = [
            left ? hud.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16)
                 : hud.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            top ? hud.topAnchor.constraint(equalTo: content.topAnchor, constant: 16)
                : hud.bottomAnchor.constraint(equalTo: bottom, constant: bottomInset),
            hud.topAnchor.constraint(greaterThanOrEqualTo: content.topAnchor, constant: 16),
            hud.bottomAnchor.constraint(lessThanOrEqualTo: bottom, constant: bottomInset),
        ]
        NSLayoutConstraint.activate(gameHUDPlacement)
    }

    @MainActor private func refreshGameHUD() {
        guard let gameHUD else { return }
        let game = gameHUDState.snapshot(at: DispatchTime.now().uptimeNanoseconds)
        let sample = game?.sample
        gameHUD.update(.make(level: gameHUDLevel, game: game,
                             hardware: gameHUDLevel.rawValue >= 2 ? hardwareSample?() : nil,
                             refreshRate: displayRefreshRate))
        if let sample, sample.source != reportedGameSource {
            report("BRINEWELL game-hud source backend=\(sample.source.backend) pid=\(sample.source.processID) stream=\(sample.source.streamID) size=\(sample.width)x\(sample.height) fps=\(String(format: "%.1f", sample.framesPerSecond)) frame_ms=\(String(format: "%.2f", sample.frameTimeMS))")
        }
        reportedGameSource = sample?.source
    }

    @MainActor func setWorkContextWarning(_ message: String?) {
        if message == workContextWarning && warningBanner != nil { return }
        defer { placeGameHUD() }
        workContextWarning = message
        warningBanner?.removeFromSuperview()
        warningBanner = nil
        guard let message, let content = window?.contentView else { return }
        let banner = WorkContextBanner(message: message)
        content.addSubview(banner)
        let width = banner.widthAnchor.constraint(equalToConstant: 660)
        width.priority = .defaultHigh
        NSLayoutConstraint.activate([
            width,
            banner.leadingAnchor.constraint(greaterThanOrEqualTo: content.leadingAnchor, constant: 24),
            banner.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -24),
            banner.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            banner.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -24),
        ])
        warningBanner = banner
    }

    /// Called after the VMM has stopped (or startup failed), while AppKit and
    /// the launcher remain alive. Closing the window also ends its full screen.
    @MainActor func detachForRestart(onShutdownRequested: @escaping () -> Void) -> VMWindow? {
        let preserved = window
        teardown(closeWindow: false)
        guard let preserved else { return nil }
        let status = NSView(frame: preserved.contentView?.bounds ?? .zero)
        status.wantsLayer = true
        status.layer?.backgroundColor = NSColor.black.cgColor
        let label = NSTextField(labelWithString: "Restarting SteamOS…")
        label.textColor = .white
        label.font = .systemFont(ofSize: 20)
        label.translatesAutoresizingMaskIntoConstraints = false
        status.addSubview(label)
        NSLayoutConstraint.activate([label.centerXAnchor.constraint(equalTo: status.centerXAnchor), label.centerYAnchor.constraint(equalTo: status.centerYAnchor)])
        preserved.contentView = status
        preserved.onClose = onShutdownRequested
        return preserved
    }

    @MainActor func close() { teardown(closeWindow: true) }

    @MainActor private func teardown(closeWindow: Bool) {
        lock.lock()
        retired = true
        latest = nil
        slots.removeAll()
        allocated.removeAll()
        lock.unlock()
        stopInput()
        if let screenObserver {
            NotificationCenter.default.removeObserver(screenObserver)
            self.screenObserver = nil
        }
        window?.onClose = nil
        window?.onScreenChange = nil
        window?.onExitFullScreen = nil
        setWorkContextWarning(nil)
        setGameHUD(level: .off, position: gameHUDPosition)
        if closeWindow { window?.close() }
        window = nil
        inputView = nil
        lock.lock()
        latest = nil
        lock.unlock()
        renderer.clear()
    }

    @MainActor func dump(to path: String) throws {
        lock.lock()
        let frame = latest
        lock.unlock()
        guard let frame else { throw DumpError.noFrame }
        defer { withExtendedLifetime(frame) {} }
        let snapshot: FrameSnapshot
        switch frame {
        case .surface(let surfaceFrame):
            snapshot = try readSurface(surfaceFrame.surface)
        case .metal:
            snapshot = try renderer.readback()
        }
        try writePNG(snapshot, to: path)
    }

    /// Copies a CPU-visible IOSurface into a snapshot (diagnostic path only).
    private func readSurface(_ surface: IOSurfaceRef) throws -> FrameSnapshot {
        let width = IOSurfaceGetWidth(surface)
        let height = IOSurfaceGetHeight(surface)
        guard width > 0, height > 0 else { throw DumpError.encoding }
        guard IOSurfaceLock(surface, .readOnly, nil) == 0 else { throw DumpError.encoding }
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let stride = IOSurfaceGetBytesPerRow(surface)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { output in
            guard let destination = output.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            for y in 0..<height {
                destination.advanced(by: y * width * 4)
                    .update(from: base.advanced(by: y * stride), count: width * 4)
            }
        }
        return FrameSnapshot(width: width, height: height, swapRedBlue: true, pixels: pixels)
    }

    private func writePNG(_ snapshot: FrameSnapshot, to path: String) throws {
        guard let image = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: snapshot.width,
                                           pixelsHigh: snapshot.height, bitsPerSample: 8,
                                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                           colorSpaceName: .deviceRGB,
                                           bitmapFormat: .alphaNonpremultiplied,
                                           bytesPerRow: snapshot.width * 4, bitsPerPixel: 32),
              let output = image.bitmapData else { throw DumpError.encoding }
        snapshot.pixels.withUnsafeBufferPointer { source in
            guard let input = source.baseAddress else { return }
            for y in 0..<snapshot.height {
                for x in 0..<snapshot.width {
                    let src = y * snapshot.width * 4 + x * 4
                    let dst = src
                    output[dst] = input[src + (snapshot.swapRedBlue ? 2 : 0)]
                    output[dst + 1] = input[src + 1]
                    output[dst + 2] = input[src + (snapshot.swapRedBlue ? 0 : 2)]
                    output[dst + 3] = input[src + 3]
                }
            }
        }
        guard let png = image.representation(using: .png, properties: [:]) else { throw DumpError.encoding }
        try png.write(to: URL(fileURLWithPath: path))
    }

    private enum DumpError: LocalizedError {
        case noFrame, encoding
        var errorDescription: String? {
            switch self {
            case .noFrame: return "No guest frame has been presented."
            case .encoding: return "Unable to encode the guest frame as PNG."
            }
        }
    }
}

/// The notice remains visible in the VM's full-screen Space but never takes
/// keyboard focus or intercepts mouse/controller input intended for the guest.
@MainActor
private final class WorkContextBanner: NSVisualEffectView {
    init(message: String) {
        super.init(frame: .zero)
        identifier = NSUserInterfaceItemIdentifier("work-context-warning")
        translatesAutoresizingMaskIntoConstraints = false
        material = .hudWindow
        blendingMode = .withinWindow
        state = .active
        wantsLayer = true
        layer?.cornerRadius = 8
        let label = NSTextField(wrappingLabelWithString: message)
        label.translatesAutoresizingMaskIntoConstraints = false
        label.font = .systemFont(ofSize: 13)
        label.textColor = .labelColor
        addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
        ])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}
