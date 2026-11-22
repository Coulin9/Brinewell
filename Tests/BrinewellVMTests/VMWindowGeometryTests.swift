import AppKit
import XCTest
@testable import BrinewellVMRuntime

final class VMWindowGeometryTests: XCTestCase {
    func testRetinaWindowSizeFollowsGuestPixels() {
        let available = NSSize(width: 1700, height: 1000)
        XCTAssertEqual(VMWindowGeometry.contentSize(pixels: NSSize(width: 1280, height: 800), backingScale: 2, available: available),
                       NSSize(width: 640, height: 400))
        XCTAssertEqual(VMWindowGeometry.contentSize(pixels: NSSize(width: 1920, height: 1080), backingScale: 2, available: available),
                       NSSize(width: 960, height: 540))
    }

    func testLargeAndPortraitModesFitTheScreenWithoutChangingAspect() {
        for pixels in [NSSize(width: 3840, height: 2160), NSSize(width: 1080, height: 1920)] {
            let available = NSSize(width: 1200, height: 700)
            let size = VMWindowGeometry.contentSize(pixels: pixels, backingScale: 1, available: available)
            XCTAssertLessThanOrEqual(size.width, available.width)
            XCTAssertLessThanOrEqual(size.height, available.height)
            XCTAssertEqual(size.width / size.height, pixels.width / pixels.height, accuracy: 0.00001)
        }
    }

    @MainActor
    func testActualWindowGeometryTracksResolution() throws {
        guard NSScreen.main != nil else { throw XCTSkip("No host display is available") }
        let window = VMWindow(contentRect: NSRect(x: 100, y: 100, width: 1024, height: 640))
        window.isReleasedWhenClosed = false
        defer { window.close() }
        VMWindowGeometry.fit(window, pixels: NSSize(width: 1920, height: 1080))
        let content = window.contentRect(forFrameRect: window.frame).size
        XCTAssertEqual(content.width / content.height, 16.0 / 9.0, accuracy: 0.005)
        XCTAssertEqual(window.contentAspectRatio, NSSize(width: 1920, height: 1080))
        VMWindowGeometry.fit(window, pixels: NSSize(width: 1280, height: 800))
        let smaller = window.contentRect(forFrameRect: window.frame).size
        XCTAssertLessThan(smaller.width, content.width)
        XCTAssertEqual(smaller.width / smaller.height, 1.6, accuracy: 0.005)
    }
}
