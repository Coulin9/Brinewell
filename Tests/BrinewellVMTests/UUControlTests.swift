import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class UUControlTests: XCTestCase {
    @MainActor
    func testInstallRequiresGuestHandshakeAndStopsAfterClose() throws {
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory)
        defer { ports.close() }
        XCTAssertThrowsError(try ports.sendUU(.install))
        ports.readUU(Array("hel".utf8))
        XCTAssertThrowsError(try ports.sendUU(.status))
        ports.readUU(Array("lo\n".utf8))
        XCTAssertNoThrow(try ports.sendUU(.status))
        XCTAssertNoThrow(try ports.sendUU(.install))
        ports.close()
        XCTAssertThrowsError(try ports.sendUU(.install))
        ports.readUU(Array("hello\n".utf8))
        XCTAssertThrowsError(try ports.sendUU(.status))
    }

    @MainActor
    func testGuestEventsSurviveSplitReadsAndDiscardOversizedLines() throws {
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory)
        defer { ports.close() }
        let previous = reportHandler
        defer { reportHandler = previous }
        let hello = expectation(description: "guest channel ready")
        let progress = expectation(description: "split progress reaches launcher")
        let status = expectation(description: "valid status after oversized line")
        hello.assertForOverFulfill = true
        progress.assertForOverFulfill = true
        status.assertForOverFulfill = true
        reportHandler = { line in
            switch line {
            case "BRINEWELL uu hello": hello.fulfill()
            case "BRINEWELL uu progress 50 Installing": progress.fulfill()
            case "BRINEWELL uu status installed Ready": status.fulfill()
            default: XCTFail("Unexpected UU event: \(line.prefix(80))")
            }
        }
        ports.readUU(Array("hello\nprogress 50 Ins".utf8))
        ports.readUU(Array("talling\n".utf8))
        ports.readUU(Array(String(repeating: "x", count: 65537).utf8))
        ports.readUU(Array("\nstatus installed Ready\n".utf8))
        wait(for: [hello, progress, status], timeout: 1)
    }

    @MainActor
    func testFIFOAcceptsOnlyExactUUCommands() throws {
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory)
        defer { ports.close() }
        ports.readUU(Array("hello\n".utf8))
        let previous = reportHandler
        defer { reportHandler = previous }
        let accepted = expectation(description: "known command accepted")
        accepted.expectedFulfillmentCount = 2
        let rejected = expectation(description: "unknown command rejected")
        rejected.expectedFulfillmentCount = 5
        reportHandler = { line in
            switch line {
            case "BRINEWELL control ok uu install requested", "BRINEWELL control ok uu status requested":
                accepted.fulfill()
            case "BRINEWELL control error invalid command": rejected.fulfill()
            default: XCTFail("Unexpected control response: \(line)")
            }
        }
        ports.control("uu install")
        ports.control("uu status")
        for command in ["uu", "uu install extra", "uu exec id", "uu  install", "uu install\nstatus"] {
            ports.control(command)
        }
        wait(for: [accepted, rejected], timeout: 1)
    }
}
