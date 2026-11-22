import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class RuntimeLogTests: XCTestCase {
    func testCapturesReportsNativeOutputAndInheritedChildOutput() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("host.log")
        let previousHandler = reportHandler
        defer { reportHandler = previousHandler }
        let received = expectation(description: "report reaches launcher")
        reportHandler = { line in
            if line == "BRINEWELL test line" { received.fulfill() }
        }
        let log = try RuntimeLog(url: path)
        defer { log.restore() }
        report("BRINEWELL test\nline")
        let native = Array("native stderr\n".utf8)
        _ = native.withUnsafeBytes { Darwin.write(STDERR_FILENO, $0.baseAddress, $0.count) }
        let child = Process()
        child.executableURL = URL(fileURLWithPath: "/bin/sh")
        child.arguments = ["-c", "printf 'renderer stdout\\n'; printf 'renderer stderr\\n' >&2"]
        try child.run()
        child.waitUntilExit()
        let childStatus = child.terminationStatus
        log.restore()
        log.restore() // Restoration must be idempotent.
        wait(for: [received], timeout: 1)
        XCTAssertEqual(childStatus, 0)
        let text = try String(contentsOf: path, encoding: .utf8)
        XCTAssertTrue(text.contains("BRINEWELL test line\n"))
        XCTAssertTrue(text.contains("native stderr\n"))
        XCTAssertTrue(text.contains("renderer stdout\n"))
        XCTAssertTrue(text.contains("renderer stderr\n"))
        let attributes = try FileManager.default.attributesOfItem(atPath: path.path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o600)
    }

    func testUnwritableLogLeavesStandardDescriptorsUnchanged() {
        var outputBefore = stat(), errorBefore = stat()
        XCTAssertEqual(fstat(STDOUT_FILENO, &outputBefore), 0)
        XCTAssertEqual(fstat(STDERR_FILENO, &errorBefore), 0)
        XCTAssertThrowsError(try RuntimeLog(url: URL(fileURLWithPath: "/nonexistent-\(UUID().uuidString)/host.log")))
        var outputAfter = stat(), errorAfter = stat()
        XCTAssertEqual(fstat(STDOUT_FILENO, &outputAfter), 0)
        XCTAssertEqual(fstat(STDERR_FILENO, &errorAfter), 0)
        XCTAssertEqual(outputBefore.st_ino, outputAfter.st_ino)
        XCTAssertEqual(outputBefore.st_dev, outputAfter.st_dev)
        XCTAssertEqual(errorBefore.st_ino, errorAfter.st_ino)
        XCTAssertEqual(errorBefore.st_dev, errorAfter.st_dev)
    }
}
