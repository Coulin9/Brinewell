import CKrun
import Darwin
import XCTest
@testable import BrinewellVMRuntime

final class RuntimeDisplayModeTests: XCTestCase {
    func testNativeModeAPIRejectsInvalidModesAndSupportsLegacyResize() throws {
        let created = krun_create_ctx()
        XCTAssertGreaterThanOrEqual(created, 0)
        guard created >= 0 else { return }
        let context = UInt32(created)
        defer { _ = krun_free_ctx(context) }
        XCTAssertEqual(krun_set_gpu_options2(context, (1 << 6) | (1 << 7), 8192 << 20), 0)
        let display = krun_add_display(context, 1280, 800)
        XCTAssertEqual(display, 0)
        guard display >= 0 else { return }
        let id = UInt32(display)
        XCTAssertEqual(krun_display_set_mode(context, id, 1920, 1080, 90, 339, 212), 0)
        XCTAssertEqual(krun_display_set_mode(context, id, 1920, 1080, 0, 339, 212), -EINVAL)
        XCTAssertEqual(krun_display_set_mode(context, id, 3456, 2234, 120, 339, 212), 0)
        XCTAssertEqual(krun_display_set_mode(context, id, 3840, 2160, 240, 339, 212), 0)
        XCTAssertEqual(krun_display_set_mode(context, id, 5120, 2880, 144, 339, 212), 0)
        XCTAssertEqual(krun_display_set_mode(context, id, 16384, 16384, 1000, 339, 212), -EINVAL)
        XCTAssertEqual(krun_display_set_refresh_rate(context, id, 0), -EINVAL)
        XCTAssertEqual(krun_display_set_refresh_rate(context, id, .max), -EINVAL)
        XCTAssertEqual(krun_display_set_mode(context, id + 1, 1280, 800, 60, 339, 212), -EINVAL)
        XCTAssertEqual(krun_display_resize(context, id, 1280, 800, 339, 212), 0)
    }
}
