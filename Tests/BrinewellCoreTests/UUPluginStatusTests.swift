import XCTest
@testable import BrinewellCore

final class UUPluginStatusTests: XCTestCase {
    func testInstallRemainsUnavailableUntilGuestReportsAbsence() {
        var status = UUPluginStatus()
        XCTAssertFalse(status.canInstall)
        XCTAssertFalse(status.receive("status installed ready"))
        XCTAssertTrue(status.receive("hello"))
        XCTAssertEqual(status.phase, .checking)
        XCTAssertFalse(status.canInstall)
        XCTAssertTrue(status.receive("status absent 尚未安装 UU"))
        XCTAssertTrue(status.canInstall)
    }

    func testProgressCannotClaimInstallationSuccess() {
        var status = readyStatus()
        status.beginInstallation()
        XCTAssertFalse(status.canInstall)
        XCTAssertFalse(status.canRefresh)
        XCTAssertTrue(status.receive("progress 100 正在验证服务"))
        XCTAssertEqual(status.phase, .installing)
        XCTAssertNil(status.bindingCode)
        XCTAssertTrue(status.receive("status installed UU 已运行"))
        XCTAssertEqual(status.phase, .installed)
        XCTAssertTrue(status.canRefresh)
    }

    func testBindingCodeIsPreservedExactlyAndClearedOnFailure() {
        var status = readyStatus()
        let identifier = "0123456789abcdef0123456789ABCDEF"
        XCTAssertFalse(status.receive("binding \(identifier)"))
        status.receive("status installed UU 已运行")
        XCTAssertTrue(status.receive("binding \(identifier)"))
        XCTAssertEqual(status.bindingCode, identifier)
        for value in ["https://example.com", "1234", "0123456789abcdef0123456789abcdef extra"] {
            XCTAssertFalse(status.receive("binding \(value)"))
            XCTAssertEqual(status.bindingCode, identifier)
        }
        status.receive("status error UU 服务已停止，请重试")
        XCTAssertEqual(status.phase, .failed)
        XCTAssertNil(status.bindingCode)
        XCTAssertTrue(status.canInstall)
        status.beginInstallation()
        XCTAssertEqual(status.phase, .installing)
    }

    func testRefreshDoesNotRetainAnOldBindingCode() {
        var status = readyStatus()
        status.receive("status installed UU 已运行")
        status.receive("binding 0123456789abcdef0123456789abcdef")
        status.receive("status installed UU 已运行，正在生成绑定二维码")
        XCTAssertNil(status.bindingCode)
        XCTAssertEqual(status.phase, .installed)
        XCTAssertTrue(status.canRefresh)
    }

    func testMalformedStatusDoesNotReplaceKnownState() {
        var status = readyStatus()
        let expected = status
        for line in ["progress -1 invalid", "progress 101 invalid", "progress x invalid", "progress 25",
                     "status unknown invalid", "status installed", "status installed a\nstatus absent b",
                     "binding " + String(repeating: "a", count: 5000)] {
            XCTAssertFalse(status.receive(line))
            XCTAssertEqual(status, expected)
        }
    }

    private func readyStatus() -> UUPluginStatus {
        var status = UUPluginStatus()
        status.receive("hello")
        status.receive("status absent 尚未安装")
        return status
    }
}
