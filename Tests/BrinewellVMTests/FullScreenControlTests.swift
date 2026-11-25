import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class FullScreenControlTests: XCTestCase {
    @MainActor
    func testCloseDrainsPendingShutdownWithoutReentering() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let ports = try VMPorts(runDirectory: directory)
        var shutdowns = 0
        try ports.start(onReady: {}, onShutdown: { shutdowns += 1; ports.close() },
                        onFrameDump: { _ in }, onPointerFocus: { _ in })
        let fd = open(directory.appendingPathComponent("control.fifo").path, O_WRONLY | O_NONBLOCK)
        XCTAssertGreaterThanOrEqual(fd, 0)
        guard fd >= 0 else { ports.close(); return }
        defer { Darwin.close(fd) }
        let bytes = Array("shutdown\npartial".utf8)
        XCTAssertEqual(bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress, $0.count) }, bytes.count)
        ports.close()
        XCTAssertEqual(shutdowns, 1)
        XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("control.fifo").path))
    }

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
