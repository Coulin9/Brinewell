import AppKit

/// AppKit owns the full-screen Space, animations and window restoration.
@MainActor
final class VMWindow: NSWindow, NSWindowDelegate {
    private enum FullScreenPhase { case windowed, entering, fullScreen, exiting }
    private var phase = FullScreenPhase.windowed
    private var requestedFullScreen: Bool?
    private var hiddenMenuBarSpace: UInt64?
    private let extendsCameraArea: Bool
    var isFullScreen: Bool { phase != .windowed || styleMask.contains(.fullScreen) }
    var isFullScreenTransitioning: Bool { phase == .entering || phase == .exiting }
    var onExitFullScreen: (() -> Void)?
    var onScreenChange: (() -> Void)?
    var onClose: (() -> Void)?

    init(contentRect: NSRect, screen: NSScreen? = nil) {
        extendsCameraArea = NativeFullScreenSupport.isAvailable
        let rect = contentRect.offsetBy(dx: screen?.frame.minX ?? 0, dy: screen?.frame.minY ?? 0)
        super.init(contentRect: rect,
                   styleMask: [.titled, .closable, .miniaturizable, .resizable],
                   backing: .buffered, defer: false)
        collectionBehavior.insert(.fullScreenPrimary)
        isReleasedWhenClosed = false
        delegate = self
    }

    /// Coalesce requests during AppKit's transition; a repeated "on" or "off"
    /// never toggles the window back to the opposite state.
    func setFullScreen(_ enabled: Bool) {
        requestedFullScreen = enabled
        applyFullScreenRequest()
    }

    private func applyFullScreenRequest() {
        guard !isFullScreenTransitioning, let enabled = requestedFullScreen else { return }
        requestedFullScreen = nil
        guard isFullScreen != enabled else { return }
        phase = enabled ? .entering : .exiting
        super.toggleFullScreen(nil)
    }

    private func finishFullScreenRequest() {
        report("BRINEWELL display fullscreen \(phase == .fullScreen ? "on" : "off")")
        DispatchQueue.main.async { [weak self] in
            guard let self, self.isVisible else { return }
            self.applyFullScreenRequest()
        }
    }

    override func close() {
        requestedFullScreen = nil
        super.close()
    }

    func windowWillEnterFullScreen(_ notification: Notification) {
        phase = .entering
    }

    func windowDidEnterFullScreen(_ notification: Notification) {
        finishEnteringFullScreen()
    }

    private func finishEnteringFullScreen() {
        phase = .fullScreen
        restoreKeyboardFocus()
        updateMenuBar()
        finishFullScreenRequest()
    }

    func windowWillExitFullScreen(_ notification: Notification) {
        phase = .exiting
        restoreMenuBar()
    }

    func windowDidExitFullScreen(_ notification: Notification) {
        finishExitingFullScreen()
    }

    private func finishExitingFullScreen() {
        phase = .windowed
        onExitFullScreen?()
        restoreKeyboardFocus()
        finishFullScreenRequest()
    }

    func windowDidFailToEnterFullScreen(_ window: NSWindow) {
        finishExitingFullScreen()
        (contentView as? VMInputView)?.updateActivity()
    }

    func windowDidFailToExitFullScreen(_ window: NSWindow) {
        finishEnteringFullScreen()
        (contentView as? VMInputView)?.updateActivity()
    }

    private func restoreKeyboardFocus() {
        // AppKit updates its responder again after the delegate callback returns.
        DispatchQueue.main.async { [weak self] in
            guard let self, self.isVisible else { return }
            self.makeFirstResponder(self.contentView)
        }
    }

    func windowDidChangeScreen(_ notification: Notification) { onScreenChange?() }
    func windowDidChangeBackingProperties(_ notification: Notification) { onScreenChange?() }
    func windowDidResize(_ notification: Notification) { updateMenuBar() }
    func windowDidBecomeKey(_ notification: Notification) { updateMenuBar() }
    func windowWillClose(_ notification: Notification) { restoreMenuBar() }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard let onClose else { return true }
        onClose()
        return false
    }

    /// Reordering an already extended full-screen window otherwise reapplies the
    /// camera inset. Allow AppKit to resize tiles and to restore windowed mode.
    override func setFrame(_ frameRect: NSRect, display flag: Bool) {
        if extendsCameraArea, (phase == .entering || phase == .fullScreen),
           styleMask.contains(.fullScreen), let screen, screen.safeAreaInsets.top > 0,
           frame == screen.frame, NativeFullScreenSupport.occupiesWholeScreen(self) {
            if flag { display() }
            return
        }
        super.setFrame(frameRect, display: flag)
    }

    private func updateMenuBar() {
        let space: UInt64?
        if extendsCameraArea, phase == .fullScreen, let screen,
           screen.safeAreaInsets.top > 0, frame == screen.frame {
            space = NativeFullScreenSupport.fullScreenSpaceID(for: self)
        } else {
            space = nil
        }
        guard space != hiddenMenuBarSpace else { return }
        restoreMenuBar()
        if let space, NativeFullScreenSupport.setMenuBarAlpha(0, in: space) {
            hiddenMenuBarSpace = space
        }
    }

    private func restoreMenuBar() {
        if let space = hiddenMenuBarSpace {
            _ = NativeFullScreenSupport.setMenuBarAlpha(1, in: space)
            hiddenMenuBarSpace = nil
        }
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.modifierFlags.intersection([.control, .command, .option, .shift]) == [.control, .command],
           event.charactersIgnoringModifiers?.lowercased() == "f" {
            if !isFullScreenTransitioning && !event.isARepeat { super.toggleFullScreen(nil) }
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}
