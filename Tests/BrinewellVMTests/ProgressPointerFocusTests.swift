import AppKit
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class ProgressPointerFocusTests: XCTestCase {
    @MainActor
    func testProgressFramingFocusAndClose() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let ports = try VMPorts(runDirectory: directory)
        defer { ports.close() }
        var focus: [String] = []
        var readyCount = 0
        try ports.start(onReady: { readyCount += 1 }, onShutdown: {}, onFrameDump: { _ in },
                        onPointerFocus: { focus.append($0) })

        ports.readProgress(Array("unknown\nalive\nfocus steam 1920x".utf8))
        XCTAssertTrue(focus.isEmpty)
        ports.readProgress(Array("1200\nready\nfocus game 42 1600x900\nfocus desktop 1280x800\nfocus desktop\n".utf8))
        XCTAssertEqual(focus, ["focus steam 1920x1200", "focus game 42 1600x900",
                               "focus desktop 1280x800", "focus desktop"])
        XCTAssertEqual(readyCount, 1)

        ports.readProgress(Array(("focus " + String(repeating: "x", count: 65536)).utf8))
        ports.readProgress(Array("\nfocus steam 1280x800\n".utf8))
        XCTAssertEqual(focus, ["focus steam 1920x1200", "focus game 42 1600x900",
                               "focus desktop 1280x800", "focus desktop", "focus steam 1280x800"])
        XCTAssertEqual(readyCount, 1)

        ports.close()
        ports.readProgress(Array("focus desktop\nready\n".utf8))
        XCTAssertEqual(focus.count, 5)
        XCTAssertEqual(readyCount, 1)
    }
    private func drainFrames(_ device: VMInputDevice) -> [VMInputEvent] {
        var events: [VMInputEvent] = []
        while let event = device.nextEvent() { events.append(event) }
        return events
    }

    @MainActor
    func testProgressFocusRoutesRealPointerInput() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let ports = try VMPorts(runDirectory: directory)
        defer { ports.close() }
        let input = try VMInput()
        input.setActive(true)
        try ports.start(onReady: {}, onShutdown: {}, onFrameDump: { _ in },
                        onPointerFocus: { input.updateGuestFocus($0) })

        let syn = VMInputEvent(0, 0, 0)
        let motion = [VMInputEvent(2, 0, 9), VMInputEvent(2, 1, -6), syn]

        ports.readProgress(Array("focus steam 1920x".utf8))
        ports.readProgress(Array("1200\n".utf8))
        input.move(deltaX: 9, deltaY: -6)
        input.button(0, down: true)
        XCTAssertEqual(drainFrames(input.mouse), motion + [VMInputEvent(1, 272, 1), syn])

        ports.readProgress(Array("focus desktop 1280x800\n".utf8))
        let steamRelease = drainFrames(input.mouse)
        XCTAssertTrue(steamRelease.contains(VMInputEvent(1, 272, 0)))
        XCTAssertEqual(steamRelease.last, syn)
        XCTAssertFalse(steamRelease.contains { $0.type == 2 })
        input.button(0, down: true)
        XCTAssertEqual(drainFrames(input.mouse), [VMInputEvent(1, 272, 1), syn])

        ports.readProgress(Array("focus desktop\n".utf8))
        let desktopRelease = drainFrames(input.mouse)
        XCTAssertTrue(desktopRelease.contains(VMInputEvent(1, 272, 0)))
        XCTAssertEqual(desktopRelease.last, syn)
        XCTAssertFalse(desktopRelease.contains { $0.type == 2 })
        input.move(deltaX: 9, deltaY: -6)
        input.button(0, down: true)
        input.scroll(deltaX: 1, deltaY: 1, precise: false)
        XCTAssertEqual(drainFrames(input.mouse), [])

        ports.readProgress(Array("focus steam 1920x1200\n".utf8))
        input.move(deltaX: 9, deltaY: -6)
        input.button(0, down: true)
        XCTAssertEqual(drainFrames(input.mouse), motion + [VMInputEvent(1, 272, 1), syn])
    }
}
