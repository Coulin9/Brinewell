import XCTest
@testable import BrinewellVMRuntime

final class VMSessionRebootTests: XCTestCase {
    func testResetAfterReadyPreservesSession() {
        var policy = VMSessionPolicy()
        policy.begin(shrinkPreparation: false)
        let first = policy.beginBoot()
        XCTAssertTrue(policy.markReady())
        XCTAssertTrue(policy.shouldPreserveWindow)
        XCTAssertEqual(policy.decideReset(), .restart)
        let second = policy.beginBoot()
        XCTAssertGreaterThan(second, first)
        XCTAssertTrue(policy.active)
        XCTAssertFalse(policy.accepts(first))
    }

    func testStopWinsReset() {
        var policy = VMSessionPolicy()
        policy.begin(shrinkPreparation: false)
        _ = policy.beginBoot()
        policy.requestedStop = true
        XCTAssertFalse(policy.shouldPreserveWindow)
        XCTAssertEqual(policy.decideReset(), .stop)
        XCTAssertFalse(policy.markReady())
    }

    func testStaleGenerationRejected() {
        var policy = VMSessionPolicy()
        policy.begin(shrinkPreparation: false)
        let old = policy.beginBoot()
        _ = policy.beginBoot()
        XCTAssertFalse(policy.accepts(old))
        policy.active = false
        XCTAssertFalse(policy.accepts(policy.generation))
    }

    func testResetLimitAndReadyRecovery() {
        var policy = VMSessionPolicy(maxPreReadyResets: 3)
        policy.begin(shrinkPreparation: false)
        for _ in 0..<3 {
            _ = policy.beginBoot()
            XCTAssertEqual(policy.decideReset(), .restart)
        }
        XCTAssertFalse(policy.shouldPreserveWindow)
        XCTAssertEqual(policy.decideReset(), .tooManyResets)
        XCTAssertTrue(policy.markReady())
        for _ in 0..<10 {
            XCTAssertEqual(policy.decideReset(), .restart)
            _ = policy.beginBoot()
            XCTAssertTrue(policy.markReady())
        }
    }

    func testNewBootClearsFatalAndReady() {
        var policy = VMSessionPolicy()
        policy.begin(shrinkPreparation: false)
        _ = policy.beginBoot()
        policy.fatalError = true
        XCTAssertFalse(policy.markReady())
        _ = policy.beginBoot()
        XCTAssertFalse(policy.fatalError)
        XCTAssertFalse(policy.ready)
    }

    func testShrinkResetDoesNotRestart() {
        var policy = VMSessionPolicy()
        policy.begin(shrinkPreparation: true)
        _ = policy.beginBoot()
        XCTAssertFalse(policy.shouldPreserveWindow)
        XCTAssertEqual(policy.decideReset(), .abortShrink)
    }

    func testReadyRejectsFatalOrStop() {
        var policy = VMSessionPolicy()
        policy.begin(shrinkPreparation: false)
        _ = policy.beginBoot()
        policy.fatalError = true
        XCTAssertFalse(policy.markReady())
        policy.fatalError = false
        policy.requestedStop = true
        XCTAssertFalse(policy.markReady())
    }
}
