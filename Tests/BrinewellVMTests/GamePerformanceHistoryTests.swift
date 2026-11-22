import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class GamePerformanceHistoryTests: XCTestCase {
    private func sample(_ intervals: [UInt64], sequence: UInt64 = 1,
                        pid: UInt32 = 321, width: UInt32 = 1920) throws -> GameFrameSample {
        let values = intervals.map(String.init).joined(separator: ",")
        let total = intervals.reduce(0, +)
        return try XCTUnwrap(GameFrameSample(line:
            "game-frame 2 neptune \(pid) 42 \(width) 1080 1 \(intervals.count) \(total) \(sequence) 1 \(values)"))
    }

    func testTrueFrameOrderAndKnownOnePercentLow() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        let intervals: [UInt64] = Array(repeating: 10_000_000, count: 98) + [20_000_000, 40_000_000]
        state.receive(try sample(intervals), at: 1)
        var history = try XCTUnwrap(state.snapshot(at: 1)?.history)
        XCTAssertEqual(history.frameTimesMS, Array(repeating: 10, count: 98) + [20, 40])
        XCTAssertEqual(history.sampleCount, 100)
        XCTAssertEqual(history.durationMS, 1040)
        XCTAssertEqual(history.minimumFrameTimeMS, 10)
        XCTAssertEqual(history.maximumFrameTimeMS, 40)
        XCTAssertEqual(try XCTUnwrap(history.onePercentLowFPS), 25, accuracy: 0.0001)

        state.receive(try sample([20_000_000], sequence: 2), at: 2)
        history = try XCTUnwrap(state.snapshot(at: 2)?.history)
        XCTAssertEqual(history.sampleCount, 101)
        XCTAssertEqual(try XCTUnwrap(history.onePercentLowFPS), 1000 / 30, accuracy: 0.0001,
                       "ceil(101 * 1%) takes both 40ms and 20ms, not the batch mean or a percentile")
    }

    func testOnePercentLowWaitsForOneHundredRealIntervals() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try sample(Array(repeating: 10_000_000, count: 99)), at: 1)
        XCTAssertNil(state.snapshot(at: 1)?.history?.onePercentLowFPS)
        state.receive(try sample([10_000_000], sequence: 2), at: 2)
        XCTAssertEqual(state.snapshot(at: 2)?.history?.onePercentLowFPS, 100)
    }

    func testLostBatchAndUnavailableBatchRestartHistoryWithoutInventingFrames() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try sample(Array(repeating: 10_000_000, count: 100)), at: 1)
        state.receive(try sample([20_000_000], sequence: 3), at: 2)
        XCTAssertEqual(state.snapshot(at: 2)?.history?.frameTimesMS, [20])
        XCTAssertNil(state.snapshot(at: 2)?.history?.onePercentLowFPS)
        let unavailable = try XCTUnwrap(GameFrameSample(line:
            "game-frame 2 neptune 321 42 1920 1080 1 300 250000000 4 0 -"))
        state.receive(unavailable, at: 3)
        XCTAssertEqual(state.current(at: 3)?.framesPerSecond, 1200)
        XCTAssertNil(state.snapshot(at: 3)?.history)
        state.receive(try sample([40_000_000], sequence: 5), at: 4)
        XCTAssertEqual(state.snapshot(at: 4)?.history?.frameTimesMS, [40])
    }

    func testOldPacketsDoNotRefreshOrClearARecentSource() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try sample([20_000_000], sequence: 5), at: 1)
        state.receive(try sample([1_000_000], sequence: 5), at: 2)
        state.receive(try sample([1_000_000], sequence: 4), at: 3)
        let staleLoss = try XCTUnwrap(GameFrameSample(line:
            "game-frame 2 neptune 321 42 1920 1080 0 0 0 4 0 -"))
        state.receive(staleLoss, at: 4)
        XCTAssertEqual(state.snapshot(at: 4)?.history?.frameTimesMS, [20])
        XCTAssertNil(state.snapshot(at: 2_000_000_002), "rejected packets must not extend freshness")
    }

    func testFocusLossExpiryAndToggleClearHistory() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try sample([10_000_000], sequence: 1), at: 1)
        let lost = try XCTUnwrap(GameFrameSample(line:
            "game-frame 2 neptune 321 42 1920 1080 0 0 0 2 0 -"))
        state.receive(lost, at: 2)
        XCTAssertNil(state.snapshot(at: 2))
        state.receive(try sample([20_000_000], sequence: 3), at: 3)
        XCTAssertEqual(state.snapshot(at: 3)?.history?.frameTimesMS, [20])
        XCTAssertNil(state.snapshot(at: 2_000_000_004))
        state.receive(try sample([30_000_000], sequence: 4), at: 2_000_000_005)
        XCTAssertEqual(state.snapshot(at: 2_000_000_005)?.history?.frameTimesMS, [30])
        state.setEnabled(false)
        state.setEnabled(true)
        XCTAssertNil(state.snapshot(at: 2_000_000_006))
        state.receive(try sample([40_000_000], sequence: 5), at: 2_000_000_007)
        XCTAssertEqual(state.snapshot(at: 2_000_000_007)?.history?.frameTimesMS, [40])
    }

    func testSourceSelectionNeverMixesOtherSwapchainHistory() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try sample([10_000_000, 20_000_000]), at: 1)
        XCTAssertEqual(state.snapshot(at: 1)?.history?.frameTimesMS, [10, 20])
        state.receive(try sample([50_000_000], pid: 322, width: 2560), at: 2)
        XCTAssertEqual(state.snapshot(at: 2)?.sample.source.processID, 322)
        XCTAssertEqual(state.snapshot(at: 2)?.history?.frameTimesMS, [50])
        let lost = try XCTUnwrap(GameFrameSample(line:
            "game-frame 2 neptune 322 42 2560 1080 0 0 0 2 0 -"))
        state.receive(lost, at: 3)
        XCTAssertEqual(state.snapshot(at: 3)?.sample.source.processID, 321)
        XCTAssertEqual(state.snapshot(at: 3)?.history?.frameTimesMS, [10, 20])
    }

    func testTenSecondAndSampleCountBoundsKeepOnlyWholeRecentIntervals() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try sample(Array(repeating: 100_000_000, count: 100)), at: 1)
        state.receive(try sample([200_000_000], sequence: 2), at: 2)
        var history = try XCTUnwrap(state.snapshot(at: 2)?.history)
        XCTAssertEqual(history.sampleCount, 99)
        XCTAssertEqual(history.durationMS, 10_000)
        XCTAssertEqual(history.frameTimesMS.last, 200)
        state.receive(try sample([10_000_000_001], sequence: 3), at: 3)
        XCTAssertNil(state.snapshot(at: 3)?.history, "a frame longer than the window is not shortened")
        for sequence in 4...43 {
            state.receive(try sample(Array(repeating: 1_000_000, count: 128),
                                     sequence: UInt64(sequence)), at: UInt64(sequence))
        }
        history = try XCTUnwrap(state.snapshot(at: 43)?.history)
        XCTAssertEqual(history.sampleCount, 4096)
        XCTAssertEqual(history.durationMS, 4096)
    }

    func testLegacyMeanRemainsAvailableWithoutAFrameCurve() throws {
        let old = try XCTUnwrap(GameFrameSample(line:
            "game-frame 1 neptune 321 42 1920 1080 1 15 250000000"))
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(old, at: 1)
        XCTAssertEqual(state.snapshot(at: 1)?.sample.framesPerSecond, 60)
        XCTAssertNil(state.snapshot(at: 1)?.history)
    }

    func testMalformedFrameArraysAndOverflowAreRejected() throws {
        let valid = "game-frame 2 neptune 321 42 1920 1080 1 2 30000000 1 1 10000000,20000000"
        XCTAssertNotNil(GameFrameSample(line: valid))
        let invalid = [
            valid.replacingOccurrences(of: "10000000,20000000", with: "10000000"),
            valid.replacingOccurrences(of: "10000000,20000000", with: "10000000,20000000,"),
            valid.replacingOccurrences(of: "10000000,20000000", with: "10000000,0"),
            valid.replacingOccurrences(of: "10000000,20000000", with: "10000000,-1"),
            valid.replacingOccurrences(of: "10000000,20000000", with: "10000000,+20000000"),
            valid.replacingOccurrences(of: "30000000", with: "30000001"),
            valid.replacingOccurrences(of: " 1 1 10000000", with: " 0 1 10000000"),
            valid.replacingOccurrences(of: " 1 1 10000000", with: " 1 0 10000000"),
            "game-frame 2 neptune 321 42 1920 1080 1 2 18446744073709551615 1 1 18446744073709551615,1",
            "game-frame 2 neptune 321 42 1920 1080 1 129 129 1 1 " + Array(repeating: "1", count: 129).joined(separator: ","),
            "game-frame 2 neptune 321 42 1920 1080 0 2 30000000 1 0 -",
            valid + " extra", valid + "\n", String(repeating: "x", count: 4097),
        ]
        for line in invalid { XCTAssertNil(GameFrameSample(line: line), "accepted \(line)") }
    }

    @MainActor
    func testProgressTransportPreservesACompleteLargeBatchAcrossFragments() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ports = try VMPorts(runDirectory: directory)
        defer { ports.close() }
        var received: [GameFrameSample] = []
        try ports.start(onReady: {}, onShutdown: {}, onFrameDump: { _ in },
                        onPointerFocus: { _ in }, onGameFrame: { received.append($0) })
        let values = Array(repeating: "2000000", count: 128).joined(separator: ",")
        let line = "game-frame 2 vulkan 321 42 1280 720 1 128 256000000 1 1 \(values)"
        let data = Array((line + "\n").utf8)
        for offset in stride(from: 0, to: data.count, by: 97) {
            ports.readProgress(Array(data[offset..<min(offset + 97, data.count)]))
        }
        XCTAssertEqual(received.count, 1)
        XCTAssertEqual(received.first?.frameIntervalsNS, Array(repeating: 2_000_000, count: 128))
        XCTAssertEqual(try String(contentsOf: directory.appendingPathComponent("progress.log"), encoding: .utf8), "")
    }
}
