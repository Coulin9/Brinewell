import Foundation
import XCTest
@testable import BrinewellCore

final class PCGamingWikiResolverTests: XCTestCase, @unchecked Sendable {
    private func candidate(_ versions: String, appIDs: String = "730", pageID: Int = 1, date: String = "") -> PCGamingWikiCandidate {
        let wiki = """
        {{Infobox game|steam appid = \(appIDs)|release dates = {{Infobox game/row/date|Windows|\(date)}}}}
        {{API|direct3d versions = \(versions)}}
        """
        return PCGamingWikiParser.parse(pageID: pageID, title: "Game", wikitext: wiki)
    }

    func testDirect3DTiersAndAnnotations() {
        for (value, expected) in [("12", GraphicsBackend.neptune), ("11", .neptune), ("11, 12", .neptune),
                                  ("9.0c, 10", .mullion), ("10", .mullion), ("10.1", .mullion),
                                  ("10-11", .neptune), ("9Ex", .venus), ("true", .venus), ("false", .venus),
                                  ("unknown", .venus), ("", .venus), ("n/a", .venus), ("DirectDraw", .venus)] {
            let parsed = candidate(value + " {{note|12}} <ref>12</ref> <!-- 12 -->")
            XCTAssertEqual(PCGamingWikiParser.recommendedBackend(forDirect3DMajors: parsed.direct3DMajors), expected, value)
        }
        let parsed = PCGamingWikiParser.parse(pageID: 1, title: "API", wikitext: "{{API|vulkan versions=1.3|opengl versions=4.6|direct3d notes=12}}")
        XCTAssertTrue(parsed.hasVulkan)
        XCTAssertTrue(parsed.hasOpenGL)
        XCTAssertTrue(parsed.direct3DMajors.isEmpty)
    }

    func testTemplateAnnotationsPrefixesAndInvalidDates() {
        let wiki = """
        <nowiki/><nowiki name="empty"/>
        {{note|{{API|direct3d versions=12}}}}
        {{API|direct3d versions=DX11, D3D10.1, DirectX 12, foo11bar}}
        {{Infobox game|release dates={{note|{{Infobox game/row/date|Windows|1999}}}}{{Infobox game/row/date|Windows|2024-00-00}}{{Infobox game/row/date|Windows|2024-01-00}}}}
        """
        let result = PCGamingWikiParser.parse(pageID: 1, title: "Test", wikitext: wiki)
        XCTAssertEqual(result.direct3DMajors, [10, 11, 12])
        for (value, expected) in [("DX11", 11), ("DX12", 12), ("D3D10.1", 10), ("D3D11", 11), ("Direct3D 11", 11), ("DirectX 12", 12)] {
            XCTAssertEqual(candidate(value).direct3DMajors, [expected], value)
        }
        XCTAssertNil(result.windowsReleaseDate)
        for value in ["foo11bar", "{{note|{{API|direct3d versions=12}}}}"] {
            let text = value.hasPrefix("{{") ? value : "{{API|direct3d versions=\(value)}}"
            XCTAssertTrue(PCGamingWikiParser.parse(pageID: 1, title: "Test", wikitext: text).direct3DMajors.isEmpty)
        }
        for value in ["2024-00-00", "2024-01-00", "2024-+1-01"] {
            XCTAssertNil(candidate("9", date: value).windowsReleaseDate, value)
        }
    }

    func testUnterminatedRefsAndDeadline() {
        let pathological = String(repeating: "<ref ", count: 100_000)
        let prefix = "{{Infobox game|steam appid=730}}{{API|direct3d versions=11}}"
        let result = PCGamingWikiParser.parse(pageID: 1, title: "Test", wikitext: prefix + pathological)
        XCTAssertEqual(result.appIDs, [730])
        XCTAssertEqual(result.direct3DMajors, [11])
        var checks = 0
        let expired = PCGamingWikiParser.parse(pageID: 1, title: "Test", wikitext: prefix + pathological, isExpired: {
            checks += 1
            return checks >= 3
        })
        XCTAssertEqual(checks, 3)
        XCTAssertTrue(expired.appIDs.isEmpty)
    }

    func testExactIDsAndSharedAppIDSelection() {
        let text = "{{Infobox game|steam appid = 7300, 1730020 730|steam appid side=17300}}"
        let ids = PCGamingWikiParser.parse(pageID: 1, title: "IDs", wikitext: text).appIDs
        XCTAssertEqual(ids, [730, 7300, 1730020])
        XCTAssertFalse(ids.contains(17300))
        let cs2 = candidate("11", pageID: 186284, date: "September 27, 2023")
        let csgo = candidate("9", pageID: 196, date: "August 21, 2012")
        for candidates in [[cs2, csgo], [csgo, cs2]] {
            let best = PCGamingWikiParser.selectBestCandidate(candidates, for: 730)
            XCTAssertEqual(best?.pageID, 186284)
            XCTAssertEqual(best.map { PCGamingWikiParser.recommendedBackend(forDirect3DMajors: $0.direct3DMajors) }, .neptune)
        }
    }

    func testNetworkPaginationAndCacheAvailability() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        func page(_ id: Int, _ date: String, _ version: String) -> [String: Any] {
            ["pageid": id, "ns": 0, "title": "Game", "revisions": [["slots": ["main": ["content": "{{Infobox game|steam appid=730|release dates={{Infobox game/row/date|Windows|\(date)}}}}{{API|direct3d versions=\(version)}}"]]]]]
        }
        func json(_ object: [String: Any]) throws -> Data { try JSONSerialization.data(withJSONObject: object) }
        WikiStub.state.reset([
            try json(["continue": ["continue": "-||", "geucontinue": "next"], "query": ["pages": [page(196, "August 21, 2012", "9")]]]),
            try json(["batchcomplete": true, "query": ["pages": [page(186284, "September 27, 2023", "11")]]])
        ])
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [WikiStub.self]
        let resolver = PCGamingWikiResolver(cacheDirectory: directory, sessionConfiguration: configuration)
        let fallback = await resolver.resolveBackend(for: 730, availableBackends: [.venus])
        XCTAssertEqual(fallback, .venus)
        let recommended = await resolver.resolveBackend(for: 730, availableBackends: [.venus, .neptune])
        XCTAssertEqual(recommended, .neptune)
        XCTAssertEqual(WikiStub.state.count, 2)
        let cached = try JSONSerialization.jsonObject(with: Data(contentsOf: directory.appendingPathComponent("730.json"))) as? [String: Any]
        XCTAssertEqual(cached?["pageID"] as? Int, 186284)
        XCTAssertEqual(cached?["backend"] as? String, "neptune")
    }

    func testEarliestWindowsReleaseAndNestedValues() {
        let text = """
        <!-- {{API|direct3d versions=12}} -->
        {{Infobox_game|steam_appid=42 {{note|730}}|release dates=
        {{Infobox game/row/date|Windows|2024-03-01|note={{note|x|y}}}}
        {{Infobox game/row/date|Microsoft Windows|January 2010}}
        {{Infobox game/row/date|Linux|1999}}}}
        {{API|direct3d versions=10.0{{note|12}}|direct3d notes=12}}
        """
        let result = PCGamingWikiParser.parse(pageID: 42, title: "Test", wikitext: text)
        XCTAssertEqual(result.appIDs, [42])
        XCTAssertEqual(result.windowsReleaseDate, PCGamingWikiReleaseDate(year: 2010, month: 1, day: 0))
        XCTAssertEqual(result.direct3DMajors, [10])
    }
}

private final class WikiStub: URLProtocol, @unchecked Sendable {
    enum Reply {
        case response(Int, Data)
        case failure(URLError.Code)
        case redirect(URL)
        case held(Data, XCTestExpectation, DispatchSemaphore)
    }
    final class State: @unchecked Sendable {
        private let lock = NSLock()
        private var replies: [Reply] = []
        private var requests = 0
        var count: Int { lock.withLock { requests } }
        func reset(_ values: [Data]) { resetReplies(values.map { .response(200, $0) }) }
        func resetReplies(_ values: [Reply]) { lock.withLock { replies = values; requests = 0 } }
        func next() -> Reply? { lock.withLock { requests += 1; return replies.isEmpty ? nil : replies.removeFirst() } }
    }
    static let state = State()
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        switch Self.state.next() {
        case .response(let status, let body):
            let response = HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: "HTTP/1.1", headerFields: nil)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: body)
            client?.urlProtocolDidFinishLoading(self)
        case .held(let body, let started, let release):
            started.fulfill()
            release.wait()
            let response = HTTPURLResponse(url: request.url!, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: nil)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocol(self, didLoad: body)
            client?.urlProtocolDidFinishLoading(self)
        case .redirect(let url):
            let response = HTTPURLResponse(url: request.url!, statusCode: 302, httpVersion: "HTTP/1.1", headerFields: ["Location": url.absoluteString])!
            client?.urlProtocol(self, wasRedirectedTo: URLRequest(url: url), redirectResponse: response)
            client?.urlProtocolDidFinishLoading(self)
        case .failure(let code):
            client?.urlProtocol(self, didFailWithError: URLError(code))
        case nil:
            client?.urlProtocol(self, didFailWithError: URLError(.notConnectedToInternet))
        }
    }
    override func stopLoading() {}
}


private final class WikiTestClock: @unchecked Sendable {
    private let lock = NSLock()
    private var value = Date(timeIntervalSince1970: 1_700_000_000)
    func now() -> Date { lock.withLock { value } }
    func advance(_ seconds: TimeInterval) { lock.withLock { value.addTimeInterval(seconds) } }
}

extension PCGamingWikiResolverTests {
    private func response(appID: UInt32 = 730, version: String = "11", pageID: Int = 1,
                          date: String = "2023", continuation: String? = nil) throws -> Data {
        let content = "{{Infobox game|steam appid=\(appID)|release dates={{Infobox game/row/date|Windows|\(date)}}}}{{API|direct3d versions=\(version)}}"
        let page: [String: Any] = ["pageid": pageID, "ns": 0, "title": "Game", "revisions": [["slots": ["main": ["content": content]]]]]
        var object: [String: Any] = ["query": ["pages": [page]]]
        if let continuation { object["continue"] = ["continue": "-||", "geucontinue": continuation] }
        else { object["batchcomplete"] = true }
        return try JSONSerialization.data(withJSONObject: object)
    }

    private func resolver(_ directory: URL, clock: WikiTestClock = WikiTestClock()) -> PCGamingWikiResolver {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [WikiStub.self]
        return PCGamingWikiResolver(cacheDirectory: directory, sessionConfiguration: configuration, now: { clock.now() })
    }

    private func temporaryDirectory() -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    }

    func testSteamGraphicsIndexLoading() throws {
        let file = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: file) }
        let json = #"{"schemaVersion":1,"generatedAt":"now","source":"PCGamingWiki","neptune":[0,1,2],"mullion":[2,3],"venus":[3,4]}"#
        try Data(json.utf8).write(to: file)
        let index = try SteamGraphicsIndex.load(from: file)
        XCTAssertEqual(index.count, 4)
        XCTAssertEqual(index.backend(for: 1), .neptune)
        XCTAssertEqual(index.backend(for: 2), .neptune)
        XCTAssertEqual(index.backend(for: 3), .mullion)
        XCTAssertEqual(index.backend(for: 4), .venus)
        XCTAssertNil(index.backend(for: 0))
        XCTAssertNil(index.backend(for: 5))
        try Data(json.replacingOccurrences(of: "schemaVersion\":1", with: "schemaVersion\":2").utf8).write(to: file)
        XCTAssertThrowsError(try SteamGraphicsIndex.load(from: file))
        XCTAssertNil(SteamGraphicsIndex.loadIfPresent(from: file))
        XCTAssertNil(SteamGraphicsIndex.loadIfPresent(from: file.appendingPathComponent("missing")))
    }

    func testBundledIndexAndDynamicCachePrecedence() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [WikiStub.self]
        let index = SteamGraphicsIndex(entries: [730: .mullion])
        let subject = PCGamingWikiResolver(cacheDirectory: directory, sessionConfiguration: config, bundledIndex: index)
        WikiStub.state.resetReplies([.response(403, Data())])
        let filtered = await subject.resolveForGuest(appID: 730, availableBackends: [.venus])
        let unfiltered = await subject.resolveForGuest(appID: 730, availableBackends: [.venus, .mullion])
        XCTAssertEqual(filtered, .concrete(.venus))
        XCTAssertEqual(unfiltered, .concrete(.mullion))
        XCTAssertEqual(WikiStub.state.count, 0)
        WikiStub.state.reset([try response()])
        _ = await PCGamingWikiResolver(cacheDirectory: directory, sessionConfiguration: config).recommendation(for: 730)
        let cached = await subject.resolveForGuest(appID: 730, availableBackends: [.venus, .neptune, .mullion])
        XCTAssertEqual(cached, .concrete(.neptune))
        XCTAssertEqual(WikiStub.state.count, 1)
    }

    func testMissProbesAndOnlyDefinitiveNegativeIsCached() async throws {
        for reply: WikiStub.Reply in [.response(403, Data()), .failure(.timedOut), .failure(.notConnectedToInternet)] {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            WikiStub.state.resetReplies([reply, reply])
            let subject = resolver(directory)
            let first = await subject.resolveForGuest(appID: 730, availableBackends: [.venus, .mullion])
            let second = await subject.resolveForGuest(appID: 730, availableBackends: [.venus])
            XCTAssertEqual(first, .probeLocal(allowMullion: true))
            XCTAssertEqual(second, .probeLocal(allowMullion: false))
            XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("730.json").path))
            XCTAssertEqual(WikiStub.state.count, 2)
        }
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        WikiStub.state.reset([Data(#"{"batchcomplete":true}"#.utf8)])
        let subject = resolver(directory)
        let result = await subject.resolveForGuest(appID: 730, availableBackends: [.mullion, .venus])
        XCTAssertEqual(result, .probeLocal(allowMullion: true))
        let cache = try JSONSerialization.jsonObject(with: Data(contentsOf: directory.appendingPathComponent("730.json"))) as? [String: Any]
        XCTAssertEqual(cache?["status"] as? String, "notFound")
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [WikiStub.self]
        let indexed = PCGamingWikiResolver(cacheDirectory: directory, sessionConfiguration: config, bundledIndex: SteamGraphicsIndex(entries: [730: .neptune]))
        let overriding = await indexed.resolveForGuest(appID: 730, availableBackends: [.neptune, .venus])
        XCTAssertEqual(overriding, .concrete(.neptune))
        XCTAssertEqual(WikiStub.state.count, 1)
    }

    func testMixedAPIsAndMaskedMarkup() {
        let cases: [(String, GraphicsBackend)] = [
            ("direct3d versions=9", .venus), ("direct3d versions=10", .mullion),
            ("direct3d versions=10.1", .mullion), ("direct3d versions=11", .neptune),
            ("direct3d versions=12", .neptune), ("direct3d versions=9, 10", .mullion),
            ("direct3d versions=10|vulkan versions=1.3", .mullion),
            ("direct3d versions=11|vulkan versions=1.3", .neptune),
            ("direct3d versions=12|vulkan versions=1.3", .neptune),
            ("vulkan versions=1.3", .venus), ("opengl versions=4.6", .venus),
            ("directdraw versions=7|software=true", .venus)
        ]
        for (fields, expected) in cases {
            let result = PCGamingWikiParser.parse(pageID: 1, title: "API", wikitext: "{{API|\(fields)}}")
            XCTAssertEqual(PCGamingWikiParser.recommendedBackend(forDirect3DMajors: result.direct3DMajors), expected, fields)
        }
        let text = "{{API|direct3d versions=9 {{note|outer {{note|12}}}} <ref>11</ref><ref name=\"x\"/> <!-- 12 --> <nowiki>11</nowiki>}}"
        let result = PCGamingWikiParser.parse(pageID: 1, title: "masked", wikitext: text)
        XCTAssertEqual(result.direct3DMajors, [9])
    }

    func testExactAppIDDoesNotMatchPrefixes() {
        for (field, target): (String, UInt32) in [("7300", 730), ("1730020", 17300)] {
            let result = PCGamingWikiParser.parse(pageID: 1, title: "IDs", wikitext: "{{Infobox game|steam appid=\(field)|steam appid side=\(target)}}")
            XCTAssertNil(PCGamingWikiParser.selectBestCandidate([result], for: target))
        }
    }

    func testSharedAppIDAcrossContinuationInEitherOrder() async throws {
        for reversed in [false, true] {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            let first = reversed ? (186284, "September 27, 2023", "11") : (196, "August 21, 2012", "9")
            let second = reversed ? (196, "August 21, 2012", "9") : (186284, "September 27, 2023", "11")
            WikiStub.state.reset([try response(version: first.2, pageID: first.0, date: first.1, continuation: "next"),
                                  try response(version: second.2, pageID: second.0, date: second.1)])
            let result = await resolver(directory).recommendation(for: 730)
            XCTAssertEqual(result, .neptune)
            let cached = try JSONSerialization.jsonObject(with: Data(contentsOf: directory.appendingPathComponent("730.json"))) as? [String: Any]
            XCTAssertEqual(cached?["pageID"] as? Int, 186284)
            XCTAssertEqual(WikiStub.state.count, 2)
        }
    }

    func testPositiveAndNegativeCacheExpiry() async throws {
        for negative in [false, true] {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            let clock = WikiTestClock()
            let initial = negative ? Data(#"{"batchcomplete":true}"#.utf8) : try response()
            WikiStub.state.reset([initial])
            let subject = resolver(directory, clock: clock)
            let first = await subject.recommendation(for: 730)
            XCTAssertEqual(first, negative ? .venus : .neptune)
            WikiStub.state.resetReplies([.failure(.notConnectedToInternet)])
            clock.advance((negative ? 86400 : 30 * 86400) - 1)
            let cached = await resolver(directory, clock: clock).recommendation(for: 730)
            XCTAssertEqual(cached, first)
            XCTAssertEqual(WikiStub.state.count, 0)
            WikiStub.state.reset([try response(version: "10")])
            clock.advance(1)
            let refreshed = await subject.recommendation(for: 730)
            XCTAssertEqual(refreshed, .mullion)
            XCTAssertEqual(WikiStub.state.count, 1)
        }
    }

    func testOfflineCacheHitAndSessionFallbackDoNotPoisonCache() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        WikiStub.state.reset([try response(version: "10")])
        let subject = resolver(directory)
        let fallback = await subject.resolveBackend(for: 730, availableBackends: [.venus, .neptune])
        XCTAssertEqual(fallback, .venus)
        let cached = await resolver(directory).resolveBackend(for: 730, availableBackends: [.venus, .mullion])
        XCTAssertEqual(cached, .mullion)
        XCTAssertEqual(WikiStub.state.count, 1)
    }

    func testTransientFailuresRetryInsteadOfCachingNotFound() async throws {
        let failures: [WikiStub.Reply] = [
            .response(500, Data()), .response(429, Data()),
            .response(200, Data(#"{"error":{"code":"bad"}}"#.utf8)),
            .response(200, Data("invalid json".utf8)), .failure(.notConnectedToInternet), .failure(.timedOut),
            .redirect(URL(string: "https://evil.example/wiki")!)
        ]
        for failure in failures {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            WikiStub.state.resetReplies([failure, .response(200, try response())])
            let subject = resolver(directory)
            let first = await subject.recommendation(for: 730)
            XCTAssertEqual(first, .venus)
            XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("730.json").path))
            let second = await subject.recommendation(for: 730)
            XCTAssertEqual(second, .neptune)
            XCTAssertEqual(WikiStub.state.count, 2)
        }
    }

    func testIncompleteTerminalResponsesAreNotCached() async throws {
        let page = """
        {"query":{"pages":[{"pageid":1,"ns":0,"title":"Game","revisions":[{"slots":{"main":{"content":"{{Infobox game|steam appid=730}}{{API|direct3d versions=12}}"}}}]}]}}
        """
        for incomplete in [#"{"query":{}}"#, page] {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            WikiStub.state.reset([Data(incomplete.utf8), try response()])
            let subject = resolver(directory)
            let first = await subject.recommendation(for: 730)
            XCTAssertEqual(first, .venus)
            XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("730.json").path))
            let second = await subject.recommendation(for: 730)
            XCTAssertEqual(second, .neptune)
            XCTAssertEqual(WikiStub.state.count, 2)
        }
    }

    func testExpiredPositiveCacheSurvivesTransientFailure() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let clock = WikiTestClock()
        WikiStub.state.resetReplies([.response(200, try response()), .failure(.timedOut), .response(200, try response(version: "10"))])
        let subject = resolver(directory, clock: clock)
        let initial = await subject.recommendation(for: 730)
        XCTAssertEqual(initial, .neptune)
        let file = directory.appendingPathComponent("730.json")
        let original = try Data(contentsOf: file)
        clock.advance(30 * 86400 + 1)
        let failed = await subject.recommendation(for: 730)
        XCTAssertEqual(failed, .venus)
        XCTAssertEqual(try Data(contentsOf: file), original)
        let retried = await subject.recommendation(for: 730)
        XCTAssertEqual(retried, .mullion)
        XCTAssertEqual(WikiStub.state.count, 3)
    }

    func testInvalidCacheEntriesAreRefetched() async throws {
        let clock = WikiTestClock()
        let now = clock.now().timeIntervalSinceReferenceDate
        let entries = ["not json", "{\"schemaVersion\":2,\"appID\":730,\"fetchedAt\":\(now),\"status\":\"found\",\"pageID\":1,\"backend\":\"venus\"}",
                       "{\"schemaVersion\":1,\"appID\":731,\"fetchedAt\":\(now),\"status\":\"found\",\"pageID\":1,\"backend\":\"venus\"}",
                       "{\"schemaVersion\":1,\"appID\":730,\"fetchedAt\":\(now + 301),\"status\":\"found\",\"pageID\":1,\"backend\":\"venus\"}"]
        for entry in entries {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try Data(entry.utf8).write(to: directory.appendingPathComponent("730.json"))
            WikiStub.state.reset([try response()])
            let result = await resolver(directory, clock: clock).recommendation(for: 730)
            XCTAssertEqual(result, .neptune)
            XCTAssertEqual(WikiStub.state.count, 1)
            let cached = try JSONSerialization.jsonObject(with: Data(contentsOf: directory.appendingPathComponent("730.json"))) as? [String: Any]
            XCTAssertEqual(cached?["backend"] as? String, "neptune")
        }
    }

    func testCacheWriteFailureDoesNotDiscardResult() async throws {
        let path = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: path) }
        try Data("blocking file".utf8).write(to: path)
        WikiStub.state.reset([try response()])
        let result = await resolver(path).recommendation(for: 730)
        XCTAssertEqual(result, .neptune)
        XCTAssertEqual(try Data(contentsOf: path), Data("blocking file".utf8))
    }

    func testConcurrentWaitersShareFetchWhenOneIsCancelled() async throws {
        let directory = temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let started = expectation(description: "shared request started")
        let waitersStarted = expectation(description: "other waiters started")
        waitersStarted.expectedFulfillmentCount = 8
        let release = DispatchSemaphore(value: 0)
        WikiStub.state.resetReplies([.held(try response(), started, release)])
        let subject = resolver(directory)
        let cancelled = Task { await subject.recommendation(for: 730) }
        await fulfillment(of: [started], timeout: 2)
        let waiters = (0..<8).map { _ in
            Task {
                waitersStarted.fulfill()
                return await subject.recommendation(for: 730)
            }
        }
        await fulfillment(of: [waitersStarted], timeout: 2)
        cancelled.cancel()
        release.signal()
        for waiter in waiters {
            let result = await waiter.value
            XCTAssertEqual(result, .neptune)
        }
        _ = await cancelled.value
        XCTAssertEqual(WikiStub.state.count, 1)
    }

    func testContinuationLoopAndPageLimitAreNotCached() async throws {
        for loop in [true, false] {
            let directory = temporaryDirectory()
            defer { try? FileManager.default.removeItem(at: directory) }
            let pages = try (0..<(loop ? 2 : 8)).map { try response(appID: 999, continuation: loop ? "same" : "cursor\($0)") }
            WikiStub.state.reset(pages + [try response()])
            let subject = resolver(directory)
            let first = await subject.recommendation(for: 730)
            XCTAssertEqual(first, .venus)
            XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("730.json").path))
            let second = await subject.recommendation(for: 730)
            XCTAssertEqual(second, .neptune)
            XCTAssertEqual(WikiStub.state.count, pages.count + 1)
        }
    }
}
