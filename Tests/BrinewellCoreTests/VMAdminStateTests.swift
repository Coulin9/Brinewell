import XCTest
@testable import BrinewellCore

final class VMAdminStateTests: XCTestCase {
    private func state(active: String = "inactive", unit: String = "disabled",
                       load: String = "loaded", socketActive: String = "inactive",
                       socketUnit: String = "disabled", listening: Bool = false) -> VMGuestSSHState {
        VMGuestSSHState(serviceLoad: load, serviceActive: active, serviceSub: "dead", serviceUnitFile: unit,
                        socketLoad: "loaded", socketActive: socketActive, socketSub: "dead",
                        socketUnitFile: socketUnit, listeningOn22: listening)
    }

    func testEndpoint() {
        XCTAssertEqual(VMSSHEndpoint().command, "ssh -p 2222 steamos@127.0.0.1")
        XCTAssertEqual(VMSSHEndpoint(host: "example", port: 22, username: "user").command, "ssh -p 22 user@example")
    }

    func testSSHDerivedStates() {
        let off = state()
        XCTAssertFalse(off.isActive)
        XCTAssertFalse(off.isEnabledOnBoot)
        XCTAssertFalse(off.shouldExposeHostForward)
        let on = state(active: "active", unit: "enabled", listening: true)
        XCTAssertTrue(on.isServiceActive)
        XCTAssertTrue(on.isServiceEnabledOnBoot)
        XCTAssertTrue(on.isEnabledOnBoot)
        XCTAssertTrue(on.shouldExposeHostForward)
        XCTAssertFalse(on.isRunningOnly)
        XCTAssertFalse(on.isBootOnly)
        XCTAssertTrue(state(active: "active").isRunningOnly)
        XCTAssertTrue(state(unit: "enabled").isBootOnly)
        let socket = state(socketActive: "active", socketUnit: "enabled", listening: true)
        XCTAssertTrue(socket.isSocketActive)
        XCTAssertTrue(socket.isSocketOnly)
        XCTAssertTrue(socket.isSocketEnabledOnBoot)
        XCTAssertTrue(socket.shouldExposeHostForward)
        let maskedActive = state(active: "active", load: "masked", listening: true)
        XCTAssertTrue(maskedActive.isMasked)
        XCTAssertTrue(maskedActive.isActive)
        XCTAssertTrue(maskedActive.shouldExposeHostForward)
        XCTAssertTrue(state(load: "masked").isMasked)
        XCTAssertTrue(state(unit: "masked-runtime").isMasked)
        XCTAssertFalse(state(unit: "masked", socketActive: "active").isMasked)
        XCTAssertFalse(state(unit: "masked", socketUnit: "enabled").isMasked)
        for unit in ["static", "enabled-runtime", "indirect", "masked-runtime"] {
            XCTAssertFalse(state(unit: unit).isServiceEnabledOnBoot)
        }
        let unavailable = state(active: "active")
        XCTAssertTrue(unavailable.isPort22UnavailableWhileActive)
        XCTAssertFalse(unavailable.shouldExposeHostForward)
    }

    func testPasswordValidation() {
        XCTAssertEqual(VMPasswordValidation.validate(password: "", confirmation: ""), .empty)
        XCTAssertEqual(VMPasswordValidation.validate(password: "a", confirmation: "b"), .mismatch)
        XCTAssertEqual(VMPasswordValidation.validate(password: "é", confirmation: "e\u{301}"), .mismatch)
        XCTAssertNil(VMPasswordValidation.validate(password: " 密码 ", confirmation: " 密码 "))
        let boundary = String(repeating: "é", count: 128)
        XCTAssertNil(VMPasswordValidation.validate(password: boundary, confirmation: boundary))
        XCTAssertEqual(VMPasswordValidation.validate(password: boundary + "x", confirmation: boundary + "x"), .tooLong)
        for character in ["\n", "\r", "\0", ":"] {
            XCTAssertEqual(VMPasswordValidation.validate(password: "a" + character, confirmation: "a" + character), .unsupportedCharacter)
        }
    }
}
