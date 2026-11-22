import AppKit
import BrinewellCore
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class GamePerformanceHUDTests: XCTestCase {
    private let line = "game-frame 1 neptune 321 42 1920 1080 1 15 250000000"

    func testPresentIntervalsProduceGameFPSAndFrameTime() throws {
        let sample = try XCTUnwrap(GameFrameSample(line: line))
        XCTAssertEqual(sample.source.processID, 321)
        XCTAssertEqual(sample.source.streamID, 42)
        XCTAssertEqual(sample.framesPerSecond, 60, accuracy: 0.001)
        XCTAssertEqual(sample.frameTimeMS, 1000 / 60, accuracy: 0.001)
        XCTAssertEqual(sample.area, 1920 * 1080)
        XCTAssertNotNil(GameFrameSample(line: "game-frame 1 vulkan 321 42 1920 1080 0 0 0"))
    }

    func testMalformedOrUnrepresentableSamplesAreRejected() {
        let fields = line.components(separatedBy: " ")
        let invalidFields: [(Int, String)] = [
            (0, "other"), (1, "2"), (2, "metal"), (3, "0"), (3, "2147483648"),
            (3, "+321"), (3, "-1"), (4, "0"), (4, "18446744073709551616"),
            (5, "0"), (5, "65536"), (6, "-1"), (7, "2"), (8, "0"),
            (8, "NaN"), (8, "18446744073709551616"), (9, "0"), (9, "inf"),
            (9, "18446744073709551616"),
        ]
        for (index, invalid) in invalidFields {
            var words = fields
            words[index] = invalid
            XCTAssertNil(GameFrameSample(line: words.joined(separator: " ")), "accepted \(words)")
        }
        for invalid in [line + " extra", " " + line, line + "\n", line.replacingOccurrences(of: " ", with: "\t"),
                        String(repeating: "x", count: 257), fields.dropLast().joined(separator: " ")] {
            XCTAssertNil(GameFrameSample(line: invalid))
        }
    }

    func testStreamsAreSelectedByAreaWithoutMixingIntervals() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        let first = try XCTUnwrap(GameFrameSample(line: line))
        let equal = try XCTUnwrap(GameFrameSample(line: "game-frame 1 vulkan 322 42 1920 1080 1 30 250000000"))
        let larger = try XCTUnwrap(GameFrameSample(line: "game-frame 1 neptune 321 43 2560 1440 1 25 250000000"))
        state.receive(first, at: 1)
        XCTAssertEqual(state.current(at: 1), first)
        state.receive(equal, at: 2)
        XCTAssertEqual(state.current(at: 2), first, "equal area must keep the current source")
        state.receive(larger, at: 3)
        let chosen = try XCTUnwrap(state.current(at: 3))
        XCTAssertEqual(chosen.source, larger.source)
        XCTAssertEqual(chosen.framesPerSecond, 100, "do not add the other sources' frames")
        XCTAssertEqual(chosen.frameTimeMS, 10)
    }

    func testFocusLossAndExpirationRemoveOldNumbers() throws {
        var state = GamePerformanceState()
        state.setEnabled(true)
        let sample = try XCTUnwrap(GameFrameSample(line: line))
        state.receive(sample, at: 10)
        XCTAssertNotNil(state.current(at: 2_000_000_010))
        XCTAssertNil(state.current(at: 2_000_000_011))
        state.receive(sample, at: 3_000_000_000)
        XCTAssertNotNil(state.current(at: 3_000_000_000))
        let unfocused = try XCTUnwrap(GameFrameSample(line: "game-frame 1 neptune 321 42 1920 1080 0 0 0"))
        state.receive(unfocused, at: 3_000_000_001)
        XCTAssertNil(state.current(at: 3_000_000_001))
    }

    func testRuntimeToggleRequiresFreshDataAfterReenable() throws {
        var state = GamePerformanceState()
        let sample = try XCTUnwrap(GameFrameSample(line: line))
        state.receive(sample, at: 1)
        XCTAssertNil(state.current(at: 1))
        state.setEnabled(true)
        XCTAssertNil(state.current(at: 2))
        state.receive(sample, at: 3)
        XCTAssertEqual(state.current(at: 3), sample)
        state.setEnabled(false)
        XCTAssertNil(state.current(at: 4))
        state.setEnabled(true)
        XCTAssertNil(state.current(at: 5))
        state.receive(sample, at: 6)
        XCTAssertEqual(state.current(at: 6), sample)
    }

    @MainActor
    func testProgressFramingRoutesSamplesWithoutLoggingEachFrame() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ports = try VMPorts(runDirectory: directory)
        defer { ports.close() }
        var samples: [GameFrameSample] = []
        var ready = 0
        var focus: [String] = []
        try ports.start(onReady: { ready += 1 }, onShutdown: {}, onFrameDump: { _ in },
                        onPointerFocus: { focus.append($0) }, onGameFrame: { samples.append($0) })
        ports.readProgress(Array("ready\ngame-fr".utf8))
        ports.readProgress(Array("ame 1 neptune 321 42 1920 1080 1 15 ".utf8))
        XCTAssertTrue(samples.isEmpty)
        ports.readProgress(Array("250000000\ngame-frame invalid\nfocus steam 1920x1080\n".utf8))
        XCTAssertEqual(samples, [try XCTUnwrap(GameFrameSample(line: line))])
        XCTAssertEqual(ready, 1)
        XCTAssertEqual(focus, ["focus steam 1920x1080"])
        XCTAssertEqual(try String(contentsOf: directory.appendingPathComponent("progress.log"), encoding: .utf8),
                       "ready\nfocus steam 1920x1080\n")
        ports.close()
        ports.readProgress(Array((line + "\n").utf8))
        XCTAssertEqual(samples.count, 1)
    }

    @MainActor
    func testHUDIsNonInteractiveAndClearsRenderedNumbers() throws {
        let hud = GamePerformanceHUD(frame: .zero)
        var state = GamePerformanceState()
        state.setEnabled(true)
        state.receive(try XCTUnwrap(GameFrameSample(line: line)), at: 1)
        hud.update(.make(level: .compact, game: state.snapshot(at: 1), hardware: nil, refreshRate: 120))
        XCTAssertEqual(hud.presentation.headline, "60.0 FPS")
        XCTAssertNil(hud.hitTest(.zero))
        hud.update(.make(level: .compact, game: nil, hardware: nil, refreshRate: 120))
        XCTAssertEqual(hud.presentation.headline, "No frames yet")
        XCTAssertTrue(hud.presentation.frameTimes.isEmpty)
        XCTAssertTrue(hud.presentation.rows.contains { $0.title == "Game Frame Time" && $0.value == "—" })
    }
}
