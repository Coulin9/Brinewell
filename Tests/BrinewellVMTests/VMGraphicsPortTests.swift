import BrinewellCore
import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

private actor GraphicsTestResolver: GameGraphicsBackendResolving {
    private var continuations: [UInt32: CheckedContinuation<GraphicsBackend, Never>] = [:]
    private(set) var calls: [UInt32] = []

    func resolveBackend(for appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GraphicsBackend {
        calls.append(appID)
        return await withCheckedContinuation { continuations[appID] = $0 }
    }

    func complete(_ appID: UInt32, _ backend: GraphicsBackend) {
        continuations.removeValue(forKey: appID)?.resume(returning: backend)
    }
}

private struct ProbeTestResolver: GameGraphicsBackendResolving {
    func resolveBackend(for appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GraphicsBackend { .venus }
    func resolveForGuest(appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GameGraphicsResolution {
        .probeLocal(allowMullion: availableBackends.contains(.mullion))
    }
}

@MainActor
final class VMGraphicsPortTests: XCTestCase {
    private let first = "0123456789abcdef0123456789abcdef"
    private let second = "abcdef0123456789abcdef0123456789"

    private func receive(_ line: String, _ ports: VMPorts) {
        ports.readGraphics(Array((line + "\n").utf8))
    }

    func testProbeReplies() async throws {
        for allow in [true, false] {
            var output = [UInt8]()
            let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
                graphicsWriter: { bytes in output += bytes; return bytes.count }, graphicsResolver: ProbeTestResolver(),
                availableBackends: allow ? [.automatic, .venus, .mullion] : [.automatic, .venus])
            try ports.setGraphicsBackend(.automatic)
            receive("ready", ports)
            receive("resolve \(first) 730", ports)
            for _ in 0..<100 where !String(decoding: output, as: UTF8.self).contains("resolved ") { await Task.yield() }
            XCTAssertTrue(String(decoding: output, as: UTF8.self).contains("resolved \(first) \(allow ? "probe" : "probe_nomullion")\n"))
            ports.close()
        }
    }

    func testResolutionAndReconnect() async throws {
        let resolver = GraphicsTestResolver()
        var output = [UInt8](), applied: [GraphicsBackend] = []
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in output += bytes; return bytes.count },
            graphicsResolver: resolver, availableBackends: [.automatic, .neptune, .mullion, .venus])
        defer { ports.close() }
        ports.onGraphicsApplied = { applied.append($0) }
        try ports.setGraphicsBackend(.automatic)
        receive("ready", ports)
        ports.sendDisplayHDR(supported: true, maxLuminanceNits: 1000,
                             maxFrameAverageLuminanceNits: 500, minLuminance10000ths: 1, sdrReferenceNits: 200)
        receive("applied automatic", ports)
        receive("resolve \(first) 11", ports)
        receive("resolve \(second) 12", ports)
        receive("resolve \(first) 11", ports)
        for _ in 0..<100 where await resolver.calls.count < 2 { await Task.yield() }
        let calls = await resolver.calls
        XCTAssertEqual(calls.count, 2)
        await resolver.complete(12, .mullion)
        await Task.yield()
        await resolver.complete(11, .neptune)
        for _ in 0..<100 where !String(decoding: output, as: UTF8.self).contains("resolved \(first)") { await Task.yield() }
        let text = String(decoding: output, as: UTF8.self)
        XCTAssertTrue(text.contains("backend automatic\n"))
        XCTAssertTrue(text.contains("hdr 1 1000 500 1 200\n"))
        XCTAssertTrue(text.contains("resolved \(second) mullion\n"))
        XCTAssertTrue(text.contains("resolved \(first) neptune\n"))
        XCTAssertEqual(applied, [.automatic])

        receive("resolve \(first) 13", ports)
        for _ in 0..<100 where await resolver.calls.count < 3 { await Task.yield() }
        receive("ready", ports)
        await resolver.complete(13, .venus)
        for _ in 0..<10 { await Task.yield() }
        XCTAssertFalse(String(decoding: output, as: UTF8.self).contains("resolved \(first) venus\n"))
    }

    func testPartiallyWrittenResolutionIsNotInterruptedByControl() async throws {
        let resolver = GraphicsTestResolver()
        var output = [UInt8](), budget = Int.max
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in
                guard budget > 0 else { errno = EAGAIN; return -1 }
                let count = min(budget, bytes.count)
                output += bytes.prefix(count); budget -= count
                return count
            }, graphicsResolver: resolver, availableBackends: [.automatic, .neptune, .venus])
        defer { ports.close() }
        try ports.setGraphicsBackend(.automatic)
        receive("ready", ports)
        budget = 5
        receive("resolve \(first) 1", ports)
        for _ in 0..<100 where await resolver.calls.isEmpty { await Task.yield() }
        await resolver.complete(1, .neptune)
        for _ in 0..<100 where output.count < "backend automatic\n".utf8.count + 5 { await Task.yield() }
        try ports.setGraphicsBackend(.venus)
        budget = Int.max
        ports.flushGraphics()
        XCTAssertEqual(String(decoding: output, as: UTF8.self),
                       "backend automatic\nresolved \(first) neptune\nbackend venus\n")
    }

    func testReconnectDropsCompletedBlockedReplyAndOldControls() async throws {
        let resolver = GraphicsTestResolver()
        var output = [UInt8](), blocked = false, sawReply = false
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in
                if blocked {
                    sawReply = sawReply || String(decoding: bytes, as: UTF8.self).contains("resolved ")
                    errno = EAGAIN; return -1
                }
                output += bytes; return bytes.count
            }, graphicsResolver: resolver, availableBackends: [.automatic, .neptune, .venus])
        defer { ports.close() }
        try ports.setGraphicsBackend(.automatic)
        ports.sendDisplayHDR(supported: true, maxLuminanceNits: 1000,
                             maxFrameAverageLuminanceNits: 500, minLuminance10000ths: 1, sdrReferenceNits: 200)
        receive("ready", ports)
        output.removeAll()
        blocked = true
        receive("resolve \(first) 1", ports)
        for _ in 0..<100 where await resolver.calls.isEmpty { await Task.yield() }
        await resolver.complete(1, .neptune)
        for _ in 0..<100 where !sawReply { await Task.yield() }
        XCTAssertTrue(sawReply)
        try ports.setGraphicsBackend(.venus)
        receive("ready", ports)
        blocked = false
        ports.flushGraphics()
        XCTAssertEqual(String(decoding: output, as: UTF8.self),
                       "backend venus\nhdr 1 1000 500 1 200\n")
    }

    func testReconnectPreservesOnlyPartialFrameTail() throws {
        var output = [UInt8](), budget = Int.max
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in
                guard budget > 0 else { errno = EAGAIN; return -1 }
                let count = min(budget, bytes.count)
                output += bytes.prefix(count); budget -= count
                return count
            })
        defer { ports.close() }
        try ports.setGraphicsBackend(.venus)
        receive("ready", ports)
        output.removeAll()
        budget = 5
        receive("resolve \(first) 1", ports)
        receive("resolve \(second) 2", ports)
        receive("ready", ports)
        budget = Int.max
        ports.flushGraphics()
        XCTAssertEqual(String(decoding: output, as: UTF8.self),
                       "resolved \(first) venus\nbackend venus\n")
    }

    func testBlockedResolutionRepliesAreBoundedAndControlsTakePriority() throws {
        var output = [UInt8](), blocked = false, largestPending = 0
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in
                if blocked { largestPending = max(largestPending, bytes.count); errno = EAGAIN; return -1 }
                output += bytes; return bytes.count
            })
        defer { ports.close() }
        try ports.setGraphicsBackend(.venus)
        receive("ready", ports)
        output.removeAll()
        blocked = true
        for id in 0..<500 {
            receive("resolve \(String(format: "%032x", id)) 1", ports)
        }
        XCTAssertLessThanOrEqual(largestPending, 4096)
        try ports.setGraphicsBackend(.automatic)
        ports.sendDisplayHDR(supported: true, maxLuminanceNits: 1000,
                             maxFrameAverageLuminanceNits: 500, minLuminance10000ths: 1, sdrReferenceNits: 200)
        blocked = false
        ports.flushGraphics()
        let text = String(decoding: output, as: UTF8.self)
        XCTAssertTrue(text.hasPrefix("backend automatic\nhdr 1 1000 500 1 200\nresolved "))
        let replies = text.split(separator: "\n").filter { $0.hasPrefix("resolved ") }
        XCTAssertGreaterThan(replies.count, 0)
        XCTAssertLessThan(replies.count, 500)
        XCTAssertLessThanOrEqual(replies.reduce(0) { $0 + $1.utf8.count + 1 }, 4096)
    }

    func testTerminalWriteErrorClearsQueueAndCancelsResolution() async throws {
        let resolver = GraphicsTestResolver()
        var output = [UInt8](), mode = 0, writes = 0
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in
                writes += 1
                if mode == 1 { errno = EAGAIN; return -1 }
                if mode == 2 { errno = EPIPE; return -1 }
                output += bytes; return bytes.count
            }, graphicsResolver: resolver, availableBackends: [.automatic, .neptune, .venus])
        defer { ports.close() }
        try ports.setGraphicsBackend(.automatic)
        receive("ready", ports)
        output.removeAll()
        receive("resolve \(first) 1", ports)
        for _ in 0..<100 where await resolver.calls.isEmpty { await Task.yield() }
        mode = 1
        try ports.setGraphicsBackend(.venus)
        mode = 2
        ports.flushGraphics()
        let writesAfterError = writes
        ports.flushGraphics()
        XCTAssertEqual(writes, writesAfterError)
        await resolver.complete(1, .neptune)
        for _ in 0..<10 { await Task.yield() }
        XCTAssertEqual(writes, writesAfterError)
        mode = 0
        receive("ready", ports)
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "backend venus\n")
    }

    func testCloseRejectsLateCompletion() async throws {
        let resolver = GraphicsTestResolver()
        var output = [UInt8]()
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in output += bytes; return bytes.count },
            graphicsResolver: resolver, availableBackends: [.automatic, .neptune, .venus])
        try ports.setGraphicsBackend(.automatic)
        receive("ready", ports)
        receive("resolve \(first) 1", ports)
        for _ in 0..<100 where await resolver.calls.isEmpty { await Task.yield() }
        ports.close()
        await resolver.complete(1, .neptune)
        for _ in 0..<10 { await Task.yield() }
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "backend automatic\n")
    }

    func testValidationFallbackAndPartialWrites() async throws {
        let resolver = GraphicsTestResolver()
        var output = [UInt8](), blocked = true
        let ports = try VMPorts(runDirectory: FileManager.default.temporaryDirectory,
            graphicsWriter: { bytes in
                if blocked { errno = EAGAIN; return -1 }
                let count = min(3, bytes.count); output += bytes.prefix(count); return count
            }, graphicsResolver: resolver, availableBackends: [.automatic, .neptune, .venus])
        defer { ports.close() }
        try ports.setGraphicsBackend(.automatic)
        receive("ready", ports)
        for line in ["resolve ABC 1", "resolve \(first) 0", "resolve \(first) 01", "resolve \(first) 4294967296", "resolve  \(first) 1", "resolve \(first) 1 extra"] { receive(line, ports) }
        receive("resolve \(first) 1", ports)
        for _ in 0..<100 where await resolver.calls.isEmpty { await Task.yield() }
        await resolver.complete(1, .mullion)
        for _ in 0..<10 { await Task.yield() }
        try ports.setGraphicsBackend(.venus)
        receive("resolve \(second) 2", ports)
        blocked = false
        ports.flushGraphics()
        let calls = await resolver.calls
        XCTAssertEqual(calls, [1])
        XCTAssertEqual(String(decoding: output, as: UTF8.self), "backend automatic\nbackend venus\nresolved \(first) venus\nresolved \(second) venus\n")
    }
}
