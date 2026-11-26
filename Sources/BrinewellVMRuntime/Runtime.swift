import AppKit
import BrinewellCore
import CKrun
import Darwin
import Foundation

@discardableResult
func krunCheck(_ operation: String, _ result: Int32) throws -> Int32 {
    guard result >= 0 else {
        throw RuntimeError("\(operation) failed: \(result) (\(String(cString: strerror(-result))))")
    }
    return result
}

func sharedFolderKernelArguments(_ folder: VMSharedFolder) -> String {
    let id = VMSharedFolder.shareIdentifier(forResolvedPath: folder.path)
    let labelHex = VMSharedFolder.displayLabelHex(forResolvedPath: folder.path)
    return " brinewell.share=\(folder.readOnly ? "ro" : "rw") brinewell.share_id=\(id) brinewell.share_label_hex=\(labelHex)"
}

public enum RuntimeExitOutcome: Equatable, Sendable {
    case poweroff(status: Int32)
    case reset(status: Int32)
    case failed(code: Int32)
}

private final class ShutdownEvent: @unchecked Sendable {
    private let lock = NSLock()
    private var fd: Int32 = -1

    func install(_ fd: Int32) { lock.lock(); defer { lock.unlock() }; self.fd = fd }
    func close() {
        lock.lock(); defer { lock.unlock() }
        if fd >= 0 { Darwin.close(fd); fd = -1 }
    }
    func signal() {
        lock.lock(); defer { lock.unlock() }
        guard fd >= 0 else { return }
        var one: UInt64 = 1
        if Darwin.write(fd, &one, MemoryLayout<UInt64>.size) != MemoryLayout<UInt64>.size {
            report("BRINEWELL control error cannot send guest shutdown key: \(String(cString: strerror(errno)))")
        }
    }
    deinit { close() }
}

public struct VMAdminRuntimeUpdate: Equatable, Sendable {
    public let connected: Bool
    public let snapshot: VMAdminSnapshot?
    public let forwardState: VMSSHForwardState
    public let sshPort: Int
}

@MainActor
public final class Runtime {
    private static var logInitialized = false
    private let options: RuntimeOptions
    private let display: VMDisplay
    private let ports: VMPorts
    private let input: VMInput
    private let network: RuntimeNetwork
    private let workContext = WorkContextService()
    private let adminQueue = DispatchQueue(label: "brinewell.admin.forwarder")
    private var adminInstance: String?
    private var adminSequence = 0
    private var adminConnected = false
    private var adminSnapshot: VMAdminSnapshot?
    private var adminForward: VMSSHForwardState = .unpublished
    private var adminGeneration = 0
    private var desiredForwardExposed = false
    private var forwardSyncInFlight = false
    private var forwardResyncNeeded = false
    private var forwardRetryWorkItem: DispatchWorkItem?
    private var forwardRetryCount = 0
    private var adminProbe: (id: String, instance: String, seq: Int)?
    private var adminProbeTimeout: DispatchWorkItem?
    private var adminPending: (id: String, completion: @Sendable (Result<VMAdminSnapshot, VMAdminErrorCode>) -> Void)?
    private var adminTimeout: DispatchWorkItem?
    private var adminWatchdog: DispatchWorkItem?
    public var onAdminUpdate: (@Sendable (VMAdminRuntimeUpdate) -> Void)?
    private var context: UInt32?
    private let shutdownEvent = ShutdownEvent()
    private var diskFD: Int32 = -1
    private var stopping = false
    private var started = false
    private var ready = false
    private var currentDisplayMode: VMDisplayConfiguration
    private let initialScreenID: UInt32
    private var hdrWork: DispatchWorkItem?
    private var guestHDRCapabilities: HostDisplayCapabilities?
    private var edidCapabilities: HostDisplayCapabilities?
    private var hostCapabilities: HostDisplayCapabilities

    /// Called on the main actor after boot-scoped services have stopped.
    public var onOutcome: ((RuntimeExitOutcome) -> Void)?
    public var onShutdownRequested: (() -> Void)?
    public var shouldPreserveWindowOnReset: (() -> Bool)?
    private var restartWindow: VMWindow?
    public func detachWindowForRestart() -> VMWindow? {
        defer { restartWindow = nil }
        return restartWindow
    }
    public var onExit: (@Sendable (Int32) -> Void)?
    public var onGraphicsApplied: ((GraphicsBackend) -> Void)? {
        get { ports.onGraphicsApplied }
        set { ports.onGraphicsApplied = newValue }
    }

    /// Takes ownership of options.diskFD only after initialization succeeds.
    public init(options: RuntimeOptions, existingWindow: VMWindow? = nil) throws {
        self.options = options
        guard let capabilities = HostDisplayCapabilities.detect(displayID: options.hostDisplayID) else {
            throw RuntimeError(String(localized: "Could not read the available display modes for the host display."))
        }
        try capabilities.validate(options.display)
        currentDisplayMode = options.display
        initialScreenID = capabilities.id
        hostCapabilities = capabilities
        display = try VMDisplay(existingWindow: existingWindow)
        display.displayRefreshRate = Double(options.display.refreshRate)
        display.setGameHUD(level: options.gameHUDLevel, position: options.gameHUDPosition)
        network = try RuntimeNetwork(directory: options.runDirectory)
        let indexURL = options.resources.deletingLastPathComponent().appendingPathComponent("steam-graphics-index.json")
        let index = SteamGraphicsIndex.loadIfPresent(from: indexURL)
            ?? SteamGraphicsIndex.loadIfPresent(from: Bundle.main.url(forResource: "steam-graphics-index", withExtension: "json"))
        ports = try VMPorts(runDirectory: options.runDirectory, graphicsResolver: PCGamingWikiResolver(bundledIndex: index),
                            availableBackends: options.availableBackends)
        display.hardwareSample = { [weak ports] in ports?.hardwareSample() }
        do {
            input = try VMInput()
        } catch {
            ports.close()
            throw error
        }
        ports.onGraphicsRequest = { [weak self] mode in try self?.setGraphicsBackend(mode) }
        ports.onAdminMessage = { [weak self] message in self?.receiveAdmin(message) }
        ports.onAdminDisconnected = { [weak self] in self?.disconnectAdmin() }
        diskFD = options.diskFD
        workContext.onReport = { [weak display] line in
            display?.setWorkContextWarning(WorkContextNotice.warning(for: line))
        }
    }

    public func start() async throws {
        do {
            try Task.checkCancellation()
            guard !stopping else { throw CancellationError() }
            let sanitizedPrefixes = ["NPT_", "DMN_", "STEAMAC_", "BRINEWELL_WMTR", "WMTR_"]
            for key in ProcessInfo.processInfo.environment.keys
                where sanitizedPrefixes.contains(where: { key.hasPrefix($0) }) {
                unsetenv(key)
            }
            for key in ["D3DMETAL_FRAMEWORK_PATH", "RENDER_SERVER_EXEC_PATH"] { unsetenv(key) }
            setenv("RENDER_SERVER_EXEC_PATH", options.renderer.path, 1)
            setenv("VKR_VULKAN_DRIVER", options.vulkanDriver.path, 1)
            setenv("VKR_GPU_BUDGET_MIB", "4096", 1)
            // Host-only trusted allowlists, set only for validated supports so
            // a guest tag can never steer a worker to an unvalidated adapter.
            if let support = options.neptuneD3DMetal {
                setenv("NPT_BRINEWELL_D3DMETAL_ADAPTER", support.adapter.path, 1)
                if case .d3dmetal(let framework, _) = support.backend {
                    setenv("D3DMETAL_FRAMEWORK_PATH", framework.path, 1)
                }
                // The arm64 process reporting the capset cannot probe an x86_64 library.
                setenv("NPT_CAPSET_D3D12", "1", 1)
                setenv("DMN_LOG", "warn", 1)
            }
            if let support = options.neptuneMullion {
                setenv("NPT_BRINEWELL_MULLION_ADAPTER", support.adapter.path, 1)
                setenv("MULLION_LOG_PATH", "none", 0)
            }
            if let neptune = options.neptune {
                setenv("NPT_D3D11_LIBRARY_PATH", neptune.adapter.path, 1)
                setenv("NPT_DXGI_LIBRARY_PATH", neptune.adapter.path, 1)
                setenv("NPT_D3D12_LIBRARY_PATH", neptune.adapter.path, 1)
                let settings = "adapter=\(neptune.adapter.path) aperture_mib=\(neptune.apertureMiB) multi_ring=\(neptune.multiRing ? 1 : 0)"
                switch neptune.backend {
                case .d3dmetal(let framework, let version):
                    setenv("NPT_BACKEND", "d3dmetal", 1)
                    report("BRINEWELL neptune enabled framework=\(framework.path) version=\(version ?? "unknown") \(settings)")
                case .mullion:
                    setenv("NPT_BACKEND", "mullion", 1)
                    report("BRINEWELL neptune enabled backend=mullion \(settings)")
                }
            }
            await workContext.start(renderer: options.renderer, supportsBackend: options.neptune != nil)
            try Task.checkCancellation()
            guard !stopping else { throw CancellationError() }
            try network.start(binary: options.resources.appendingPathComponent("gvproxy").path,
                              sshPort: options.sshPort, configuration: options.network)
            raiseFileLimit()
            if !Self.logInitialized {
                try krunCheck("krun_init_log", krun_init_log(KRUN_LOG_TARGET_DEFAULT, UInt32(KRUN_LOG_LEVEL_WARN), UInt32(KRUN_LOG_STYLE_AUTO), UInt32(KRUN_LOG_OPTION_NO_ENV)))
                Self.logInitialized = true
            }
            let ctx = UInt32(try krunCheck("krun_create_ctx", krun_create_ctx()))
            context = ctx
            try krunCheck("krun_set_vm_config", krun_set_vm_config(ctx, options.cpus, options.memoryMiB))
            try krunCheck("krun_disable_implicit_vsock", krun_disable_implicit_vsock(ctx))
            try ports.attach(context: ctx)
            try input.attach(context: ctx)
            let kernel = options.resources.appendingPathComponent("Image").path
            let initrd = options.resources.appendingPathComponent("initramfs.cpio.gz").path
            let telemetrySampler = options.resources.appendingPathComponent("macmon")
            var cmdline = "console=hvc0 loglevel=4 rootwait brinewell.steam_client=deck"
            cmdline += HostCPUInfo.kernelArguments(sampler: telemetrySampler)
            // Boot-time seed only; the live authority is the fx.graphics port.
            cmdline += " brinewell.graphics=\(options.graphicsBackend.rawValue)"
            if let neptune = options.neptune {
                cmdline += " brinewell.gfx=neptune"
                if case .mullion = neptune.backend { cmdline += " brinewell.neptune.backend=mullion" }
                if neptune.multiRing { cmdline += " brinewell.neptune.multi_ring=1" }
            }
            if let folder = options.sharedFolder {
                // Present the share as owned by steamos, with a five-second attribute cache.
                try krunCheck("krun_add_virtiofs5", krun_add_virtiofs5(
                    ctx, "brinewell-share", folder.path, 0, folder.readOnly, 1000, 1000, 5
                ))
                cmdline += sharedFolderKernelArguments(folder)
            }
            if let shrinkHomeKiB = options.shrinkHomeKiB {
                cmdline += " brinewell.shrink_home=\(shrinkHomeKiB)"
            }
            try krunCheck("krun_set_kernel", krun_set_kernel(ctx, kernel, 0, initrd, cmdline))
            guard diskFDMatchesPath(diskFD, path: options.disk) else { throw RuntimeError("disk was replaced before attachment") }
            try krunCheck("krun_add_disk_raw_fd(vda)", krun_add_disk_raw_fd(ctx, "vda", diskFD, false))
            try krunCheck("krun_add_disk2(vdb)", krun_add_disk2(ctx, "vdb", options.resources.appendingPathComponent("brinewell-layer.img").path, 0, true))
            if krun_has_feature(UInt64(KRUN_FEATURE_SND)) == 1 {
                try krunCheck("krun_set_snd_device", krun_set_snd_device(ctx, true))
            }
            let venus: UInt32 = 1 << 6
            let noVirgl: UInt32 = 1 << 7
            let neptuneFlag: UInt32 = 1 << 15
            let anyNeptune = options.neptuneD3DMetal != nil || options.neptuneMullion != nil
            let gpuFlags = venus | noVirgl | (anyNeptune ? neptuneFlag : 0)
            let apertureMiB = options.neptune?.apertureMiB ?? 8192
            try krunCheck("krun_set_gpu_options2", krun_set_gpu_options2(ctx, gpuFlags, UInt64(apertureMiB) << 20))
            let mode = options.display
            let displayID = UInt32(try krunCheck("krun_add_display", krun_add_display(ctx, UInt32(mode.width), UInt32(mode.height))))
            try krunCheck("krun_display_set_hdr_capabilities", krun_display_set_hdr_capabilities(
                ctx, displayID, hostCapabilities.hdrSupported, hostCapabilities.maxLuminanceNits,
                hostCapabilities.maxFrameAverageLuminanceNits, hostCapabilities.minLuminance10000ths))
            edidCapabilities = hostCapabilities
            sendHDR(hostCapabilities)
            try krunCheck("krun_display_set_refresh_rate", krun_display_set_refresh_rate(ctx, displayID, UInt32(mode.refreshRate)))
            try krunCheck("krun_display_set_physical_size", krun_display_set_physical_size(ctx, displayID, 339, 212))
            report("BRINEWELL display width=\(mode.width) height=\(mode.height) refresh_rate=\(mode.refreshRate)")
            var backend = display.makeBackend()
            try krunCheck("krun_set_display_backend", krun_set_display_backend(ctx, &backend, MemoryLayout<krun_display_backend>.size))
            try network.attach(context: ctx)
            let shutdownEventFD = try krunCheck("krun_get_shutdown_eventfd", krun_get_shutdown_eventfd(ctx))
            _ = fcntl(shutdownEventFD, F_SETNOSIGPIPE, 1)
            shutdownEvent.install(shutdownEventFD)
            try ports.start(onReady: { [weak self] in
                guard let self, !self.stopping, !self.ready else { return }
                self.ready = true
                report("BRINEWELL state running")
            }, onShutdown: { [weak self] in self?.requestShutdown() }, onFrameDump: { [weak self] path in
                guard let self else { throw RuntimeError("runtime unavailable") }
                try self.display.dump(to: path)
                self.reportScanoutStats()
            }, onPointerFocus: { [weak self] line in
                self?.input.updateGuestFocus(line)
            }, onFullScreen: { [weak self] enabled in
                guard let self, !self.stopping else { throw RuntimeError("runtime stopping") }
                try self.display.setFullScreen(enabled)
            }, onGameFrame: { [weak self] sample in
                self?.display.receiveGameFrame(sample)
            })
            // Desired mode is sent as soon as the guest control service is up.
            try ports.setGraphicsBackend(options.graphicsBackend)
            ports.startTelemetry(sampler: telemetrySampler)
            display.show(input: input, mode: currentDisplayMode, screenID: initialScreenID,
                         onScreenChange: { [weak self] screen in self?.hostScreenChanged(screen) },
                         onActivity: { [weak self] active in
                self?.ports.setInputActive(active)
                self?.workContext.setActive(active)
            }, onClose: { [weak self] in self?.requestShutdown() })
            report("BRINEWELL state booting")
            started = true
            let thread = Thread { [self] in
                var result = krun_run_result()
                let code = krun_run(ctx, &result)
                if code < 0 { _ = krun_free_ctx(ctx) }
                self.shutdownEvent.close()
                let outcome: RuntimeExitOutcome
                if code != 0 { outcome = .failed(code: code) }
                else if result.reason == UInt32(KRUN_EXIT_REASON_GUEST_POWEROFF) { outcome = .poweroff(status: result.guest_status) }
                else if result.reason == UInt32(KRUN_EXIT_REASON_GUEST_RESET) { outcome = .reset(status: result.guest_status) }
                else { outcome = .failed(code: -1) }
                Task { @MainActor in
                    self.stopping = true
                    self.hdrWork?.cancel(); self.hdrWork = nil
                    self.started = false
                    self.workContext.stop()
                    self.context = nil // krun_run consumes the context.
                    self.shutdownEvent.close()
                    self.ports.close()
                    if case .reset = outcome, self.shouldPreserveWindowOnReset?() == true {
                        let request = self.onShutdownRequested ?? { [weak self] in self?.shutdown() }
                        self.restartWindow = self.display.detachForRestart(onShutdownRequested: request)
                    } else { self.display.close() }
                    self.input.setActive(false)
                    self.network.stop()
                    self.closeDisk()
                    if let onOutcome = self.onOutcome { onOutcome(outcome) }
                    else if let onExit = self.onExit {
                        switch outcome {
                        case .poweroff(let status), .reset(let status): onExit(status)
                        case .failed(let code): onExit(code)
                        }
                    } else {
                        switch outcome {
                        case .poweroff(let status), .reset(let status): Darwin.exit(status)
                        case .failed: Darwin.exit(1)
                        }
                    }
                }
            }
            thread.name = "brinewell-vmm"
            thread.stackSize = 16 << 20
            thread.qualityOfService = .userInteractive
            thread.start()
        } catch {
            stopping = true
            hdrWork?.cancel(); hdrWork = nil
            shutdownEvent.close()
            workContext.stop()
            display.close()
            input.setActive(false)
            ports.setInputActive(false)
            if let context { _ = krun_free_ctx(context); self.context = nil }
            ports.close()
            network.stop()
            closeDisk()
            throw error
        }
    }

    private func publishAdminUpdate() {
        onAdminUpdate?(VMAdminRuntimeUpdate(connected: adminConnected, snapshot: adminSnapshot,
                                            forwardState: adminForward, sshPort: options.sshPort))
    }

    private func receiveAdmin(_ message: VMAdminGuestMessage) {
        guard !stopping else { return }
        let snapshot: VMAdminSnapshot
        switch message {
        case .hello(let version, let instance, _, let seq, let value):
            guard version == 1, !instance.isEmpty, seq > 0 else { return }
            if adminInstance != instance { ports.clearAdminPending(); completeAdmin(.failure(.disconnected)) }
            clearAdminProbe()
            adminInstance = instance; adminSequence = seq; adminConnected = true
            resetAdminWatchdog(); snapshot = value
        case .state(let instance, let seq, let value):
            guard !instance.isEmpty, seq > 0 else { return }
            if adminInstance != instance { ports.clearAdminPending(); completeAdmin(.failure(.disconnected)) }
            else if adminConnected && (seq <= adminSequence || seq - adminSequence > 1) { requestAdminRefresh() }
            clearAdminProbe()
            adminInstance = instance; adminSequence = seq; adminConnected = true
            resetAdminWatchdog(); snapshot = value
        case .reply(let instance, let id, let ok, let code, let value):
            guard !instance.isEmpty else { return }
            if !adminConnected {
                guard let probe = adminProbe, probe.id == id, probe.instance == instance, ok else { return }
                adminInstance = instance; adminSequence = probe.seq; adminConnected = true
                clearAdminProbe()
            } else {
                guard adminInstance == instance else { return }
                if adminProbe?.id == id { clearAdminProbe() }
                if adminPending?.id == id { completeAdmin(ok ? .success(value) : .failure(code)) }
            }
            resetAdminWatchdog(); snapshot = value
        case .heartbeat(let instance, let seq):
            guard !instance.isEmpty, seq > 0 else { return }
            if !adminConnected || adminInstance != instance {
                if adminConnected { disconnectAdmin() }
                probeAdmin(instance: instance, seq: seq)
            } else {
                resetAdminWatchdog()
                if seq != adminSequence { requestAdminRefresh() }
            }
            return
        case .bye(let instance):
            if adminConnected && adminInstance == instance { disconnectAdmin() }
            return
        }
        adminSnapshot = snapshot
        synchronizeAdminForward(desired: snapshot.ssh.shouldExposeHostForward)
        publishAdminUpdate()
    }

    private func clearAdminProbe() {
        if let id = adminProbe?.id { ports.cancelAdminPending(requestID: id) }
        adminProbe = nil
        adminProbeTimeout?.cancel(); adminProbeTimeout = nil
    }

    private func probeAdmin(instance: String, seq: Int) {
        guard adminProbe == nil, started, !stopping else { return }
        let id = UUID().uuidString
        adminProbe = (id, instance, seq)
        guard sendAdmin(.getSnapshot(id: id), requestID: id) else { clearAdminProbe(); return }
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.adminProbe?.id == id else { return }
            self.clearAdminProbe()
        }
        adminProbeTimeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 10, execute: timeout)
    }

    private func synchronizeAdminForward(desired: Bool) {
        let changed = desiredForwardExposed != desired
        desiredForwardExposed = desired
        if changed {
            forwardRetryWorkItem?.cancel(); forwardRetryWorkItem = nil
            forwardRetryCount = 0
        }
        if forwardSyncInFlight {
            forwardResyncNeeded = true
            return
        }
        if forwardRetryWorkItem != nil { return }
        runAdminForwardSync()
    }

    private func runAdminForwardSync() {
        guard !forwardSyncInFlight else { return }
        let desired = desiredForwardExposed
        if desired && adminForward != .published { adminForward = .publishing }
        if !desired && adminForward != .unpublished { adminForward = .unpublishing }
        publishAdminUpdate()
        forwardSyncInFlight = true
        forwardResyncNeeded = false
        let generation = adminGeneration
        let client = GVProxyForwarderClient(socketPath: network.apiSocketPath)
        let port = options.sshPort
        adminQueue.async { [weak self] in
            let state = client.syncSSHForward(desiredExposed: desired, localPort: port)
            Task { @MainActor [weak self] in
                guard let self, self.adminGeneration == generation else { return }
                self.forwardSyncInFlight = false
                if self.forwardResyncNeeded {
                    self.runAdminForwardSync()
                    return
                }
                switch state {
                case .conflict(let message) where !desired:
                    self.adminForward = .error(message)
                default: self.adminForward = state
                }
                self.publishAdminUpdate()
                switch state {
                case .conflict, .error:
                    guard self.forwardRetryCount < 5 else { return }
                    let delay = (desired ? 5 : 1) * (1 << self.forwardRetryCount)
                    self.forwardRetryCount += 1
                    let retry = DispatchWorkItem { [weak self] in
                        guard let self else { return }
                        self.forwardRetryWorkItem = nil
                        self.runAdminForwardSync()
                    }
                    self.forwardRetryWorkItem = retry
                    DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(delay), execute: retry)
                default: self.forwardRetryCount = 0
                }
            }
        }
    }

    private func resetAdminWatchdog() {
        adminWatchdog?.cancel()
        let watchdog = DispatchWorkItem { [weak self] in self?.disconnectAdmin() }
        adminWatchdog = watchdog
        DispatchQueue.main.asyncAfter(deadline: .now() + 15, execute: watchdog)
    }

    private func disconnectAdmin() {
        adminWatchdog?.cancel(); adminWatchdog = nil
        clearAdminProbe()
        ports.clearAdminPending()
        guard adminConnected || adminPending != nil else { return }
        adminConnected = false; adminInstance = nil; adminSequence = 0
        completeAdmin(.failure(.disconnected))
        synchronizeAdminForward(desired: false)
        publishAdminUpdate()
    }

    private func completeAdmin(_ result: Result<VMAdminSnapshot, VMAdminErrorCode>) {
        adminTimeout?.cancel(); adminTimeout = nil
        if let id = adminPending?.id { ports.cancelAdminPending(requestID: id) }
        let completion = adminPending?.completion
        adminPending = nil
        completion?(result)
    }

    private func sendAdmin(_ request: VMAdminHostRequest, requestID: String? = nil) -> Bool {
        guard let data = try? JSONEncoder().encode(request) else { return false }
        return ports.sendAdminLine(data, requestID: requestID)
    }

    public func requestAdminRefresh() {
        guard adminConnected, !stopping else { return }
        _ = sendAdmin(.getSnapshot(id: UUID().uuidString))
    }

    private func sendAdminWrite(id: String, request: VMAdminHostRequest,
                                completion: @escaping @Sendable (Result<VMAdminSnapshot, VMAdminErrorCode>) -> Void) {
        guard adminConnected, adminInstance != nil, !stopping else { completion(.failure(.notConnected)); return }
        guard adminPending == nil else { completion(.failure(.busy)); return }
        adminPending = (id, completion)
        guard sendAdmin(request, requestID: id) else { completeAdmin(.failure(.disconnected)); return }
        let timeout = DispatchWorkItem { [weak self] in
            guard let self, self.adminPending?.id == id else { return }
            self.completeAdmin(.failure(.timeout))
        }
        adminTimeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 10, execute: timeout)
    }

    public func setGuestSSH(enabled: Bool, completion: @escaping @Sendable (Result<VMAdminSnapshot, VMAdminErrorCode>) -> Void) {
        guard adminConnected, let instance = adminInstance, !stopping else { completion(.failure(.notConnected)); return }
        let id = UUID().uuidString
        sendAdminWrite(id: id, request: .setSSH(id: id, enabled: enabled, targetInstance: instance), completion: completion)
    }

    public func setGuestSudoPassword(_ password: String, confirmUnlock: Bool,
                                     completion: @escaping @Sendable (Result<VMAdminSnapshot, VMAdminErrorCode>) -> Void) {
        guard VMPasswordValidation.validate(password: password, confirmation: password) == nil else {
            completion(.failure(.invalidPassword)); return
        }
        guard adminConnected, let instance = adminInstance, !stopping else { completion(.failure(.notConnected)); return }
        let id = UUID().uuidString
        sendAdminWrite(id: id, request: .setPassword(id: id, password: password, confirmUnlock: confirmUnlock, targetInstance: instance), completion: completion)
    }

    private func requestShutdown() {
        if let onShutdownRequested { onShutdownRequested() } else { shutdown() }
    }

    private func closeDisk() {
        if diskFD >= 0 { Darwin.close(diskFD); diskFD = -1 }
    }

    deinit {
        if diskFD >= 0 { Darwin.close(diskFD) }
    }

    private func sendHDR(_ capabilities: HostDisplayCapabilities) {
        ports.sendDisplayHDR(supported: capabilities.hdrSupported, maxLuminanceNits: capabilities.maxLuminanceNits,
            maxFrameAverageLuminanceNits: capabilities.maxFrameAverageLuminanceNits,
            minLuminance10000ths: capabilities.minLuminance10000ths, sdrReferenceNits: capabilities.sdrReferenceNits)
        guestHDRCapabilities = capabilities
    }

    private func updateEDID(_ capabilities: HostDisplayCapabilities) {
        guard !stopping, let context else { return }
        do {
            try krunCheck("krun_display_set_hdr_capabilities", krun_display_set_hdr_capabilities(
                context, 0, capabilities.hdrSupported, capabilities.maxLuminanceNits,
                capabilities.maxFrameAverageLuminanceNits, capabilities.minLuminance10000ths))
            edidCapabilities = capabilities
        } catch {
            report("BRINEWELL error \(error)")
            shutdown()
        }
    }

    private func hostScreenChanged(_ capabilities: HostDisplayCapabilities) {
        guard !stopping else { return }
        func changed(_ a: UInt16, _ b: UInt16, by threshold: Int) -> Bool { abs(Int(a) - Int(b)) >= threshold }
        if let previous = guestHDRCapabilities {
            if previous.hdrSupported != capabilities.hdrSupported
                || changed(previous.maxLuminanceNits, capabilities.maxLuminanceNits, by: 10)
                || changed(previous.maxFrameAverageLuminanceNits, capabilities.maxFrameAverageLuminanceNits, by: 10)
                || changed(previous.sdrReferenceNits, capabilities.sdrReferenceNits, by: 10) {
                sendHDR(capabilities)
            }
        } else { sendHDR(capabilities) }
        if let previous = edidCapabilities {
            let flipped = previous.hdrSupported != capabilities.hdrSupported
            hdrWork?.cancel(); hdrWork = nil
            if flipped { updateEDID(capabilities) }
            else if changed(previous.maxLuminanceNits, capabilities.maxLuminanceNits, by: 50) {
                let item = DispatchWorkItem { [weak self] in self?.updateEDID(capabilities) }
                hdrWork = item
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: item)
            }
        }
        hostCapabilities = capabilities
        guard (try? capabilities.validate(currentDisplayMode)) == nil,
              let mode = capabilities.preferredMode(for: currentDisplayMode), let context else { return }
        do {
            try krunCheck("krun_display_set_mode", krun_display_set_mode(
                context, 0, UInt32(mode.width), UInt32(mode.height), UInt32(mode.refreshRate), 339, 212
            ))
            currentDisplayMode = mode
            display.displayRefreshRate = Double(mode.refreshRate)
            report("BRINEWELL display width=\(mode.width) height=\(mode.height) refresh_rate=\(mode.refreshRate)")
        } catch {
            report("BRINEWELL error \(error)")
            shutdown()
        }
    }

    /// Emits the scanout path counters next to a frame dump. "native" counts
    /// scanouts handed to the display as a GPU resource; a healthy zero-copy
    /// session keeps `cpu_readback_bytes` at zero after the first frames.
    private func reportScanoutStats() {
        guard let context else { return }
        var native: UInt64 = 0
        var fallbacks: UInt64 = 0
        var readback: UInt64 = 0
        let result = krun_gpu_get_scanout_stats(context, &native, &fallbacks, &readback)
        guard result == 0 else { return }
        report("BRINEWELL scanout native=\(native) fallbacks=\(fallbacks) cpu_readback_bytes=\(readback)")
    }

    public func setGameHUD(level: GameHUDLevel, position: GameHUDPosition) {
        guard !stopping else { return }
        display.setGameHUD(level: level, position: position)
    }

    public func installUU() throws {
        guard !stopping else { throw RuntimeError(String(localized: "The virtual machine is shutting down.")) }
        try ports.sendUU(.install)
    }

    public func refreshUUStatus() throws {
        guard !stopping else { throw RuntimeError(String(localized: "The virtual machine is shutting down.")) }
        try ports.sendUU(.status)
    }

    /// Live backend switch, applied on the next game launch. Availability was
    /// validated once at startup; refuse anything else.
    public func setGraphicsBackend(_ backend: GraphicsBackend) throws {
        guard !stopping else { throw RuntimeError(String(localized: "The virtual machine is shutting down.")) }
        guard options.availableBackends.contains(backend) else {
            throw RuntimeError(options.neptuneReasons[backend] ?? String(localized: "This graphics backend is not available."))
        }
        try ports.setGraphicsBackend(backend)
    }

    public func shutdown() {
        guard !stopping else { return }
        disconnectAdmin()
        stopping = true
        hdrWork?.cancel(); hdrWork = nil
        // Registration may be awaiting launchd. The startup continuation will
        // clean up without creating a VM when it resumes.
        guard started else {
            workContext.stop()
            return
        }
        ports.stopTelemetry()
        display.stopInput()
        input.setActive(false)
        ports.setInputActive(false)
        report("BRINEWELL state stopping")
        shutdownEvent.signal()
    }


    private func raiseFileLimit() {
        var limit = rlimit()
        var maximum: Int32 = 0
        var size = MemoryLayout<Int32>.size
        guard getrlimit(RLIMIT_NOFILE, &limit) == 0,
              sysctlbyname("kern.maxfilesperproc", &maximum, &size, nil, 0) == 0, maximum > 0 else { return }
        let wanted = min(limit.rlim_max, rlim_t(maximum))
        if limit.rlim_cur < wanted {
            limit.rlim_cur = wanted
            _ = setrlimit(RLIMIT_NOFILE, &limit)
        }
    }
}
