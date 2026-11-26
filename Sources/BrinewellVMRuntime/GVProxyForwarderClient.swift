import BrinewellCore
import Darwin
import Foundation

public struct GVProxyForwardEntry: Codable, Equatable, Sendable {
    public let local: String
    public let remote: String
    public let `protocol`: String

    public init(local: String, remote: String, protocol: String) {
        self.local = local
        self.remote = remote
        self.protocol = `protocol`
    }
}

public enum GVProxyForwarderError: Error, Equatable, Sendable {
    case socketError(String)
    case httpError(Int, String)
    case portConflict(String)
    case invalidResponse
}

public final class GVProxyForwarderClient: Sendable {
    public let socketPath: String
    public let timeoutSeconds: Int
    private static let maxResponse = 64 * 1024
    private let lock = NSRecursiveLock()

    public init(socketPath: String, timeoutSeconds: Int = 3) {
        self.socketPath = socketPath
        self.timeoutSeconds = timeoutSeconds
    }

    public func listForwards() throws -> [GVProxyForwardEntry] {
        lock.lock(); defer { lock.unlock() }
        let (status, body) = try request(method: "GET", path: "/services/forwarder/all", body: nil)
        guard status == 200 else { throw GVProxyForwarderError.httpError(status, String(decoding: body, as: UTF8.self)) }
        guard let entries = try? JSONDecoder().decode([GVProxyForwardEntry].self, from: body) else {
            throw GVProxyForwarderError.invalidResponse
        }
        return entries
    }

    public func expose(local: String, remote: String, proto: String = "tcp") throws {
        lock.lock(); defer { lock.unlock() }
        let entry = GVProxyForwardEntry(local: local, remote: remote, protocol: proto)
        let (status, body) = try request(method: "POST", path: "/services/forwarder/expose", body: JSONEncoder().encode(entry))
        guard status != 200 else { return }
        let message = String(decoding: body, as: UTF8.self)
        let lowered = message.lowercased()
        if lowered.contains("proxy already running") {
            if try listForwards().contains(entry) { return }
            throw GVProxyForwarderError.portConflict(message)
        }
        if lowered.contains("address already in use") || lowered.contains("bind") {
            throw GVProxyForwarderError.portConflict(message)
        }
        throw GVProxyForwarderError.httpError(status, message)
    }

    public func unexpose(local: String, expectedRemote: String, proto: String = "tcp") throws {
        lock.lock(); defer { lock.unlock() }
        let entries = try listForwards().filter { $0.local == local && $0.protocol == proto }
        guard !entries.isEmpty else { return }
        guard entries.allSatisfy({ $0.remote == expectedRemote }) else {
            throw GVProxyForwarderError.portConflict("local port is forwarded to another target")
        }
        let body = try JSONEncoder().encode(UnexposeRequest(local: local, remote: expectedRemote, protocol: proto))
        let (status, response) = try request(method: "POST", path: "/services/forwarder/unexpose", body: body)
        let message = String(decoding: response, as: UTF8.self)
        if status == 409 { throw GVProxyForwarderError.portConflict(message) }
        guard status == 200 || message.lowercased().contains("proxy not found") else {
            throw GVProxyForwarderError.httpError(status, message)
        }
    }

    public func syncSSHForward(desiredExposed: Bool, localPort: Int, guestRemote: String = "192.168.127.2:22") -> VMSSHForwardState {
        lock.lock(); defer { lock.unlock() }
        let local = "127.0.0.1:\(localPort)"
        do {
            if desiredExposed { try expose(local: local, remote: guestRemote) }
            else { try unexpose(local: local, expectedRemote: guestRemote) }
            let matches = try listForwards().filter { $0.local == local && $0.protocol == "tcp" }
            if matches.contains(where: { $0.remote != guestRemote }) {
                return .conflict("local port is forwarded to another target")
            }
            if desiredExposed {
                return matches.isEmpty ? .error("forward was not published") : .published
            }
            return matches.isEmpty ? .unpublished : .error("forward remains published")
        } catch GVProxyForwarderError.portConflict(let message) {
            return .conflict(message)
        } catch {
            return .error(String(describing: error))
        }
    }

    private struct UnexposeRequest: Encodable {
        let local: String
        let remote: String
        let `protocol`: String
    }

    private func request(method: String, path: String, body: Data?) throws -> (Int, Data) {
        let deadline = DispatchTime.now() + .seconds(max(1, timeoutSeconds))
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw socketError() }
        defer { Darwin.close(fd) }
        guard fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
              fcntl(fd, F_SETNOSIGPIPE, 1) == 0 else { throw socketError() }
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = Array(socketPath.utf8) + [0]
        guard pathBytes.count <= MemoryLayout.size(ofValue: address.sun_path), !socketPath.utf8.contains(0) else {
            throw GVProxyForwarderError.socketError("invalid socket path")
        }
        withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: pathBytes) }
        address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        try setTimeout(fd, deadline: deadline)
        let connected = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connected == 0 else { throw requestSocketError() }
        let payload = body ?? Data()
        var request = Data("\(method) \(path) HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: \(payload.count)\r\n\r\n".utf8)
        request.append(payload)
        var offset = 0
        while offset < request.count {
            try setTimeout(fd, deadline: deadline)
            let count = request.withUnsafeBytes { Darwin.write(fd, $0.baseAddress!.advanced(by: offset), $0.count - offset) }
            if count > 0 { offset += count }
            else if count < 0 && errno == EINTR { continue }
            else { throw requestSocketError() }
        }
        var response = Data()
        var buffer = [UInt8](repeating: 0, count: 4096)
        while true {
            try setTimeout(fd, deadline: deadline)
            let count = Darwin.read(fd, &buffer, buffer.count)
            if count > 0 {
                response.append(contentsOf: buffer[..<count])
                guard response.count <= Self.maxResponse else { throw GVProxyForwarderError.invalidResponse }
                if let parsed = try parseResponse(response, eof: false) { return parsed }
            } else if count == 0 {
                guard let parsed = try parseResponse(response, eof: true) else { throw GVProxyForwarderError.invalidResponse }
                return parsed
            } else if errno != EINTR { throw requestSocketError() }
        }
    }

    private func setTimeout(_ fd: Int32, deadline: DispatchTime) throws {
        let now = DispatchTime.now().uptimeNanoseconds
        guard now < deadline.uptimeNanoseconds else { throw GVProxyForwarderError.socketError("request timed out") }
        let micros = max(1, (deadline.uptimeNanoseconds - now + 999) / 1_000)
        var timeout = timeval(tv_sec: Int(micros / 1_000_000), tv_usec: Int32(micros % 1_000_000))
        guard setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) == 0,
              setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) == 0 else {
            throw socketError()
        }
    }

    private func requestSocketError() -> GVProxyForwarderError {
        if errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT { return .socketError("request timed out") }
        return socketError()
    }

    private func socketError() -> GVProxyForwarderError {
        .socketError(String(cString: strerror(errno)))
    }

    private func parseResponse(_ data: Data, eof: Bool) throws -> (Int, Data)? {
        guard let separator = data.range(of: Data("\r\n\r\n".utf8)) else {
            if eof { throw GVProxyForwarderError.invalidResponse }
            return nil
        }
        guard let header = String(data: data[..<separator.lowerBound], encoding: .utf8) else { throw GVProxyForwarderError.invalidResponse }
        let lines = header.components(separatedBy: "\r\n")
        let statusParts = lines[0].split(separator: " ", maxSplits: 2)
        guard statusParts.count >= 2, statusParts[0].hasPrefix("HTTP/1."), let status = Int(statusParts[1]) else {
            throw GVProxyForwarderError.invalidResponse
        }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let colon = line.firstIndex(of: ":") else { throw GVProxyForwarderError.invalidResponse }
            headers[String(line[..<colon]).lowercased()] = String(line[line.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
        }
        let body = Data(data[separator.upperBound...])
        if headers["transfer-encoding"]?.lowercased().split(separator: ",").map({ $0.trimmingCharacters(in: .whitespaces) }).contains("chunked") == true {
            if let decoded = try decodeChunks(body) { return (status, decoded) }
            if eof { throw GVProxyForwarderError.invalidResponse }
            return nil
        }
        if let lengthText = headers["content-length"] {
            guard let length = Int(lengthText), length >= 0, length <= Self.maxResponse else { throw GVProxyForwarderError.invalidResponse }
            if body.count >= length { return (status, Data(body.prefix(length))) }
            if eof { throw GVProxyForwarderError.invalidResponse }
            return nil
        }
        return eof ? (status, body) : nil
    }

    private func decodeChunks(_ data: Data) throws -> Data? {
        let bytes = [UInt8](data)
        var offset = 0
        var result = Data()
        while true {
            guard offset < bytes.count, let end = bytes[offset...].firstIndex(of: 13) else { return nil }
            guard end + 1 < bytes.count else { return nil }
            guard bytes[end + 1] == 10, let line = String(bytes: bytes[offset..<end], encoding: .utf8),
                  let sizeText = line.split(separator: ";", maxSplits: 1).first,
                  let size = Int(sizeText, radix: 16), size >= 0, size <= Self.maxResponse - result.count else {
                throw GVProxyForwarderError.invalidResponse
            }
            offset = end + 2
            if size == 0 {
                // A zero chunk ends with an empty trailer line, possibly after trailers.
                guard offset + 1 < bytes.count else { return nil }
                if bytes[offset] == 13 && bytes[offset + 1] == 10 { return result }
                guard data.range(of: Data("\r\n\r\n".utf8), in: offset..<data.count) != nil else { return nil }
                return result
            }
            guard size <= bytes.count - offset, bytes.count - offset - size >= 2 else { return nil }
            result.append(contentsOf: bytes[offset..<(offset + size)])
            offset += size
            guard bytes[offset] == 13 && bytes[offset + 1] == 10 else { throw GVProxyForwarderError.invalidResponse }
            offset += 2
        }
    }
}
