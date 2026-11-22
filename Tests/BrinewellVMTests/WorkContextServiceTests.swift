import Foundation
import ServiceManagement
import XCTest
@testable import BrinewellVMRuntime

@MainActor private var activityCalls: [Bool] = []

private func recordWorkContextActivity(_ active: Bool) {
    MainActor.assumeIsolated { activityCalls.append(active) }
}

private func readyWorkContextStatus() -> Int32 { 1 }

private final class BrokerProbeState: @unchecked Sendable {
    private let lock = NSLock()
    private var results: [Int32] = []
    private var calls: [(String, UInt32)] = []
    private var started: XCTestExpectation?
    private var release: DispatchSemaphore?

    func reset(results: [Int32], started: XCTestExpectation? = nil, release: DispatchSemaphore? = nil) {
        lock.withLock {
            self.results = results
            calls = []
            self.started = started
            self.release = release
        }
    }

    func call(_ name: UnsafePointer<CChar>, _ timeout: UInt32) -> Int32 {
        let (result, started, release) = lock.withLock {
            calls.append((String(cString: name), timeout))
            return (results.isEmpty ? 0 : results.removeFirst(), self.started, self.release)
        }
        started?.fulfill()
        _ = release?.wait(timeout: .now() + .seconds(2))
        return result
    }

    var recordedCalls: [(String, UInt32)] { lock.withLock { calls } }
}

private let brokerProbeState = BrokerProbeState()

private func probeWorkContextBroker(_ name: UnsafePointer<CChar>, _ timeout: UInt32) -> Int32 {
    brokerProbeState.call(name, timeout)
}

final class WorkContextServiceTests: XCTestCase {
    @MainActor
    func testBrokerProbeRetriesUntilPongAndBoundsImmediateFailures() async throws {
        let service = WorkContextService()
        defer { service.stop() }
        brokerProbeState.reset(results: [0, 1])
        let ready = try await service.waitForBroker(probeWorkContextBroker, timeout: .seconds(2))
        XCTAssertTrue(ready)
        XCTAssertEqual(brokerProbeState.recordedCalls.count, 2)
        XCTAssertTrue(brokerProbeState.recordedCalls.allSatisfy {
            $0.0 == WorkContextService.name && $0.1 > 0 && $0.1 <= 1_000
        })

        brokerProbeState.reset(results: [])
        let clock = ContinuousClock()
        let began = clock.now
        let unavailable = try await service.waitForBroker(probeWorkContextBroker, timeout: .milliseconds(550))
        XCTAssertFalse(unavailable)
        XCTAssertGreaterThanOrEqual(began.duration(to: clock.now), .milliseconds(550))
        XCTAssertLessThan(began.duration(to: clock.now), .seconds(2))
        XCTAssertTrue((2...3).contains(brokerProbeState.recordedCalls.count))
    }

    @MainActor
    func testStopRejectsPendingPongAndLeavesMonitoringDetached() async throws {
        activityCalls = []
        let service = WorkContextService()
        defer { service.stop() }
        let started = expectation(description: "probe entered")
        let release = DispatchSemaphore(value: 0)
        brokerProbeState.reset(results: [1], started: started, release: release)
        let waiting = Task { try await service.waitForBroker(probeWorkContextBroker) }
        await fulfillment(of: [started], timeout: 2)
        service.stop()
        release.signal()
        do {
            _ = try await waiting.value
            XCTFail("A stopped startup must reject even a valid pending pong")
        } catch is CancellationError { }
        XCTAssertEqual(brokerProbeState.recordedCalls.count, 1)
        service.setActive(true)
        service.poll()
        XCTAssertTrue(activityCalls.isEmpty)
        XCTAssertNil(getenv("NPT_WORK_CONTEXT_SERVICE"))
    }

    @MainActor
    func testTaskCancellationRejectsPendingPong() async throws {
        let service = WorkContextService()
        defer { service.stop() }
        let started = expectation(description: "probe entered")
        let release = DispatchSemaphore(value: 0)
        brokerProbeState.reset(results: [1], started: started, release: release)
        let waiting = Task { try await service.waitForBroker(probeWorkContextBroker) }
        await fulfillment(of: [started], timeout: 2)
        waiting.cancel()
        release.signal()
        do {
            _ = try await waiting.value
            XCTFail("A canceled startup must reject even a valid pending pong")
        } catch is CancellationError { }
        XCTAssertEqual(brokerProbeState.recordedCalls.count, 1)
    }

    @MainActor
    func testActivityArrivingBeforeConnectionAndUnchangedReadyStillRecheck() {
        activityCalls = []
        let service = WorkContextService()
        defer { service.stop() }
        var reports: [String] = []
        service.onReport = { reports.append($0) }

        service.setActive(true)
        XCTAssertTrue(activityCalls.isEmpty)
        service.startMonitoring(status: readyWorkContextStatus, updateActivity: recordWorkContextActivity)
        XCTAssertEqual(activityCalls, [true])
        XCTAssertEqual(reports, ["BRINEWELL work-context ready"])

        service.poll()
        service.setActive(true) // Full-screen completion can keep the same activity.
        service.poll()
        XCTAssertEqual(activityCalls, [true, true, true, true])
        XCTAssertEqual(reports.count, 1) // Stable readiness does not stop policy checks.

        service.setActive(false)
        service.poll()
        service.poll()
        XCTAssertEqual(activityCalls, [true, true, true, true, false])
    }

    @MainActor
    func testReplacingConnectionPreservesActivityAndStopDetachesCallbacks() {
        activityCalls = []
        let service = WorkContextService()
        defer { service.stop() }
        service.startMonitoring(status: readyWorkContextStatus, updateActivity: recordWorkContextActivity)
        service.setActive(true)
        service.startMonitoring(status: readyWorkContextStatus, updateActivity: recordWorkContextActivity)
        XCTAssertEqual(activityCalls, [false, true, false, true])

        service.stop()
        XCTAssertEqual(activityCalls, [false, true, false, true, false])
        service.poll()
        service.setActive(true)
        service.poll()
        XCTAssertEqual(activityCalls.count, 5)
        service.startMonitoring(status: readyWorkContextStatus, updateActivity: recordWorkContextActivity)
        XCTAssertEqual(activityCalls.last, true)
    }

    @MainActor
    func testMissingRegistrationAllowsFirstInstallButPreservesApprovalChoice() {
        XCTAssertTrue(WorkContextService.needsInitialRegistration(.notFound))
        XCTAssertTrue(WorkContextService.needsInitialRegistration(.notRegistered))
        XCTAssertFalse(WorkContextService.needsInitialRegistration(.requiresApproval))
        XCTAssertFalse(WorkContextService.needsInitialRegistration(.enabled))
    }

    @MainActor
    func testRegistrationVersionChangesWhenPackagedServiceChanges() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? fm.removeItem(at: root) }
        let renderer = root.appendingPathComponent("Contents/Helpers/virgl_render_server")
        let plist = root.appendingPathComponent("Contents/Library/LaunchAgents/org.brinewell.render-context.plist")
        let library = root.appendingPathComponent("Contents/Frameworks/libvirglrenderer.1.dylib")
        for file in [renderer, plist, library] {
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data([1]).write(to: file)
        }
        var version = try WorkContextService.registrationVersion(renderer: renderer)
        XCTAssertEqual(try WorkContextService.registrationVersion(renderer: renderer), version)
        for file in [renderer, plist, library] {
            try Data([1, 2]).write(to: file)
            let changed = try WorkContextService.registrationVersion(renderer: renderer)
            XCTAssertNotEqual(changed, version)
            version = changed
        }
        try fm.setAttributes([.modificationDate: Date(timeIntervalSince1970: 1)], ofItemAtPath: renderer.path)
        XCTAssertNotEqual(try WorkContextService.registrationVersion(renderer: renderer), version)
        try fm.removeItem(at: plist)
        XCTAssertThrowsError(try WorkContextService.registrationVersion(renderer: renderer))
    }

    func testOnlyUnavailableReportsRequestManualGameModeOff() throws {
        for reason in ["backend", "registration", "renderer-library", "renderer-connection-or-framework", "broker-startup"] {
            let warning = try XCTUnwrap(WorkContextNotice.warning(for: "BRINEWELL work-context unavailable reason=\(reason)"))
            XCTAssertTrue(warning.contains("Full screen remains available"))
            XCTAssertTrue(warning.contains("turn off Game Mode"))
            XCTAssertFalse(warning.contains("Login Items & Extensions"))
        }
        let approval = try XCTUnwrap(WorkContextNotice.warning(for: "BRINEWELL work-context unavailable reason=background-service-approval"))
        XCTAssertTrue(approval.contains("Login Items & Extensions"))
        XCTAssertTrue(approval.contains("restart the virtual machine"))
        XCTAssertNil(WorkContextNotice.warning(for: "BRINEWELL work-context ready"))
        XCTAssertNil(WorkContextNotice.warning(for: "BRINEWELL work-context pending"))
    }
}
