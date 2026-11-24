import Darwin
import Foundation

public struct GPTKVersion: Sendable, Equatable, Hashable, Comparable, Codable {
    public let major: Int
    public let minor: Int
    public let patch: Int
    public let beta: Int?

    public init(major: Int, minor: Int, patch: Int, beta: Int? = nil) {
        self.major = major
        self.minor = minor
        self.patch = patch
        self.beta = beta
    }

    public static let recommended = GPTKVersion(major: 4, minor: 0, patch: 0, beta: 3)

    public var displayString: String {
        "\(major).\(minor)" + (patch == 0 ? "" : ".\(patch)") + (beta.map { "b\($0)" } ?? "")
    }

    public static func < (lhs: Self, rhs: Self) -> Bool {
        if lhs.major != rhs.major { return lhs.major < rhs.major }
        if lhs.minor != rhs.minor { return lhs.minor < rhs.minor }
        if lhs.patch != rhs.patch { return lhs.patch < rhs.patch }
        switch (lhs.beta, rhs.beta) {
        case let (a?, b?): return a < b
        case (_?, nil): return true
        default: return false
        }
    }

    public static func parse(from text: String) -> Self? {
        let pattern = #"(?i)(?<![a-z0-9.])(\d{1,2})\.(\d{1,2})(?:\.(\d{1,2}))?(?:[ _-]*(?:beta|b)[ _-]*(\d{1,2}))?(?![a-z0-9]|\.\d)"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else { return nil }
        func number(_ index: Int) -> Int? {
            guard let range = Range(match.range(at: index), in: text) else { return nil }
            return Int(text[range])
        }
        guard let major = number(1), let minor = number(2) else { return nil }
        return Self(major: major, minor: minor, patch: number(3) ?? 0, beta: number(4))
    }
}

public struct AppleDownloadArtifact: Sendable, Equatable, Hashable, Codable {
    public let id: String
    public let releaseName: String
    public let filename: String
    public let relativePath: String
    public let fileSize: Int64?
    public let datePublished: String?
    public let version: GPTKVersion

    public init(id: String, releaseName: String, filename: String, relativePath: String,
                fileSize: Int64?, datePublished: String?, version: GPTKVersion) {
        self.id = id
        self.releaseName = releaseName
        self.filename = filename
        self.relativePath = relativePath
        self.fileSize = fileSize
        self.datePublished = datePublished
        self.version = version
    }

    public var portalDownloadURL: URL {
        var components = URLComponents(url: AppleDeveloperCatalog.downloadRedirectBaseURL, resolvingAgainstBaseURL: false)!
        components.percentEncodedQuery = "path=" + relativePath.addingPercentEncoding(withAllowedCharacters: .alphanumerics)!
        return components.url!
    }

    public var directCDNURL: URL {
        var components = URLComponents()
        components.scheme = "https"
        components.host = AppleDeveloperCatalog.allowedDownloadHost
        components.path = relativePath
        return components.url!
    }
}

public enum AppleDeveloperCatalogError: LocalizedError, Sendable, Equatable {
    case invalidJSON, unsupportedSchema, noMatchingArtifacts, authenticationRequired
    case invalidArtifactPath(String)

    public var errorDescription: String? {
        switch self {
        case .invalidJSON: return "The Apple download catalog is not valid JSON."
        case .unsupportedSchema: return "The Apple download catalog has an unsupported format."
        case .noMatchingArtifacts: return "No Game Porting Toolkit disk images were found."
        case .authenticationRequired: return "Apple Developer authentication is required."
        case .invalidArtifactPath(let path): return "Invalid Apple download path: \(path)"
        }
    }
}

public enum AppleDeveloperCatalog {
    public static let downloadsPageURL = URL(string: "https://developer.apple.com/download/all/?q=Game%20Porting%20Toolkit")!
    public static let listDownloadsURL = URL(string: "https://developer.apple.com/services-account/QH65B2/downloadws/listDownloads.action")!
    public static let downloadRedirectBaseURL = URL(string: "https://developer.apple.com/services-account/download")!
    public static let allowedDownloadHost = "download.developer.apple.com"
    public static let allowedPortalHost = "developer.apple.com"

    public static func validateRelativeDownloadPath(_ rawPath: String) throws -> String {
        func invalid() -> AppleDeveloperCatalogError { .invalidArtifactPath(rawPath) }
        guard !rawPath.isEmpty, !rawPath.hasPrefix("//"), !rawPath.contains("://"),
              !rawPath.contains("\\"), !rawPath.contains("?"), !rawPath.contains("#"),
              rawPath.rangeOfCharacter(from: .whitespacesAndNewlines.union(.controlCharacters)) == nil else { throw invalid() }
        var decoded = rawPath
        // Repeated decoding prevents nested escapes from becoming traversal at a downstream server.
        for _ in 0..<4 {
            guard let next = decoded.removingPercentEncoding else { throw invalid() }
            if next == decoded { break }
            decoded = next
        }
        guard !decoded.contains("%"), !decoded.contains("\\"), !decoded.contains("?"), !decoded.contains("#"),
              decoded.rangeOfCharacter(from: .whitespacesAndNewlines.union(.controlCharacters)) == nil,
              !decoded.split(separator: "/", omittingEmptySubsequences: false).contains(".."),
              !decoded.hasPrefix("//"), !decoded.contains("://"), decoded.lowercased().hasSuffix(".dmg") else { throw invalid() }
        return "/" + decoded.drop(while: { $0 == "/" })
    }

    public static func validateDownloadURL(_ url: URL) throws {
        guard url.scheme?.lowercased() == "https", url.host?.lowercased() == allowedDownloadHost,
              url.port == nil || url.port == 443, url.user == nil, url.password == nil,
              url.query == nil, url.fragment == nil,
              let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            throw AppleDMGDownloadError.invalidURL(url.absoluteString)
        }
        do { _ = try validateRelativeDownloadPath(components.percentEncodedPath) }
        catch { throw AppleDMGDownloadError.invalidURL(url.absoluteString) }
    }

    public static func parseGamePortingToolkitArtifacts(from data: Data) throws -> [AppleDownloadArtifact] {
        let prefix = String(data: data.prefix(256), encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() ?? ""
        if prefix.hasPrefix("<") { throw AppleDeveloperCatalogError.authenticationRequired }
        let object: Any
        do { object = try JSONSerialization.jsonObject(with: data) }
        catch { throw AppleDeveloperCatalogError.invalidJSON }
        guard let root = object as? [String: Any] else { throw AppleDeveloperCatalogError.unsupportedSchema }
        if let code = root["resultCode"], String(describing: code) != "0" { throw AppleDeveloperCatalogError.authenticationRequired }
        guard let downloads = root["downloads"] as? [[String: Any]] else { throw AppleDeveloperCatalogError.unsupportedSchema }
        var artifacts: [AppleDownloadArtifact] = []
        var seen = Set<String>()
        for entry in downloads {
            let name = (entry["name"] as? String) ?? (entry["title"] as? String) ?? (entry["description"] as? String) ?? ""
            guard let files = entry["files"] as? [[String: Any]] else { continue }
            for file in files {
                guard let filename = (file["filename"] as? String) ?? (file["displayName"] as? String),
                      filename.lowercased().hasSuffix(".dmg"),
                      let path = (file["remotePath"] as? String) ?? (file["downloadPath"] as? String) ?? (file["path"] as? String) else { continue }
                let normalizedName = (name + " " + filename).lowercased().replacingOccurrences(of: "_", with: " ")
                guard normalizedName.contains("game porting toolkit") || normalizedName.contains("evaluation environment for windows games") else { continue }
                let lowerFilename = filename.lowercased()
                guard !["source", "release notes", "release_notes", "sample", "windows helper", "windows_helper", "windows tool", "windows_tool", "shader converter", "shader_converter"].contains(where: { lowerFilename.contains($0) }) else { continue }
                if lowerFilename.contains("windows"), !lowerFilename.contains("evaluation"), !lowerFilename.contains("game_porting_toolkit"), !lowerFilename.contains("game porting toolkit") { continue }
                guard let version = GPTKVersion.parse(from: filename) ?? GPTKVersion.parse(from: name) else { continue }
                let validPath: String
                do { validPath = try validateRelativeDownloadPath(path) }
                catch { continue }
                guard seen.insert(validPath).inserted else { continue }
                let size = (file["fileSize"] as? NSNumber)?.int64Value ?? (file["fileSize"] as? String).flatMap(Int64.init)
                let date = (file["dateModified"] as? String) ?? (file["dateCreated"] as? String) ?? (file["datePublished"] as? String) ?? (entry["datePublished"] as? String)
                artifacts.append(.init(id: validPath, releaseName: name, filename: filename, relativePath: validPath, fileSize: size, datePublished: date, version: version))
            }
        }
        guard !artifacts.isEmpty else { throw AppleDeveloperCatalogError.noMatchingArtifacts }
        return artifacts
    }

    public static func selectBestArtifact(from artifacts: [AppleDownloadArtifact], preferredVersion: GPTKVersion = .recommended) -> AppleDownloadArtifact? {
        func priority(_ artifact: AppleDownloadArtifact) -> Int {
            if artifact.version == preferredVersion { return 0 }
            switch artifact.version.major { case 4: return 1; case 3: return 2; case 2: return 3; default: return 4 }
        }
        func primary(_ artifact: AppleDownloadArtifact) -> Bool {
            let name = artifact.filename.lowercased()
            return name.hasPrefix("game_porting_toolkit") || name.hasPrefix("evaluation")
        }
        return artifacts.sorted {
            let left = priority($0), right = priority($1)
            if left != right { return left < right }
            if $0.version != $1.version { return $0.version > $1.version }
            if primary($0) != primary($1) { return primary($0) }
            return $0.relativePath < $1.relativePath
        }.first
    }

    public static func extractDownloadAuthCookieHeader(from cookies: [HTTPCookie], for url: URL, now: Date = Date()) -> String? {
        guard url.scheme?.lowercased() == "https", url.host?.lowercased() == allowedDownloadHost else { return nil }
        let host = allowedDownloadHost
        let matching = cookies.filter { cookie in
            let domain = cookie.domain.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "."))
            let path = cookie.path.isEmpty ? "/" : cookie.path
            let urlPath = url.path.isEmpty ? "/" : url.path
            let pathMatches = urlPath == path || (urlPath.hasPrefix(path) && (path.hasSuffix("/") || urlPath.dropFirst(path.count).hasPrefix("/")))
            return cookie.name == "ADCDownloadAuth" && !cookie.value.isEmpty && !cookie.value.contains("\r") && !cookie.value.contains("\n") &&
                (host == domain || host.hasSuffix("." + domain)) && pathMatches && (cookie.expiresDate.map { $0 > now } ?? true)
        }.sorted { $0.path.count > $1.path.count }
        return matching.first.map { "ADCDownloadAuth=\($0.value)" }
    }
}

public struct AppleDownloadAuthorization: Sendable, Equatable, CustomStringConvertible, CustomDebugStringConvertible {
    public let downloadURL: URL
    public let cookieHeader: String

    public init(downloadURL: URL, cookieHeader: String) throws {
        try AppleDeveloperCatalog.validateDownloadURL(downloadURL)
        guard cookieHeader.hasPrefix("ADCDownloadAuth="), cookieHeader.count > "ADCDownloadAuth=".count,
              !cookieHeader.unicodeScalars.contains(where: { $0.value == 10 || $0.value == 13 }), !cookieHeader.contains(";") else {
            throw AppleDeveloperCatalogError.authenticationRequired
        }
        self.downloadURL = downloadURL
        self.cookieHeader = cookieHeader
    }

    public var description: String { "AppleDownloadAuthorization(downloadURL: \(downloadURL), cookieHeader: <redacted>)" }
    public var debugDescription: String { description }
}

public struct AppleDownloadProgress: Sendable, Equatable {
    public let bytesReceived: Int64
    public let totalBytes: Int64?
    public init(bytesReceived: Int64, totalBytes: Int64?) { self.bytesReceived = bytesReceived; self.totalBytes = totalBytes }
    public var fractionCompleted: Double? {
        guard let totalBytes, totalBytes > 0 else { return nil }
        return Double(bytesReceived) / Double(totalBytes)
    }
}

public enum AppleDMGDownloadError: LocalizedError, Sendable, Equatable {
    case invalidURL(String), untrustedRedirect(String), authorizationExpired, unexpectedResponse(Int), unexpectedContentType(String), invalidContentRange, fileTooLarge(Int64), insufficientDiskSpace(required: Int64, available: Int64), cancelled, ioFailure(String)
    public var errorDescription: String? {
        switch self {
        case .invalidURL(let value): return "Invalid download URL: \(value)"
        case .untrustedRedirect(let value): return "Untrusted download redirect: \(value)"
        case .authorizationExpired: return "Apple download authorization has expired."
        case .unexpectedResponse(let code): return "Unexpected download response: \(code)"
        case .unexpectedContentType(let value): return "Unexpected content type: \(value)"
        case .invalidContentRange: return "Invalid or incomplete download range."
        case .fileTooLarge(let size): return "Download exceeds the size limit: \(size) bytes."
        case .insufficientDiskSpace(let required, let available): return "Insufficient disk space (\(required) required, \(available) available)."
        case .cancelled: return "Download cancelled."
        case .ioFailure(let message): return "Download I/O failed: \(message)"
        }
    }
}

private struct AppleDownloadMetadata: Codable {
    let relativePath: String
    let expectedLength: Int64?
    let etag: String?
    let lastModified: String?
    var validator: String? {
        if let etag, !etag.hasPrefix("W/") { return etag }
        return lastModified
    }
}

private final class AppleRedirectDelegate: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var rejected: String?
    let cookie: String
    init(cookie: String) { self.cookie = cookie }
    var rejectedURL: String? { lock.withLock { rejected } }
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        guard let url = request.url, (try? AppleDeveloperCatalog.validateDownloadURL(url)) != nil else {
            lock.withLock { rejected = request.url?.absoluteString ?? "" }
            completionHandler(nil)
            return
        }
        var allowed = request
        allowed.setValue(cookie, forHTTPHeaderField: "Cookie")
        completionHandler(allowed)
    }
}

public final class AppleDMGDownloader: Sendable {
    public static let maximumDMGBytes: Int64 = 4 * 1024 * 1024 * 1024
    private let configuration: URLSessionConfiguration

    public init(sessionConfiguration: URLSessionConfiguration? = nil) {
        let config = (sessionConfiguration?.copy() as? URLSessionConfiguration) ?? .ephemeral
        config.httpCookieStorage = nil
        config.urlCredentialStorage = nil
        config.urlCache = nil
        config.httpShouldSetCookies = false
        self.configuration = config
    }

    public func download(artifact: AppleDownloadArtifact, authorization: AppleDownloadAuthorization, destinationDirectory: URL,
                         progress: (@Sendable (AppleDownloadProgress) -> Void)? = nil) async throws -> URL {
        do {
            try AppleDeveloperCatalog.validateDownloadURL(authorization.downloadURL)
            let path = try AppleDeveloperCatalog.validateRelativeDownloadPath(artifact.relativePath)
            let filename = URL(fileURLWithPath: artifact.filename).lastPathComponent
            guard filename == artifact.filename, filename != ".", filename != "..", filename.lowercased().hasSuffix(".dmg"),
                  !filename.contains("\\"), filename.rangeOfCharacter(from: .controlCharacters) == nil else {
                throw AppleDMGDownloadError.ioFailure("Invalid filename")
            }
            let fm = FileManager.default
            try fm.createDirectory(at: destinationDirectory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            let final = destinationDirectory.appendingPathComponent(filename)
            let part = destinationDirectory.appendingPathComponent(filename + ".part")
            let sidecar = destinationDirectory.appendingPathComponent(filename + ".meta.json")
            var previous = (try? Data(contentsOf: sidecar)).flatMap { try? JSONDecoder().decode(AppleDownloadMetadata.self, from: $0) }
            var offset = ((try? fm.attributesOfItem(atPath: part.path)[.size]) as? NSNumber)?.int64Value ?? 0
            if previous?.relativePath != path || previous?.validator == nil || offset <= 0 || offset > Self.maximumDMGBytes || (previous?.expectedLength.map { offset > $0 } ?? false) {
                previous = nil
                offset = 0
            }
            let delegate = AppleRedirectDelegate(cookie: authorization.cookieHeader)
            let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
            defer { session.invalidateAndCancel() }
            for attempt in 0...1 {
                try Task.checkCancellation()
                var request = URLRequest(url: authorization.downloadURL)
                request.setValue(authorization.cookieHeader, forHTTPHeaderField: "Cookie")
                request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
                if offset > 0 {
                    request.setValue("bytes=\(offset)-", forHTTPHeaderField: "Range")
                    request.setValue(previous?.validator, forHTTPHeaderField: "If-Range")
                }
                let bytes: URLSession.AsyncBytes
                let response: URLResponse
                do {
                    (bytes, response) = try await session.bytes(for: request)
                } catch {
                    if let rejected = delegate.rejectedURL { throw AppleDMGDownloadError.untrustedRedirect(rejected) }
                    throw error
                }
                if let rejected = delegate.rejectedURL { throw AppleDMGDownloadError.untrustedRedirect(rejected) }
                guard let http = response as? HTTPURLResponse else { throw AppleDMGDownloadError.unexpectedResponse(0) }
                if http.statusCode == 401 || http.statusCode == 403 { throw AppleDMGDownloadError.authorizationExpired }
                if http.statusCode == 416 && offset > 0 && attempt == 0 {
                    try? fm.removeItem(at: part)
                    try? fm.removeItem(at: sidecar)
                    offset = 0
                    previous = nil
                    continue
                }
                guard http.statusCode == 200 || (http.statusCode == 206 && offset > 0) else { throw AppleDMGDownloadError.unexpectedResponse(http.statusCode) }
                if http.value(forHTTPHeaderField: "Content-Type")?.lowercased().hasPrefix("text/html") == true { throw AppleDMGDownloadError.authorizationExpired }
                let expected: Int64?
                if http.statusCode == 206 {
                    let range = http.value(forHTTPHeaderField: "Content-Range") ?? ""
                    let pattern = #"^bytes (\d+)-(\d+)/(\d+)$"#
                    guard let regex = try? NSRegularExpression(pattern: pattern), let match = regex.firstMatch(in: range, range: NSRange(range.startIndex..., in: range)),
                          let startRange = Range(match.range(at: 1), in: range), let endRange = Range(match.range(at: 2), in: range), let totalRange = Range(match.range(at: 3), in: range),
                          let start = Int64(range[startRange]), let end = Int64(range[endRange]), let total = Int64(range[totalRange]),
                          start == offset, end == total - 1, total > offset,
                          previous?.expectedLength == nil || previous?.expectedLength == total,
                          http.expectedContentLength < 0 || http.expectedContentLength == total - offset else { throw AppleDMGDownloadError.invalidContentRange }
                    if let etag = previous?.etag, !etag.hasPrefix("W/"), let returned = http.value(forHTTPHeaderField: "ETag"), returned != etag { throw AppleDMGDownloadError.invalidContentRange }
                    if previous?.etag == nil, let modified = previous?.lastModified, let returned = http.value(forHTTPHeaderField: "Last-Modified"), returned != modified { throw AppleDMGDownloadError.invalidContentRange }
                    expected = total
                } else {
                    offset = 0
                    expected = http.expectedContentLength >= 0 ? http.expectedContentLength : nil
                }
                if let expected, expected > Self.maximumDMGBytes { throw AppleDMGDownloadError.fileTooLarge(expected) }
                if let expected, let available = try? destinationDirectory.resourceValues(forKeys: [.volumeAvailableCapacityKey]).volumeAvailableCapacity {
                    let needed = expected - offset
                    if needed > Int64(available) { throw AppleDMGDownloadError.insufficientDiskSpace(required: needed, available: Int64(available)) }
                }
                let metadata = AppleDownloadMetadata(relativePath: path, expectedLength: expected,
                    etag: http.value(forHTTPHeaderField: "ETag") ?? (offset > 0 ? previous?.etag : nil),
                    lastModified: http.value(forHTTPHeaderField: "Last-Modified") ?? (offset > 0 ? previous?.lastModified : nil))
                let fd = open(part.path, O_RDWR | O_CREAT | O_NOFOLLOW, 0o600)
                guard fd >= 0 else { throw AppleDMGDownloadError.ioFailure(String(cString: strerror(errno))) }
                var info = stat()
                guard fstat(fd, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG, info.st_nlink == 1,
                      fchmod(fd, 0o600) == 0 else {
                    close(fd)
                    throw AppleDMGDownloadError.ioFailure("Invalid partial file")
                }
                guard (offset != 0 || ftruncate(fd, 0) == 0), lseek(fd, off_t(offset), SEEK_SET) == off_t(offset) else {
                    close(fd)
                    throw AppleDMGDownloadError.ioFailure("Cannot position partial file")
                }
                let file = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
                defer { try? file.close() }
                try JSONEncoder().encode(metadata).write(to: sidecar, options: .atomic)
                try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: sidecar.path)
                var received = offset
                var buffer = Data()
                var first = Data()
                var lastUpdate = Date.distantPast
                for try await byte in bytes {
                    try Task.checkCancellation()
                    if first.count < 16 { first.append(byte) }
                    buffer.append(byte)
                    if received + Int64(buffer.count) > Self.maximumDMGBytes { throw AppleDMGDownloadError.fileTooLarge(received + Int64(buffer.count)) }
                    if let expected, received + Int64(buffer.count) > expected { throw AppleDMGDownloadError.invalidContentRange }
                    if buffer.count >= 64 * 1024 {
                        if offset == 0 { try Self.rejectHTML(first) }
                        try file.write(contentsOf: buffer)
                        received += Int64(buffer.count)
                        buffer.removeAll(keepingCapacity: true)
                        if Date().timeIntervalSince(lastUpdate) >= 0.2 { progress?(.init(bytesReceived: received, totalBytes: expected)); lastUpdate = Date() }
                    }
                }
                if offset == 0 { try Self.rejectHTML(first) }
                try file.write(contentsOf: buffer)
                received += Int64(buffer.count)
                guard expected == nil || received == expected else { throw AppleDMGDownloadError.invalidContentRange }
                try file.synchronize()
                try file.close()
                var finalIsDirectory: ObjCBool = false
                if fm.fileExists(atPath: final.path, isDirectory: &finalIsDirectory), finalIsDirectory.boolValue {
                    throw AppleDMGDownloadError.ioFailure("Destination is a directory")
                }
                guard rename(part.path, final.path) == 0 else {
                    throw AppleDMGDownloadError.ioFailure(String(cString: strerror(errno)))
                }
                try fm.removeItem(at: sidecar)
                progress?(.init(bytesReceived: received, totalBytes: expected))
                return final
            }
            throw AppleDMGDownloadError.invalidContentRange
        } catch is CancellationError {
            throw AppleDMGDownloadError.cancelled
        } catch let error as AppleDMGDownloadError {
            throw error
        } catch {
            if Task.isCancelled { throw AppleDMGDownloadError.cancelled }
            throw AppleDMGDownloadError.ioFailure(error.localizedDescription)
        }
    }

    private static func rejectHTML(_ prefix: Data) throws {
        let text = String(decoding: prefix, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if text.hasPrefix("<html") || text.hasPrefix("<!doctype") { throw AppleDMGDownloadError.authorizationExpired }
    }
}
