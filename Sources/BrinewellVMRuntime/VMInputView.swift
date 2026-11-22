import AppKit
import BrinewellDisplay
import SwiftUI

@MainActor
final class VMInputView: NSView {
    private let input: VMInput
    private let displayView: NSHostingView<IOSurfaceDisplay>
    private let onActivity: (Bool) -> Void
    private var observers: [NSObjectProtocol] = []
    private var pointerTracking: NSTrackingArea?
    private var stopped = false
    private var captured = false
    var frameSize: NSSize = .zero {
        didSet {
            if frameSize.width < 1 || frameSize.height < 1 { releaseCapture() }
        }
    }

    init(renderer: IOSurfaceRenderer, input: VMInput, onActivity: @escaping (Bool) -> Void) {
        self.input = input
        self.onActivity = onActivity
        displayView = NSHostingView(rootView: IOSurfaceDisplay(renderer: renderer))
        // The VM owns every pixel, including the area beside the camera housing.
        displayView.safeAreaRegions = []
        super.init(frame: .zero)
        addSubview(displayView)
    }

    required init?(coder: NSCoder) { nil }
    override var acceptsFirstResponder: Bool { true }
    // Keep the Metal/SwiftUI child presentation-only; native input has one owner.
    override func hitTest(_ point: NSPoint) -> NSView? {
        bounds.contains(convert(point, from: superview)) ? self : nil
    }
    override func layout() {
        super.layout()
        displayView.frame = bounds
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        releaseCapture()
        removeObservers()
        guard let window, !stopped else { updateActivity(forceInactive: true); return }
        window.acceptsMouseMovedEvents = true
        for name in [NSApplication.didBecomeActiveNotification, NSApplication.willResignActiveNotification,
                     NSApplication.willTerminateNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: NSApp, queue: .main) { [weak self] notification in
                let inactive = notification.name != NSApplication.didBecomeActiveNotification
                MainActor.assumeIsolated { self?.updateActivity(forceInactive: inactive) }
            })
        }
        // Window changes must release capture even if no further input arrives.
        for name in [NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification,
                     NSWindow.willMiniaturizeNotification, NSWindow.willCloseNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: window, queue: .main) { [weak self] notification in
                let inactive = notification.name != NSWindow.didBecomeKeyNotification
                MainActor.assumeIsolated { self?.updateActivity(forceInactive: inactive) }
            })
        }
        for name in [NSWindow.didEnterFullScreenNotification, NSWindow.didExitFullScreenNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.updateActivity() }
            })
        }
        updateTitle()
        updateActivity()
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let pointerTracking { removeTrackingArea(pointerTracking) }
        let area = NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self)
        addTrackingArea(area)
        pointerTracking = area
    }

    private func removeObservers() {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers.removeAll()
    }

    func updateActivity(forceInactive: Bool = false) {
        let enabled = !forceInactive && !stopped && window?.isKeyWindow == true && NSApp.isActive
        if !enabled { releaseCapture() }
        input.setActive(enabled && captured, modifiers: NSEvent.modifierFlags)
        onActivity(enabled)
    }

    private func updateTitle() {
        window?.title = captured
            ? String(localized: "Brinewell — SteamOS · Control+Option to Release Mouse")
            : String(localized: "Brinewell — SteamOS · Click to Capture Mouse")
    }

    private func capture(at point: NSPoint) {
        guard !stopped, !captured, NSApp.isActive, window?.isKeyWindow == true,
              frameSize.width > 0, frameSize.height > 0, bounds.width > 0, bounds.height > 0 else { return }
        let scale = min(bounds.width / frameSize.width, bounds.height / frameSize.height)
        let picture = NSRect(x: bounds.midX - frameSize.width * scale / 2,
                             y: bounds.midY - frameSize.height * scale / 2,
                             width: frameSize.width * scale, height: frameSize.height * scale)
        guard picture.contains(point) else { return }
        guard CGAssociateMouseAndMouseCursorPosition(0) == .success else {
            report("BRINEWELL input error cannot capture mouse")
            return
        }
        NSCursor.hide()
        captured = true
        window?.makeFirstResponder(self)
        updateTitle()
        updateActivity()
        checkInput()
    }

    private func releaseCapture() {
        guard captured else { return }
        CGAssociateMouseAndMouseCursorPosition(1)
        NSCursor.unhide()
        captured = false
        input.setActive(false)
        updateTitle()
    }

    // Queue saturation also has to restore the host cursor immediately.
    private func checkInput() {
        if !input.active { releaseCapture() }
    }

    func stop() {
        stopped = true
        updateActivity(forceInactive: true)
        removeObservers()
    }

    private func acceptsInput() -> Bool {
        updateActivity()
        return captured && input.active && !stopped
    }

    override func keyDown(with event: NSEvent) {
        guard acceptsInput() else { super.keyDown(with: event); return }
        if !input.key(code: event.keyCode, down: true, modifiers: event.modifierFlags) {
            super.keyDown(with: event)
        }
        checkInput()
    }
    override func keyUp(with event: NSEvent) {
        guard acceptsInput() else { return }
        _ = input.key(code: event.keyCode, down: false, modifiers: event.modifierFlags)
        checkInput()
    }
    override func flagsChanged(with event: NSEvent) {
        if event.modifierFlags.contains([.control, .option]) {
            releaseCapture()
            return
        }
        guard acceptsInput() else { return }
        input.flagsChanged(code: event.keyCode, flags: event.modifierFlags)
        checkInput()
    }

    private func move(_ event: NSEvent) {
        guard acceptsInput() else { return }
        input.move(deltaX: event.deltaX, deltaY: event.deltaY)
        checkInput()
    }
    private func button(_ event: NSEvent, down: Bool) {
        if !captured {
            // Consume the capture click so it cannot fire a weapon or activate a menu.
            if down && event.buttonNumber == 0 { capture(at: convert(event.locationInWindow, from: nil)) }
            return
        }
        guard acceptsInput() else { return }
        input.button(event.buttonNumber, down: down)
        checkInput()
    }
    override func mouseMoved(with event: NSEvent) { move(event) }
    override func mouseDragged(with event: NSEvent) { move(event) }
    override func rightMouseDragged(with event: NSEvent) { move(event) }
    override func otherMouseDragged(with event: NSEvent) { move(event) }
    override func mouseDown(with event: NSEvent) { button(event, down: true) }
    override func mouseUp(with event: NSEvent) { button(event, down: false) }
    override func rightMouseDown(with event: NSEvent) { button(event, down: true) }
    override func rightMouseUp(with event: NSEvent) { button(event, down: false) }
    override func otherMouseDown(with event: NSEvent) { button(event, down: true) }
    override func otherMouseUp(with event: NSEvent) { button(event, down: false) }
    override func scrollWheel(with event: NSEvent) {
        guard acceptsInput() else { return }
        input.scroll(deltaX: event.scrollingDeltaX, deltaY: event.scrollingDeltaY,
                     precise: event.hasPreciseScrollingDeltas)
        checkInput()
    }
}
