import Darwin
import Foundation

public struct SteamOSRelease: Sendable {
    public let url: URL
    public let compressedBytes: Int64

    public static let current = SteamOSRelease(
        url: URL(string: "https://steamdeck-images.steamos.cloud/recovery/steamframe-oobe-repair-20260922.5153644-0.3.0.img.bz2")!,
        compressedBytes: 4_068_109_407
    )
}

/// Keeps the partial archive plus its HTTP identity so cancellation can resume safely.
public final class SteamOSDownload: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    struct Identity: Codable, Equatable {
        let url: URL
        let bytes: Int64
        let etag: String?
        let modified: String?

        var validator: String? {
            if let etag, !etag.hasPrefix("W/") { return etag }
            return modified
        }

        func canResume(_ previous: Identity, received: Int64) -> Bool {
            url == previous.url && bytes == previous.bytes && validator != nil &&
                validator == previous.validator && received > 0 && received <= bytes
        }
    }

    private struct Transfer {
        let url: URL
        let identity: Identity
        let file: FileHandle
        let progress: @Sendable (Int64, Int64) -> Void
        let continuation: CheckedContinuation<URL, Error>
        var received: Int64
        var responseAccepted = false
        var failure: Error?
        var lastUpdate = Date.distantPast
    }

    private let release: SteamOSRelease
    private let lock = NSLock()
    private var session: URLSession!
    private var task: URLSessionDataTask?
    private var transfer: Transfer?
    private var cancelled = false

    public init(release: SteamOSRelease = .current) {
        self.release = release
        super.init()
        let queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 60
        configuration.timeoutIntervalForResource = 24 * 60 * 60
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: queue)
    }

    public func download(to url: URL, progress: @escaping @Sendable (Int64, Int64) -> Void) async throws -> URL {
        defer { session.finishTasksAndInvalidate() }
        var head = URLRequest(url: release.url)
        head.httpMethod = "HEAD"
        head.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        let (_, response) = try await session.data(for: head)
        guard let response = response as? HTTPURLResponse, response.statusCode == 200,
              response.expectedContentLength == release.compressedBytes else {
            throw SteamOSDeploymentError(String(localized: "The Valve download is temporarily unavailable. Try again or choose a local installation image."))
        }
        let identity = Identity(url: release.url, bytes: release.compressedBytes,
                                etag: response.value(forHTTPHeaderField: "ETag"),
                                modified: response.value(forHTTPHeaderField: "Last-Modified"))
        let identityURL = url.appendingPathExtension("json")
        let previous = (try? Data(contentsOf: identityURL)).flatMap { try? JSONDecoder().decode(Identity.self, from: $0) }
        let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
        let existing = (attributes?[.size] as? NSNumber)?.int64Value ?? 0
        let supportsRanges = response.value(forHTTPHeaderField: "Accept-Ranges")?.lowercased() == "bytes"
        let resumable = previous.map { identity.canResume($0, received: existing) } == true
        if resumable, existing == identity.bytes {
            progress(existing, identity.bytes)
            return url
        }
        try Task.checkCancellation()
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let fd = open(url.path, O_RDWR | O_CREAT | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { throw SteamOSDeploymentError(String(localized: "Could not save the installation image. Check permissions for the download folder.")) }
        let file = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        let offset = resumable && supportsRanges ? existing : 0
        do {
            if offset == 0 { try file.truncate(atOffset: 0) }
            try file.seek(toOffset: UInt64(offset))
            try JSONEncoder().encode(identity).write(to: identityURL, options: .atomic)
        } catch {
            try? file.close()
            throw error
        }
        var request = URLRequest(url: release.url)
        request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        if offset > 0 {
            request.setValue("bytes=\(offset)-", forHTTPHeaderField: "Range")
            request.setValue(identity.validator, forHTTPHeaderField: "If-Range")
        }
        let downloadRequest = request
        // No delegate work starts until all transfer state is installed under the same lock.
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                lock.withLock {
                    if cancelled || Task.isCancelled {
                        try? file.close()
                        continuation.resume(throwing: CancellationError())
                        return
                    }
                    transfer = Transfer(url: url, identity: identity, file: file, progress: progress,
                                        continuation: continuation, received: offset)
                    let dataTask = session.dataTask(with: downloadRequest)
                    task = dataTask
                    dataTask.resume()
                }
            }
        } onCancel: {
            self.cancel()
        }
    }

    public func cancel() {
        lock.withLock {
            cancelled = true
            task?.cancel()
        }
    }

    public func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                           newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        let allowed = request.url?.scheme == "https" && request.url?.host == release.url.host
        completionHandler(allowed ? request : nil)
    }

    public func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                           completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        let accepted = lock.withLock { () -> Transfer? in
            guard var active = transfer, let http = response as? HTTPURLResponse else { return nil }
            do {
                guard http.value(forHTTPHeaderField: "Content-Encoding").map({ $0 == "identity" }) ?? true else {
                    throw SteamOSDeploymentError(String(localized: "The download server returned an unsupported format. Try again later."))
                }
                if let etag = active.identity.etag {
                    guard http.value(forHTTPHeaderField: "ETag") == etag else { throw changedSource() }
                } else if let modified = active.identity.modified {
                    guard http.value(forHTTPHeaderField: "Last-Modified") == modified else { throw changedSource() }
                }
                if http.statusCode == 200 {
                    guard response.expectedContentLength == active.identity.bytes else { throw changedSource() }
                    // A server may ignore Range. Restart in the same file instead of appending duplicate bytes.
                    if active.received > 0 {
                        try active.file.truncate(atOffset: 0)
                        try active.file.seek(toOffset: 0)
                        active.received = 0
                    }
                } else if http.statusCode == 206, active.received > 0 {
                    let expected = "bytes \(active.received)-\(active.identity.bytes - 1)/\(active.identity.bytes)"
                    guard http.value(forHTTPHeaderField: "Content-Range") == expected,
                          response.expectedContentLength == active.identity.bytes - active.received else { throw changedSource() }
                } else {
                    throw SteamOSDeploymentError(String(localized: "Download failed (HTTP \(http.statusCode)). Try again or choose a local installation image."))
                }
                active.responseAccepted = true
                transfer = active
                return active
            } catch {
                active.failure = error
                transfer = active
                return nil
            }
        }
        if let active = accepted { active.progress(active.received, active.identity.bytes) }
        completionHandler(accepted != nil ? .allow : .cancel)
    }

    public func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        let update = lock.withLock { () -> (Transfer?, Bool) in
            guard var active = transfer, active.responseAccepted, active.failure == nil else { return (nil, false) }
            var reportProgress = false
            do {
                guard data.count <= active.identity.bytes - active.received else { throw changedSource() }
                try active.file.write(contentsOf: data)
                active.received += Int64(data.count)
                if Date().timeIntervalSince(active.lastUpdate) >= 0.2 {
                    reportProgress = true
                    active.lastUpdate = Date()
                }
            } catch {
                active.failure = error
            }
            transfer = active
            return (reportProgress ? active : nil, active.failure != nil)
        }
        if let active = update.0 { active.progress(active.received, active.identity.bytes) }
        if update.1 { dataTask.cancel() }
    }

    public func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        let completed = lock.withLock { () -> (Transfer, Bool)? in
            guard let active = transfer else { return nil }
            transfer = nil
            self.task = nil
            return (active, cancelled)
        }
        guard let (active, wasCancelled) = completed else { return }
        do {
            try active.file.synchronize()
            try active.file.close()
            if wasCancelled { throw CancellationError() }
            if let failure = active.failure { throw failure }
            if let error { throw error }
            guard active.responseAccepted, active.received == active.identity.bytes else {
                throw SteamOSDeploymentError(String(localized: "The image download is incomplete. Resume the download."))
            }
            active.progress(active.received, active.identity.bytes)
            active.continuation.resume(returning: active.url)
        } catch {
            try? active.file.close()
            active.continuation.resume(throwing: error)
        }
    }

    private func changedSource() -> SteamOSDeploymentError {
        SteamOSDeploymentError(String(localized: "The Valve image changed during the download. Start the download again."))
    }
}
