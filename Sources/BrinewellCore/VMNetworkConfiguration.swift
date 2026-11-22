import Darwin
import Foundation

public struct VMNetworkConfiguration: Codable, Equatable, Sendable {
    public var proxyEnabled: Bool
    public var proxyURL: String

    public init(proxyEnabled: Bool = false, proxyURL: String = "") {
        self.proxyEnabled = proxyEnabled
        self.proxyURL = proxyURL
    }

    public func validate() throws {
        guard proxyEnabled else { return }
        guard proxyURL.rangeOfCharacter(from: .whitespacesAndNewlines) == nil,
              let components = URLComponents(string: proxyURL),
              let scheme = components.scheme?.lowercased(), ["socks5", "http", "https"].contains(scheme),
              let host = components.host, !host.isEmpty,
              host.rangeOfCharacter(from: .whitespacesAndNewlines) == nil,
              let port = components.port, (1...65535).contains(port),
              components.path.isEmpty, components.query == nil, components.fragment == nil,
              components.url != nil else {
            throw NetworkConfigurationError.invalidProxy
        }
    }

    /// Runtime credentials travel in an owner-only file, never in process arguments.
    public func write(to url: URL) throws {
        try validate()
        try writePrivateConfiguration(JSONEncoder().encode(self), to: url)
    }
}

// Shared by persistent settings and the runtime snapshot so credentials are
// protected from the moment the file is created, including during replacement.
func writePrivateConfiguration(_ data: Data, to url: URL) throws {
    let fd = open(url.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
    guard fd >= 0 else { throw CocoaError(.fileWriteUnknown) }
    let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    do {
        try handle.write(contentsOf: data)
        try handle.close()
    } catch {
        try? handle.close()
        try? FileManager.default.removeItem(at: url)
        throw CocoaError(.fileWriteUnknown)
    }
}

private enum NetworkConfigurationError: LocalizedError {
    case invalidProxy

    var errorDescription: String? {
        switch self {
        case .invalidProxy:
            return String(localized: "Enter a socks5://, http://, or https:// address with a host and port. Leave out paths, queries, and fragments.")
        }
    }
}
