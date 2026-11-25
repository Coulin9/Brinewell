import Foundation

public enum GameGraphicsResolution: Sendable, Equatable {
    case concrete(GraphicsBackend)
    case probeLocal(allowMullion: Bool)
}

public protocol GameGraphicsBackendResolving: Sendable {
    func resolveBackend(for appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GraphicsBackend
    func resolveForGuest(appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GameGraphicsResolution
}

public extension GameGraphicsBackendResolving {
    func resolveForGuest(appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GameGraphicsResolution {
        .concrete(await resolveBackend(for: appID, availableBackends: availableBackends))
    }
}

private final class WikiRedirectDelegate: NSObject, URLSessionTaskDelegate, Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        guard let url = request.url, url.scheme?.lowercased() == "https", url.host?.lowercased() == "www.pcgamingwiki.com",
              url.port == nil || url.port == 443, url.user == nil, url.password == nil else {
            completionHandler(nil); return
        }
        completionHandler(request)
    }
}

public actor PCGamingWikiResolver: GameGraphicsBackendResolving {
    private enum LookupOutcome: Sendable {
        case found(pageID: Int, backend: GraphicsBackend)
        case notFound
    }
    private enum LookupError: Error { case invalidResponse, limit, timeout }
    private struct CacheEntry: Codable {
        let schemaVersion: Int
        let appID: UInt32
        let fetchedAt: Date
        let status: String
        let pageID: Int?
        let backend: GraphicsBackend?
    }
    private struct Response: Decodable {
        struct Query: Decodable {
            struct Page: Decodable {
                struct Revision: Decodable {
                    struct Slot: Decodable { let content: String?; let texthidden: Bool?; let contenthidden: Bool? }
                    let slots: [String: Slot]?
                    let texthidden: Bool?
                    let contenthidden: Bool?
                }
                let pageid: Int
                let ns: Int
                let title: String
                let revisions: [Revision]?
            }
            let pages: [Page]?
        }
        let batchcomplete: Bool?
        let query: Query?
        let `continue`: [String: String]?
        let error: JSONValue?
        let warnings: JSONValue?
    }
    private struct JSONValue: Decodable {
        init(from decoder: Decoder) throws {
            let container = try decoder.singleValueContainer()
            if container.decodeNil() { return }
            if (try? container.decode(Bool.self)) != nil { return }
            if (try? container.decode(Double.self)) != nil { return }
            if (try? container.decode(String.self)) != nil { return }
            if (try? container.decode([JSONValue].self)) != nil { return }
            _ = try container.decode([String: JSONValue].self)
        }
    }

    private let bundledIndex: SteamGraphicsIndex?
    private let cacheDirectory: URL
    private let sessionConfiguration: URLSessionConfiguration
    private let deadline: TimeInterval
    private let now: @Sendable () -> Date
    private var inFlight: [UInt32: Task<LookupOutcome, Error>] = [:]

    public init(cacheDirectory: URL = FileManager.default.homeDirectoryForCurrentUser
                    .appendingPathComponent("Library/Caches/Brinewell/PCGamingWiki/v1", isDirectory: true),
                sessionConfiguration: URLSessionConfiguration = .ephemeral,
                deadline: TimeInterval = 4.0, now: @escaping @Sendable () -> Date = { Date() },
                bundledIndex: SteamGraphicsIndex? = nil) {
        self.bundledIndex = bundledIndex
        self.cacheDirectory = cacheDirectory
        let configuration = sessionConfiguration.copy() as! URLSessionConfiguration
        configuration.urlCache = nil
        configuration.httpCookieStorage = nil
        configuration.httpShouldSetCookies = false
        configuration.urlCredentialStorage = nil
        self.sessionConfiguration = configuration
        self.deadline = deadline
        self.now = now
    }

    public func resolveBackend(for appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GraphicsBackend {
        switch await resolveForGuest(appID: appID, availableBackends: availableBackends) {
        case .concrete(let backend): return backend
        case .probeLocal: return .venus
        }
    }

    public func recommendation(for appID: UInt32) async -> GraphicsBackend {
        await resolveBackend(for: appID, availableBackends: [.neptune, .mullion, .venus])
    }

    public func resolveForGuest(appID: UInt32, availableBackends: Set<GraphicsBackend>) async -> GameGraphicsResolution {
        guard appID != 0 else { return .concrete(.venus) }
        func concrete(_ backend: GraphicsBackend) -> GameGraphicsResolution {
            .concrete(availableBackends.contains(backend) ? backend : .venus)
        }
        let probe = GameGraphicsResolution.probeLocal(allowMullion: availableBackends.contains(.mullion))
        if let cached = readCache(appID) {
            switch cached {
            case .found(_, let backend): return concrete(backend)
            case .notFound:
                if let backend = bundledIndex?.backend(for: appID) { return concrete(backend) }
                return probe
            }
        }
        if let backend = bundledIndex?.backend(for: appID) { return concrete(backend) }
        let task: Task<LookupOutcome, Error>
        if let existing = inFlight[appID] {
            task = existing
        } else {
            guard inFlight.count < 32 else { return probe }
            task = Task { [self] in
                defer { inFlight[appID] = nil }
                let start = ContinuousClock.now
                let budget = Duration.seconds(max(0, deadline))
                let outcome = try await fetch(appID, start: start, budget: budget)
                guard ContinuousClock.now - start < budget else { throw LookupError.timeout }
                writeCache(outcome, appID: appID)
                return outcome
            }
            inFlight[appID] = task
        }
        guard let outcome = try? await task.value else { return probe }
        switch outcome {
        case .found(_, let backend): return concrete(backend)
        case .notFound: return probe
        }
    }

    private func readCache(_ appID: UInt32) -> LookupOutcome? {
        guard let data = try? Data(contentsOf: cacheDirectory.appendingPathComponent("\(appID).json")),
              let entry = try? JSONDecoder().decode(CacheEntry.self, from: data), entry.schemaVersion == 1,
              entry.appID == appID else { return nil }
        let age = now().timeIntervalSince(entry.fetchedAt)
        guard age >= -300 else { return nil }
        if entry.status == "notFound", age < 86400 { return .notFound }
        if entry.status == "found", age < 30 * 86400, let id = entry.pageID, let backend = entry.backend, backend.isConcrete {
            return .found(pageID: id, backend: backend)
        }
        return nil
    }

    private func writeCache(_ outcome: LookupOutcome, appID: UInt32) {
        let entry: CacheEntry
        switch outcome {
        case .found(let id, let backend):
            entry = CacheEntry(schemaVersion: 1, appID: appID, fetchedAt: now(), status: "found", pageID: id, backend: backend)
        case .notFound:
            entry = CacheEntry(schemaVersion: 1, appID: appID, fetchedAt: now(), status: "notFound", pageID: nil, backend: nil)
        }
        guard let data = try? JSONEncoder().encode(entry) else { return }
        do {
            try FileManager.default.createDirectory(at: cacheDirectory, withIntermediateDirectories: true)
            try data.write(to: cacheDirectory.appendingPathComponent("\(appID).json"), options: .atomic)
        } catch { /* Cache failures must not affect a valid result. */ }
    }

    private func fetch(_ appID: UInt32, start: ContinuousClock.Instant, budget: Duration) async throws -> LookupOutcome {
        let session = URLSession(configuration: sessionConfiguration, delegate: WikiRedirectDelegate(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        func checkDeadline() throws {
            guard ContinuousClock.now - start < budget else { throw LookupError.timeout }
        }
        var continuation: [String: String] = [:], seen = Set<String>()
        var pages: [Int: PCGamingWikiCandidate] = [:]
        for _ in 0..<8 {
            let remaining = budget - (ContinuousClock.now - start)
            guard remaining > .zero else { throw LookupError.timeout }
            var components = URLComponents(string: "https://www.pcgamingwiki.com/w/api.php")!
            var query = ["action": "query", "generator": "exturlusage", "geuprotocol": "https",
                         "geuquery": "store.steampowered.com/app/\(appID)", "geulimit": "50", "prop": "revisions",
                         "rvprop": "content", "rvslots": "main", "format": "json", "formatversion": "2"]
            for (key, value) in continuation { query[key] = value }
            components.queryItems = query.sorted { $0.key < $1.key }.map { URLQueryItem(name: $0.key, value: $0.value) }
            var request = URLRequest(url: components.url!)
            request.setValue("Brinewell/1.0 (+https://github.com/Coulin9/Brinewell)", forHTTPHeaderField: "User-Agent")
            let finalRequest = request
            let data: Data = try await withThrowingTaskGroup(of: Data.self) { group in
                group.addTask {
                    let (bytes, response) = try await session.bytes(for: finalRequest)
                    guard let http = response as? HTTPURLResponse, http.statusCode == 200 else { throw LookupError.invalidResponse }
                    var data = Data()
                    for try await byte in bytes {
                        guard data.count < 8 * 1024 * 1024 else { throw LookupError.limit }
                        data.append(byte)
                    }
                    return data
                }
                group.addTask { try await Task.sleep(for: remaining); throw LookupError.timeout }
                defer { group.cancelAll() }
                guard let result = try await group.next() else { throw LookupError.invalidResponse }
                return result
            }
            try checkDeadline()
            let response = try JSONDecoder().decode(Response.self, from: data)
            try checkDeadline()
            guard response.error == nil, response.warnings == nil else { throw LookupError.invalidResponse }
            if response.continue == nil && response.batchcomplete != true { throw LookupError.invalidResponse }
            for page in response.query?.pages ?? [] where page.ns == 0 {
                guard let revision = page.revisions?.first, revision.texthidden == nil, revision.contenthidden == nil,
                      let slot = revision.slots?["main"], slot.texthidden == nil, slot.contenthidden == nil,
                      let content = slot.content else { throw LookupError.invalidResponse }
                try checkDeadline()
                pages[page.pageid] = PCGamingWikiParser.parse(pageID: page.pageid, title: page.title, wikitext: content,
                                                              isExpired: { ContinuousClock.now - start >= budget })
                try checkDeadline()
            }
            guard let next = response.continue else {
                let candidate = PCGamingWikiParser.selectBestCandidate(Array(pages.values), for: appID)
                try checkDeadline()
                guard let candidate else { return .notFound }
                return .found(pageID: candidate.pageID, backend: PCGamingWikiParser.recommendedBackend(forDirect3DMajors: candidate.direct3DMajors))
            }
            let key = next.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: "&")
            guard seen.insert(key).inserted else { throw LookupError.limit }
            continuation = next
        }
        throw LookupError.limit
    }
}
