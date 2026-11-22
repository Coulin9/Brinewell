import AppKit
import BrinewellCore
import BrinewellDisplay
import XCTest
@testable import BrinewellVMRuntime

final class MouseCaptureTests: XCTestCase {
    private func drain(_ device: VMInputDevice) -> [VMInputEvent] {
        var events: [VMInputEvent] = []
        while let event = device.nextEvent() { events.append(event) }
        return events
    }

    @MainActor
    private func click(_ view: VMInputView, window: NSWindow, point: NSPoint = NSPoint(x: 100, y: 100)) throws {
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                                       timestamp: ProcessInfo.processInfo.systemUptime,
                                                       windowNumber: window.windowNumber, context: nil,
                                                       eventNumber: 0, clickCount: 1, pressure: 1))
            if type == .leftMouseDown { view.mouseDown(with: event) } else { view.mouseUp(with: event) }
        }
    }

    @MainActor
    func testCaptureRelativeEventsAndReleaseLifecycle() throws {
        // Opt in to the real foreground/cursor test; the regular suite never steals focus.
        guard ProcessInfo.processInfo.environment["BRINEWELL_TEST_MOUSE_CAPTURE"] == "1" else {
            throw XCTSkip("Run scripts/test-mouse-capture.sh for the macOS foreground capture test")
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.finishLaunching()
        let input = try VMInput()
        var activities: [Bool] = []
        let view = VMInputView(renderer: try IOSurfaceRenderer(), input: input, onActivity: { activities.append($0) })
        let window = VMWindow(contentRect: NSRect(x: 100, y: 100, width: 400, height: 400))
        window.isReleasedWhenClosed = false
        window.contentView = view
        view.frameSize = NSSize(width: 1280, height: 800)
        defer { view.stop(); window.close() }
        window.makeKeyAndOrderFront(nil)
        app.activate(ignoringOtherApps: true)
        let deadline = Date().addingTimeInterval(3)
        while !app.isActive && Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.02)) }
        guard app.isActive, window.isKeyWindow else {
            XCTFail("The capture test runner must be launched as a foreground macOS app")
            return
        }
        XCTAssertFalse(input.active)
        XCTAssertEqual(activities.last, true) // Gamepads follow window focus without mouse capture.

        // A letterbox click does not capture; the capture click itself does not reach the guest.
        try click(view, window: window, point: NSPoint(x: 100, y: 10))
        XCTAssertFalse(input.active)
        try click(view, window: window)
        XCTAssertTrue(input.active)
        XCTAssertTrue(window.title.contains("Control+Option"))
        XCTAssertFalse(drain(input.mouse).contains { $0.type == 1 && $0.value == 1 })
        _ = drain(input.keyboard)

        // Verify real full-screen Spaces, not merely a screen-sized desktop window.
        // AppKit may release focus during a Space transition; recapturing must work.
        XCTAssertTrue(window.firstResponder === view)
        XCTAssertNil(NativeFullScreenSupport.fullScreenSpaceID(for: window))
        let windowedFrame = window.frame
        let fullScreenKey = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero,
                                                          modifierFlags: [.control, .command], timestamp: 0,
                                                          windowNumber: window.windowNumber, context: nil,
                                                          characters: "f", charactersIgnoringModifiers: "f",
                                                          isARepeat: false, keyCode: 3))
        for (index, fullScreen) in [true, false, true, false].enumerated() {
            if index == 0 {
                try XCTUnwrap(window.standardWindowButton(.zoomButton)).performClick(nil)
            } else if index == 1 {
                XCTAssertTrue(window.performKeyEquivalent(with: fullScreenKey))
            } else {
                window.setFullScreen(fullScreen)
                window.setFullScreen(!fullScreen)
                window.setFullScreen(fullScreen) // Last request wins during the transition.
            }
            let transitionDeadline = Date().addingTimeInterval(8)
            while Date() < transitionDeadline &&
                (window.isFullScreenTransitioning || window.isFullScreen != fullScreen) {
                RunLoop.current.run(until: Date().addingTimeInterval(0.02))
            }
            XCTAssertFalse(window.isFullScreenTransitioning)
            XCTAssertEqual(window.styleMask.contains(.fullScreen), fullScreen)
            window.setFullScreen(fullScreen)
            window.setFullScreen(fullScreen)
            XCTAssertFalse(window.isFullScreenTransitioning) // Repeating a state is a no-op.
            if fullScreen {
                XCTAssertNotNil(NativeFullScreenSupport.fullScreenSpaceID(for: window))
                if NativeFullScreenSupport.isAvailable, let screen = window.screen, screen.safeAreaInsets.top > 0 {
                    XCTAssertEqual(window.frame, screen.frame)
                    window.makeKeyAndOrderFront(nil)
                    XCTAssertEqual(window.frame, screen.frame)
                    view.layoutSubtreeIfNeeded()
                    XCTAssertEqual(view.bounds.size, screen.frame.size)
                    print("Native full-screen Space verified: frame=\(window.frame) pixels=\(view.bounds.width * window.backingScaleFactor)x\(view.bounds.height * window.backingScaleFactor)")
                }
            } else {
                XCTAssertNil(NativeFullScreenSupport.fullScreenSpaceID(for: window))
                XCTAssertEqual(window.frame, windowedFrame)
            }
            XCTAssertTrue(window.firstResponder === view)
            if !input.active { try click(view, window: window) }
            XCTAssertTrue(input.active)
        }
        _ = drain(input.mouse)

        // Constant absolute coordinates with different hardware deltas must keep moving.
        let cgEvent = try XCTUnwrap(CGEvent(mouseEventSource: nil, mouseType: .mouseMoved,
                                           mouseCursorPosition: .zero, mouseButton: .left))
        cgEvent.setIntegerValueField(.mouseEventDeltaX, value: 11)
        cgEvent.setIntegerValueField(.mouseEventDeltaY, value: -5)
        let motion = try XCTUnwrap(NSEvent(cgEvent: cgEvent))
        for _ in 0..<2 { view.mouseMoved(with: motion) }
        let frame = [VMInputEvent(2, 0, 11), VMInputEvent(2, 1, -5), VMInputEvent(0, 0, 0)]
        XCTAssertEqual(drain(input.mouse), frame + frame)

        input.key(code: 0, down: true)
        input.button(0, down: true)
        _ = drain(input.keyboard)
        _ = drain(input.mouse)
        let release = try XCTUnwrap(NSEvent.keyEvent(with: .flagsChanged, location: .zero,
                                                    modifierFlags: [.control, .option], timestamp: 0,
                                                    windowNumber: window.windowNumber, context: nil,
                                                    characters: "", charactersIgnoringModifiers: "",
                                                    isARepeat: false, keyCode: 58))
        view.flagsChanged(with: release)
        XCTAssertFalse(input.active)
        XCTAssertEqual(window.title, "Brinewell — SteamOS · Click to Capture Mouse")
        XCTAssertTrue(drain(input.keyboard).contains(VMInputEvent(1, 30, 0)))
        XCTAssertTrue(drain(input.mouse).contains(VMInputEvent(1, 272, 0)))
        view.mouseMoved(with: motion)
        XCTAssertEqual(drain(input.mouse), [])

        for notification in [NSWindow.didResignKeyNotification, NSWindow.willMiniaturizeNotification,
                             NSWindow.willCloseNotification] {
            try click(view, window: window)
            XCTAssertTrue(input.active)
            NotificationCenter.default.post(name: notification, object: window)
            XCTAssertFalse(input.active)
            view.updateActivity()
            XCTAssertFalse(input.active) // No automatic recapture on focus return.
        }
        try click(view, window: window)
        NotificationCenter.default.post(name: NSApplication.willResignActiveNotification, object: app)
        XCTAssertFalse(input.active)
        try click(view, window: window)
        _ = drain(input.mouse)
        XCTAssertTrue(input.mouse.send(Array(repeating: VMInputEvent(2, 0, 1), count: 4095)))
        view.mouseMoved(with: motion)
        XCTAssertFalse(input.active)
        XCTAssertEqual(window.title, "Brinewell — SteamOS · Click to Capture Mouse")
        XCTAssertTrue(drain(input.mouse).contains(VMInputEvent(1, 272, 0)))
        try click(view, window: window)
        view.frameSize = .zero
        XCTAssertFalse(input.active)
        view.frameSize = NSSize(width: 1280, height: 800)
        try click(view, window: window)
        XCTAssertTrue(input.active)
        view.stop()
        try click(view, window: window)
        XCTAssertFalse(input.active)

        // The in-process launcher must close its guest display after VMM exit;
        // the host App itself stays alive. Exercise the real AppKit boundary.
        window.close()
        let existing = Set(app.windows.map(ObjectIdentifier.init))
        let display = try VMDisplay()
        let warning = try XCTUnwrap(WorkContextNotice.warning(for: "BRINEWELL work-context unavailable reason=backend"))
        display.setWorkContextWarning(warning)
        var closeRequests = 0
        display.show(input: input, mode: VMDisplayConfiguration(width: 1280, height: 800, refreshRate: 60),
                     screenID: 0, onScreenChange: { _ in }, onActivity: { _ in },
                     onClose: { closeRequests += 1 })
        defer { display.close() }
        let guestWindow = try XCTUnwrap(app.windows.first {
            $0 is VMWindow && !existing.contains(ObjectIdentifier($0))
        } as? VMWindow)
        try display.setFullScreen(true)
        let closeDeadline = Date().addingTimeInterval(8)
        while guestWindow.isFullScreenTransitioning && Date() < closeDeadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertTrue(guestWindow.styleMask.contains(.fullScreen))
        let content = try XCTUnwrap(guestWindow.contentView)
        let banner = try XCTUnwrap(content.subviews.first { $0.identifier?.rawValue == "work-context-warning" })
        content.layoutSubtreeIfNeeded()
        XCTAssertFalse(banner.isHidden)
        XCTAssertGreaterThan(banner.frame.height, 0)
        XCTAssertTrue(content.bounds.contains(banner.frame))
        XCTAssertNil(banner.hitTest(NSPoint(x: banner.frame.midX, y: banner.frame.midY)))
        let responder = guestWindow.firstResponder
        display.setWorkContextWarning(warning)
        XCTAssertTrue(banner === content.subviews.first { $0.identifier?.rawValue == "work-context-warning" })
        XCTAssertTrue(guestWindow.firstResponder === responder)
        display.setWorkContextWarning(nil)
        XCTAssertNil(banner.superview)
        display.close()
        display.close()
        XCTAssertFalse(guestWindow.isVisible)
        XCTAssertEqual(closeRequests, 0) // Cleanup must not request a second guest shutdown.
        XCTAssertThrowsError(try display.setFullScreen(true))
    }
}
