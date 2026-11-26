import BrinewellCore
import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

private final class ForwarderServer: @unchecked Sendable {
    let path = "/tmp/bwf-\(UUID().uuidString).sock"
    private let listener: Int32
    private let responses: [String]
    private let byteDelay: TimeInterval
    private let finished = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var captured: [Data] = []
    var requests: [Data] { lock.lock(); defer { lock.unlock() }; return captured }

    init(_ responses: [String], byteDelay: TimeInterval = 0) throws {
        self.responses = responses
        self.byteDelay = byteDelay
        listener = socket(AF_UNIX, SOCK_STREAM, 0)
        guard listener >= 0 else { throw POSIXError(.EIO) }
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let bytes = Array(path.utf8) + [0]
        withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: bytes) }
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(listener, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bound == 0, listen(listener, 8) == 0 else { throw POSIXError(.EIO) }
        Thread.detachNewThread { [self] in serve() }
    }

    private func serve() {
        defer { finished.signal() }
        for response in responses {
            let client = accept(listener, nil, nil)
            guard client >= 0 else { return }
            var request = Data()
            var buffer = [UInt8](repeating: 0, count: 2048)
            while true {
                let n = Darwin.read(client, &buffer, buffer.count)
                guard n > 0 else { break }
                request.append(contentsOf: buffer[..<n])
                if let separator = request.range(of: Data("\r\n\r\n".utf8)),
                   let headers = String(data: request[..<separator.lowerBound], encoding: .utf8) {
                    let length = headers.components(separatedBy: "\r\n").first { $0.lowercased().hasPrefix("content-length:") }
                        .flatMap { Int($0.split(separator: ":")[1].trimmingCharacters(in: .whitespaces)) } ?? 0
                    if request.count - separator.upperBound >= length { break }
                }
            }
            lock.lock(); captured.append(request); lock.unlock()
            let data = Array(response.utf8)
            if byteDelay > 0 {
                _ = fcntl(client, F_SETNOSIGPIPE, 1)
                for var byte in data {
                    if Darwin.write(client, &byte, 1) != 1 { break }
                    Thread.sleep(forTimeInterval: byteDelay)
                }
            } else {
                _ = data.withUnsafeBytes { Darwin.write(client, $0.baseAddress!, $0.count) }
            }
            Darwin.close(client)
        }
    }

    func stop() {
        _ = shutdown(listener, SHUT_RDWR)
        Darwin.close(listener)
        _ = finished.wait(timeout: .now() + 5)
        unlink(path)
    }
}

final class GVProxyForwarderClientTests: XCTestCase {
    private func response(_ body: String, status: Int = 200) -> String {
        "HTTP/1.1 \(status) Test\r\nContent-Length: \(body.utf8.count)\r\nConnection: close\r\n\r\n\(body)"
    }

    private let entry = "{\"local\":\"127.0.0.1:2222\",\"remote\":\"192.168.127.2:22\",\"protocol\":\"tcp\"}"

    func testListAndChunked() throws {
        let body = "[\(entry)]"
        let chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\(String(body.utf8.count, radix: 16))\r\n\(body)\r\n0\r\n\r\n"
        let server = try ForwarderServer([response("[]"), chunked])
        defer { server.stop() }
        let client = GVProxyForwarderClient(socketPath: server.path)
        XCTAssertEqual(try client.listForwards(), [])
        XCTAssertEqual(try client.listForwards(), [.init(local: "127.0.0.1:2222", remote: "192.168.127.2:22", protocol: "tcp")])
    }

    func testSlowResponseHitsAbsoluteDeadline() throws {
        let server = try ForwarderServer([response("[]")], byteDelay: 0.1)
        defer { server.stop() }
        let client = GVProxyForwarderClient(socketPath: server.path, timeoutSeconds: 1)
        let start = Date()
        XCTAssertThrowsError(try client.listForwards()) {
            XCTAssertEqual($0 as? GVProxyForwarderError, .socketError("request timed out"))
        }
        XCTAssertLessThan(Date().timeIntervalSince(start), 3)
    }

    func testExposeConflictAndAlreadyRunning() throws {
        let server = try ForwarderServer([response(""), response("address already in use", status: 500),
                                          response("proxy already running", status: 500), response("[\(entry)]")])
        defer { server.stop() }
        let client = GVProxyForwarderClient(socketPath: server.path)
        try client.expose(local: "127.0.0.1:2222", remote: "192.168.127.2:22")
        XCTAssertThrowsError(try client.expose(local: "127.0.0.1:2222", remote: "192.168.127.2:22")) {
            XCTAssertEqual($0 as? GVProxyForwarderError, .portConflict("address already in use"))
        }
        try client.expose(local: "127.0.0.1:2222", remote: "192.168.127.2:22")
    }

    func testUnexposeAndSync() throws {
        let other = entry.replacingOccurrences(of: "192.168.127.2:22", with: "192.168.127.2:8080")
        let server = try ForwarderServer([response("[\(other)]"), response("[]"), response("[]"),
                                          response("[\(entry)]"), response(""), response("[]"),
                                          response(""), response("[\(entry)]")])
        defer { server.stop() }
        let client = GVProxyForwarderClient(socketPath: server.path)
        XCTAssertEqual(client.syncSSHForward(desiredExposed: false, localPort: 2222), .conflict("local port is forwarded to another target"))
        XCTAssertEqual(client.syncSSHForward(desiredExposed: false, localPort: 2222), .unpublished)
        XCTAssertEqual(client.syncSSHForward(desiredExposed: false, localPort: 2222), .unpublished)
        XCTAssertEqual(client.syncSSHForward(desiredExposed: true, localPort: 2222), .published)
    }
    func testUnexposeSendsRemoteAndHandlesConflict() throws {
        let server = try ForwarderServer([response("[\(entry)]"), response("changed", status: 409)])
        defer { server.stop() }
        let client = GVProxyForwarderClient(socketPath: server.path)
        XCTAssertEqual(client.syncSSHForward(desiredExposed: false, localPort: 2222), .conflict("changed"))
        let request = try XCTUnwrap(server.requests.last)
        let separator = try XCTUnwrap(request.range(of: Data("\r\n\r\n".utf8)))
        let body = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(request[separator.upperBound...])) as? [String: String])
        XCTAssertEqual(body, ["local": "127.0.0.1:2222", "remote": "192.168.127.2:22", "protocol": "tcp"])
    }

}
