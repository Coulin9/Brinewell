import AppKit
import BrinewellDisplay
import MetalKit
import XCTest
@testable import BrinewellVMRuntime

final class VMWindowTests: XCTestCase {
    @MainActor
    private func makeWindow() -> VMWindow {
        let window = VMWindow(contentRect: NSRect(x: 100, y: 100, width: 800, height: 500))
        window.isReleasedWhenClosed = false
        return window
    }

    @MainActor
    func testWindowUsesNativeFullScreen() {
        let window = makeWindow()
        defer { window.close() }
        XCTAssertTrue(window.collectionBehavior.contains(.fullScreenPrimary))
        XCTAssertFalse(window.collectionBehavior.contains(.fullScreenNone))
        XCTAssertTrue(window.styleMask.contains(.titled))
        XCTAssertFalse(window.isFullScreen)
    }

    @MainActor
    func testWindowFitWaitsForNativeTransitionAndRecoversFromFailure() {
        let window = makeWindow()
        defer { window.close() }
        let original = window.frame
        let aspect = NSSize(width: 1280, height: 800)
        window.contentAspectRatio = aspect
        window.windowWillEnterFullScreen(Notification(name: NSWindow.willEnterFullScreenNotification, object: window))
        VMWindowGeometry.fit(window, pixels: NSSize(width: 1920, height: 1080))
        XCTAssertEqual(window.frame, original)
        XCTAssertEqual(window.contentAspectRatio, aspect)
        XCTAssertTrue(window.isFullScreenTransitioning)
        window.windowDidFailToEnterFullScreen(window)
        XCTAssertFalse(window.isFullScreen)
        XCTAssertEqual(window.contentAspectRatio, aspect)
    }

    @MainActor
    func testMetalViewFillsContentWithoutSafeAreaPadding() throws {
        let window = makeWindow()
        let input = try VMInput()
        let view = VMInputView(renderer: try IOSurfaceRenderer(), input: input, onActivity: { _ in })
        window.contentView = view
        defer { view.stop(); window.close() }
        view.layoutSubtreeIfNeeded()
        func findMetalView(_ parent: NSView) -> MTKView? {
            if let metal = parent as? MTKView { return metal }
            return parent.subviews.lazy.compactMap { findMetalView($0) }.first
        }
        let metal = try XCTUnwrap(findMetalView(view))
        XCTAssertEqual(metal.convert(metal.bounds, to: view), view.bounds)
        XCTAssertEqual(metal.drawableSize.width, view.bounds.width * window.backingScaleFactor)
        XCTAssertEqual(metal.drawableSize.height, view.bounds.height * window.backingScaleFactor)
    }
}
