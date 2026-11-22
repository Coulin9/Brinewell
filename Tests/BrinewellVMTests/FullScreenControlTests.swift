import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class FullScreenControlTests: XCTestCase {
    @MainActor
    func testFullScreenCommandsAreExplicitAndStopAfterClose() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ports = try VMPorts(runDirectory: directory)
        defer { ports.close() }
        var requests: [Bool] = []
        try ports.start(onReady: {}, onShutdown: {}, onFrameDump: { _ in }, onPointerFocus: { _ in },
                        onFullScreen: { requests.append($0) })
        ports.control("fullscreen on")
        ports.control("fullscreen on")
        ports.control("fullscreen off")
        XCTAssertEqual(requests, [true, true, false])
        for invalid in ["fullscreen", "fullscreen toggle", "fullscreen on extra", "fullscreen 1"] {
            ports.control(invalid)
        }
        XCTAssertEqual(requests, [true, true, false])
        ports.close()
        ports.control("fullscreen on")
        XCTAssertEqual(requests, [true, true, false])
    }

    @MainActor
    func testFullScreenFailureDoesNotPreventShutdown() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ports = try VMPorts(runDirectory: directory)
        defer { ports.close() }
        var stopped = false
        try ports.start(onReady: {}, onShutdown: { stopped = true }, onFrameDump: { _ in },
                        onPointerFocus: { _ in }, onFullScreen: { _ in throw RuntimeError("window unavailable") })
        ports.control("fullscreen on")
        XCTAssertFalse(stopped)
        ports.control("shutdown")
        XCTAssertTrue(stopped)
    }
}
