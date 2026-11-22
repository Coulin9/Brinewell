import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class GamepadInputTests: XCTestCase {
    func testAxisConversions() {
        XCTAssertEqual(GamepadState.stick(-2), -32768)
        XCTAssertEqual(GamepadState.stick(2), 32767)
        XCTAssertEqual(GamepadState.stick(1, inverted: true), -32768)
        XCTAssertEqual(GamepadState.stick(-1, inverted: true), 32767)
        XCTAssertEqual(GamepadState.trigger(-1), 0)
        XCTAssertEqual(GamepadState.trigger(2), 255)
        XCTAssertEqual(GamepadState.hat(1, inverted: true), -1)
        XCTAssertEqual(GamepadState.hat(-1, inverted: true), 1)
        XCTAssertEqual(GamepadState.hat(0), 0)
    }

    @MainActor
    func testControlMapsAxesAndPreservesButtonsAndDpad() throws {
        var output = [UInt8]()
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            output.append(contentsOf: bytes)
            return bytes.count
        })
        defer { ports.close() }
        ports.readPad(Array("hello\n".utf8))
        let cases = [
            ("pad axis lx -32768", "3:0:-32768", "3:0:0"),
            ("pad axis ly 32767 2000", "3:1:32767", "3:1:0"),
            ("pad axis rx 12000 50", "3:3:12000", "3:3:0"),
            ("pad axis ry -12000 150", "3:4:-12000", "3:4:0"),
            ("pad axis lx 0", "3:0:0", "3:0:0"),
            ("pad press guide", "1:316:1", "1:316:0"),
            ("pad press a 50", "1:304:1", "1:304:0"),
            ("pad dpad left", "3:16:-1", "3:16:0"),
            ("pad dpad down 2000", "3:17:1", "3:17:0"),
        ]
        for (command, start, end) in cases {
            output.removeAll()
            ports.control(command)
            XCTAssertNotNil(ports.input.script, command)
            ports.control("pad neutral")
            XCTAssertEqual(String(decoding: output, as: UTF8.self), "ev \(start)\nev \(end)\n", command)
            XCTAssertNil(ports.input.script, command)
        }
    }

    @MainActor
    func testAxisControlRejectsInvalidInputAndWaitsForGuest() throws {
        var output = [UInt8]()
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            output.append(contentsOf: bytes)
            return bytes.count
        })
        defer { ports.close() }
        ports.control("pad axis ly -32768 150")
        XCTAssertTrue(output.isEmpty)
        XCTAssertNil(ports.input.script)
        ports.readPad(Array("hello\n".utf8))
        output.removeAll()
        let token = ports.input.token
        for command in [
            "pad axis", "pad axis lx", "pad axis unknown 100", "pad axis 0 100",
            "pad axis lx -32769", "pad axis ly 32768", "pad axis rx 0.5",
            "pad axis ry NaN", "pad axis lx 999999999999999999999999999999",
            "pad axis lx 100 49", "pad axis lx 100 2001", "pad axis lx 100 invalid",
            "pad axis lx 100 150 extra",
        ] {
            ports.control(command)
            XCTAssertNil(ports.input.script, command)
            XCTAssertEqual(ports.input.token, token, command)
        }
        XCTAssertTrue(output.isEmpty)
    }

    @MainActor
    func testAxisControlAutomaticallyReturnsToNeutral() async throws {
        var output = [UInt8]()
        let released = expectation(description: "axis automatically returns to neutral")
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            output.append(contentsOf: bytes)
            if String(decoding: bytes, as: UTF8.self) == "ev 3:1:0\n" { released.fulfill() }
            return bytes.count
        })
        defer { ports.close() }
        ports.readPad(Array("hello\n".utf8))
        output.removeAll()
        ports.control("pad axis ly -32768 50")
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "ev 3:1:-32768\n")
        await fulfillment(of: [released], timeout: 1)
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "ev 3:1:-32768\nev 3:1:0\n")
        XCTAssertNil(ports.input.script)
    }

    @MainActor
    func testAxisControlKeepsSingleActionAndPhysicalPriority() throws {
        var output = [UInt8]()
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            output.append(contentsOf: bytes)
            return bytes.count
        })
        defer { ports.close() }
        ports.readPad(Array("hello\n".utf8))
        output.removeAll()
        ports.control("pad axis lx 15000 2000")
        let token = ports.input.token
        ports.control("pad axis ry -12000")
        ports.control("pad press a")
        XCTAssertEqual(ports.input.token, token)
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "ev 3:0:15000\n")
        ports.input.select(true)
        ports.input.setActive(true)
        XCTAssertNil(ports.input.script)
        let frames = String(decoding: output, as: UTF8.self).split(separator: "\n").map(String.init)
        XCTAssertEqual(Array(frames.prefix(2)), ["ev 3:0:15000", "ev 3:0:0"])
        assertNeutral(try XCTUnwrap(frames.last))
        let count = output.count
        ports.control("pad axis ry -12000")
        ports.input.release(token)
        XCTAssertEqual(output.count, count)
    }

    @MainActor
    func testAxisNeutralSurvivesBackpressureAndStaleRelease() throws {
        var blocked = false
        var output = [UInt8]()
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            if blocked { errno = EAGAIN; return -1 }
            output.append(contentsOf: bytes)
            return bytes.count
        })
        defer { ports.close() }
        ports.readPad(Array("hello\n".utf8))
        output.removeAll()
        blocked = true
        ports.control("pad axis rx 10000 2000")
        let token = ports.input.token
        ports.control("pad neutral")
        XCTAssertNil(ports.input.script)
        blocked = false
        ports.flush()
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "ev 3:3:10000\nev 3:3:0\n")
        ports.control("pad axis ry -10000 2000")
        let count = output.count
        ports.input.release(token)
        XCTAssertEqual(output.count, count)
        XCTAssertEqual(ports.input.script?.code, 4)
        ports.control("pad neutral")
        XCTAssertTrue(String(decoding: output, as: UTF8.self).hasSuffix("ev 3:4:0\n"))
    }

    @MainActor
    func testFocusAndDisconnectEmitCompleteNeutralFrames() {
        var frames: [String] = []
        let input = GamepadInputState(emit: { state, _ in frames.append(state.frame) },
                                      sendScript: { _, _ in true }, cancelRelease: {})
        var state = GamepadState()
        state.pressed = [304, 305, 318]
        state.values = [0: -32768, 1: 32767, 2: 255, 17: -1]
        input.select(true, state: state)
        XCTAssertTrue(frames.isEmpty)
        input.setActive(true)
        XCTAssertTrue(frames.last!.contains("1:304:1 1:305:1"))
        XCTAssertTrue(frames.last!.contains("3:0:-32768 3:1:32767 3:2:255"))
        input.setActive(false)
        assertNeutral(frames.last!)
        input.setActive(true)
        XCTAssertTrue(frames.last!.contains("1:318:1"))
        input.select(false)
        assertNeutral(frames.last!)
    }

    @MainActor
    func testPhysicalTakeoverCancelsScriptAndRejectsOverrides() {
        var output: [String] = []
        var cancellations = 0
        let input = GamepadInputState(emit: { state, _ in output.append(state.frame) },
                                      sendScript: { line, _ in output.append(line); return true },
                                      cancelRelease: { cancellations += 1 })
        let token = input.press(type: 1, code: 304, value: 1)!
        input.setActive(true)
        input.select(true)
        XCTAssertEqual(output[0], "ev 1:304:1")
        XCTAssertEqual(output[1], "ev 1:304:0")
        XCTAssertEqual(cancellations, 1)
        let count = output.count
        input.release(token)
        XCTAssertNil(input.press(type: 3, code: 16, value: 1))
        XCTAssertFalse(input.neutralScript())
        XCTAssertEqual(output.count, count)
        input.select(false)
        let replacement = input.press(type: 3, code: 16, value: -1)!
        XCTAssertEqual(output.last, "ev 3:16:-1")
        let replacementCount = output.count
        input.release(token)
        XCTAssertEqual(input.script?.type, 3)
        XCTAssertEqual(input.script?.code, 16)
        XCTAssertEqual(output.count, replacementCount)
        input.release(replacement)
        XCTAssertEqual(output.last, "ev 3:16:0")
    }

    @MainActor
    func testPendingPhysicalReleaseBlocksScriptUntilFlushed() {
        var pending: GamepadState?
        var output: [String] = []
        let input = GamepadInputState(emit: { state, _ in pending = state },
                                      sendScript: { line, _ in output.append(line); return true },
                                      cancelRelease: {}, physicalPending: { pending != nil })
        var held = GamepadState()
        held.pressed.insert(304)
        input.select(true, state: held)
        input.setActive(true)
        pending = nil // The selected frame has entered the output stream.
        input.setActive(false)
        XCTAssertNotNil(pending)
        XCTAssertNil(input.press(type: 1, code: 305, value: 1))
        XCTAssertFalse(input.neutralScript())
        XCTAssertTrue(output.isEmpty)
        output.append(pending!.frame)
        pending = nil // Simulate the pending frame entering the output stream.
        XCTAssertNotNil(input.press(type: 1, code: 305, value: 1))
        assertNeutral(output[0])
        XCTAssertEqual(output[1], "ev 1:305:1")
    }

    @MainActor
    func testReplacementPreservesNeutralUnderBackpressure() throws {
        var blocked = false
        var output = [UInt8]()
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            if blocked { errno = EAGAIN; return -1 }
            output.append(contentsOf: bytes)
            return bytes.count
        })
        defer { ports.close() }
        ports.readPad(Array("hello\n".utf8))
        output.removeAll()
        blocked = true
        let input = GamepadInputState(
            emit: { state, reset in ports.queuePhysical(state, reset: reset) },
            sendScript: { _, _ in true }, cancelRelease: {}
        )
        var held = GamepadState()
        held.pressed.insert(304)
        held.values[0] = 32767
        input.setActive(true)
        input.select(true, state: held)
        input.select(false)
        input.select(true, state: held)
        blocked = false
        ports.flush()
        let frames = String(decoding: output, as: UTF8.self).split(separator: "\n").map(String.init)
        XCTAssertEqual(frames.count, 3)
        guard frames.count == 3 else { return }
        XCTAssertTrue(frames[0].contains("1:304:1"))
        assertNeutral(frames[1])
        XCTAssertTrue(frames[2].contains("1:304:1"))
    }

    @MainActor
    func testPartialWritesPreserveHandshakeAndSnapshotOrder() throws {
        var output = [UInt8]()
        var calls = 0
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            calls += 1
            if calls.isMultiple(of: 11) { errno = EAGAIN; return -1 }
            if calls.isMultiple(of: 7) { errno = EINTR; return -1 }
            let count = min(7, bytes.count)
            output.append(contentsOf: bytes.prefix(count))
            return count
        })
        defer { ports.close() }
        var state = GamepadState()
        state.pressed.insert(304)
        ports.input.select(true, state: state)
        ports.input.setActive(true)

        ports.readPad(Array("hello\n".utf8))
        for _ in 0..<200 { ports.flush() }
        ports.readPad(Array("hello\n".utf8))
        for _ in 0..<200 { ports.flush() }

        let text = String(decoding: output, as: UTF8.self)
        XCTAssertTrue(text.hasSuffix("\n"))
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        XCTAssertEqual(lines.count, 5)
        guard lines.count == 5 else { return }
        let create = "create 0003 045e 028e 0114 304,305,307,308,310,311,314,315,316,317,318 0:-32768:32767:16:128,1:-32768:32767:16:128,2:0:255:0:0,3:-32768:32767:16:128,4:-32768:32767:16:128,5:0:255:0:0,16:-1:1:0:0,17:-1:1:0:0 Microsoft X-Box 360 pad"
        XCTAssertEqual(lines, [create, state.frame, create, state.frame, ""])
    }

    @MainActor
    func testClosedPortsIgnoreInputAndControl() throws {
        var writes = 0
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory, padWriter: { bytes in
            writes += 1
            return bytes.count
        })
        ports.readPad(Array("hello\n".utf8))
        ports.close()
        let count = writes
        let token = ports.input.token
        ports.readPad(Array("hello\n".utf8))
        ports.queuePhysical(GamepadState(), reset: true)
        ports.control("pad press a")
        ports.control("pad axis ly -32768")
        ports.control("shutdown")
        ports.flush()
        XCTAssertEqual(writes, count)
        XCTAssertNil(ports.input.script)
        XCTAssertEqual(ports.input.token, token)
    }

    private func assertNeutral(_ frame: String, file: StaticString = #filePath, line: UInt = #line) {
        let events = Set(frame.split(separator: " ").dropFirst().map(String.init))
        for code in GamepadState.buttons { XCTAssertTrue(events.contains("1:\(code):0"), file: file, line: line) }
        for code in GamepadState.axes { XCTAssertTrue(events.contains("3:\(code):0"), file: file, line: line) }
    }
}
