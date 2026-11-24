import Foundation
import XCTest
@testable import BrinewellCore

final class AppleDeveloperDownloadsTests: XCTestCase, @unchecked Sendable {
    private let version = GPTKVersion.recommended

    private func artifact(_ name: String = "Game_Porting_Toolkit_4.0_beta_3.dmg", path: String? = nil,
                          version: GPTKVersion = .recommended) -> AppleDownloadArtifact {
        AppleDownloadArtifact(id: path ?? "/Developer_Tools/\(name)", releaseName: "Game Porting Toolkit \(version.displayString)",
                              filename: name, relativePath: path ?? "/Developer_Tools/\(name)", fileSize: nil,
                              datePublished: nil, version: version)
    }

    func testVersionParsingAndOrdering() throws {
        for text in ["Game Porting Toolkit 4.0 beta 3", "Game_Porting_Toolkit_4.0_beta_3.dmg", "4.0b3", "4.0 beta 3", "Evaluation environment for Windows games 4.0 beta 3.dmg"] {
            XCTAssertEqual(GPTKVersion.parse(from: text), version, text)
        }
        XCTAssertEqual(GPTKVersion.parse(from: "Game Porting Toolkit 3.0")?.displayString, "3.0")
        XCTAssertEqual(GPTKVersion.parse(from: "Game_Porting_Toolkit_2.1.dmg")?.displayString, "2.1")
        XCTAssertEqual(GPTKVersion.parse(from: "4.0.1")?.displayString, "4.0.1")
        XCTAssertNil(GPTKVersion.parse(from: "build 2026"))
        let ordered = ["2.1", "3.0", "4.0b2", "4.0b3", "4.0"].compactMap(GPTKVersion.parse(from:))
        XCTAssertEqual(ordered.sorted(), ordered)
        XCTAssertEqual(try JSONDecoder().decode(GPTKVersion.self, from: JSONEncoder().encode(version)), version)
    }

    func testPathAndURLValidation() throws {
        XCTAssertEqual(try AppleDeveloperCatalog.validateRelativeDownloadPath("Developer_Tools/test.DMG"), "/Developer_Tools/test.DMG")
        for path in ["", "//evil/a.dmg", "https://evil/a.dmg", "/x/../a.dmg", "/x/%2e%2E/a.dmg", "/x/%252e%252e/a.dmg", "/a b.dmg", "/a\\b.dmg", "/a.dmg?x=1", "/a.dmg#x", "/a.zip"] {
            XCTAssertThrowsError(try AppleDeveloperCatalog.validateRelativeDownloadPath(path), path)
        }
        for url in ["http://download.developer.apple.com/a.dmg", "https://evil.example/a.dmg", "https://download.developer.apple.com:444/a.dmg", "https://user@download.developer.apple.com/a.dmg"] {
            XCTAssertThrowsError(try AppleDeveloperCatalog.validateDownloadURL(URL(string: url)!))
        }
        XCTAssertNoThrow(try AppleDeveloperCatalog.validateDownloadURL(URL(string: "https://download.developer.apple.com:443/a.dmg")!))
        XCTAssertEqual(artifact().portalDownloadURL.host, "developer.apple.com")
        XCTAssertEqual(artifact().directCDNURL.host, "download.developer.apple.com")
    }

    func testCatalogFiltersNoiseAndInvalidPaths() throws {
        let json = #"""
        {"resultCode":0,"downloads":[
          {"name":"Xcode 16.0","files":[{"filename":"Xcode.dmg","remotePath":"/Xcode.dmg"}]},
          {"name":"Game Porting Toolkit 4.0 beta 3","files":[
            {"filename":"Game_Porting_Toolkit_4.0_beta_3.dmg","remotePath":"Developer_Tools/Game_Porting_Toolkit_4.0_beta_3.dmg","fileSize":123},
            {"filename":"release notes.pdf","remotePath":"/notes.pdf"},
            {"filename":"Game_Porting_Toolkit_source.tar.gz","remotePath":"/source.tar.gz"},
            {"filename":"Windows_helper.dmg","remotePath":"/Windows_helper.dmg"},
            {"filename":"Game_Porting_Toolkit_4.0_beta_3.dmg","remotePath":"/../escape.dmg"},
            {"filename":"Game_Porting_Toolkit_4.0_beta_3.dmg","remotePath":"/%2e%2e/escape.dmg"},
            {"filename":"Game_Porting_Toolkit_4.0_beta_3.dmg","remotePath":"https://evil.example/file.dmg"},
            {"filename":"Game_Porting_Toolkit_4.0_beta_3.dmg"},
            {"filename":"Game_Porting_Toolkit_4.0_beta_3.dmg","remotePath":"/Developer_Tools/Game_Porting_Toolkit_4.0_beta_3.dmg"}]},
          {"title":"Game Porting Toolkit 3.0","files":[{"displayName":"Game_Porting_Toolkit_3.0.dmg","downloadPath":"/three.dmg"}]},
          {"description":"Evaluation environment for Windows games 2.1","files":[{"filename":"Evaluation_environment_for_Windows_games_2.1.dmg","path":"/two.dmg"}]},
          {"name":"Game Porting Toolkit 4.0"} ]}
        """#
        let result = try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data(json.utf8))
        XCTAssertEqual(result.map(\.version.displayString), ["4.0b3", "3.0", "2.1"])
        XCTAssertEqual(result.first?.fileSize, 123)
        XCTAssertThrowsError(try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data("<html>Sign in</html>".utf8))) { XCTAssertEqual($0 as? AppleDeveloperCatalogError, .authenticationRequired) }
        XCTAssertThrowsError(try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data("not json".utf8))) { XCTAssertEqual($0 as? AppleDeveloperCatalogError, .invalidJSON) }
        XCTAssertThrowsError(try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data(#"{"resultCode":1100}"#.utf8))) { XCTAssertEqual($0 as? AppleDeveloperCatalogError, .authenticationRequired) }
        XCTAssertThrowsError(try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data(#"{}"#.utf8))) { XCTAssertEqual($0 as? AppleDeveloperCatalogError, .unsupportedSchema) }
        XCTAssertThrowsError(try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data(#"{"downloads":[]}"#.utf8))) { XCTAssertEqual($0 as? AppleDeveloperCatalogError, .noMatchingArtifacts) }
    }

    func testSelectionPrioritiesAndTieBreaks() {
        let v4 = GPTKVersion(major: 4, minor: 1, patch: 0)
        let v3 = GPTKVersion(major: 3, minor: 0, patch: 0)
        let recommended = artifact()
        let newer = artifact("Game_Porting_Toolkit_4.1.dmg", version: v4)
        let older = artifact("Game_Porting_Toolkit_3.0.dmg", version: v3)
        XCTAssertEqual(AppleDeveloperCatalog.selectBestArtifact(from: [older, newer, recommended]), recommended)
        XCTAssertEqual(AppleDeveloperCatalog.selectBestArtifact(from: [older, newer]), newer)
        XCTAssertEqual(AppleDeveloperCatalog.selectBestArtifact(from: [older]), older)
        let primary = artifact("Game_Porting_Toolkit_4.0_beta_3.dmg", path: "/z.dmg")
        let other = artifact("other.dmg", path: "/a.dmg")
        XCTAssertEqual(AppleDeveloperCatalog.selectBestArtifact(from: [other, primary]), primary)
        let a = artifact("Evaluation_a.dmg", path: "/a.dmg")
        XCTAssertEqual(AppleDeveloperCatalog.selectBestArtifact(from: [primary, a]), a)
    }

    func testCookiesAndAuthorizationRedaction() throws {
        let url = URL(string: "https://download.developer.apple.com/Developer_Tools/test.dmg")!
        func cookie(_ name: String, _ value: String, _ domain: String, _ path: String = "/", expires: Date? = nil) -> HTTPCookie {
            var properties: [HTTPCookiePropertyKey: Any] = [.name: name, .value: value, .domain: domain, .path: path]
            if let expires { properties[.expires] = expires }
            return HTTPCookie(properties: properties)!
        }
        let cookies = [cookie("myacinfo", "secret", ".apple.com"), cookie("ADCDownloadAuth", "wrong", ".evil.com"),
                       cookie("ADCDownloadAuth", "expired", ".apple.com", expires: Date(timeIntervalSince1970: 1)),
                       cookie("ADCDownloadAuth", "token", ".developer.apple.com", "/Developer_Tools")]
        XCTAssertEqual(AppleDeveloperCatalog.extractDownloadAuthCookieHeader(from: cookies, for: url), "ADCDownloadAuth=token")
        XCTAssertNil(AppleDeveloperCatalog.extractDownloadAuthCookieHeader(from: Array(cookies.prefix(3)), for: url))
        XCTAssertNil(AppleDeveloperCatalog.extractDownloadAuthCookieHeader(from: cookies, for: URL(string: "http://download.developer.apple.com/test.dmg")!))
        let auth = try AppleDownloadAuthorization(downloadURL: url, cookieHeader: "ADCDownloadAuth=token")
        XCTAssertFalse(String(describing: auth).contains("token"))
        XCTAssertFalse(String(reflecting: auth).contains("token"))
        XCTAssertThrowsError(try AppleDownloadAuthorization(downloadURL: url, cookieHeader: "ADCDownloadAuth=token\r\nCookie: secret"))
        XCTAssertThrowsError(try AppleDownloadAuthorization(downloadURL: URL(string: "https://evil.example/a.dmg")!, cookieHeader: "ADCDownloadAuth=token"))
    }

    private func runDownload(_ responses: [StubProtocol.Reply], partial: String? = nil,
                             metadata: String? = nil) async throws -> (Data?, [URLRequest], [String], Error?) {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let item = artifact()
        if let partial { try Data(partial.utf8).write(to: directory.appendingPathComponent(item.filename + ".part")) }
        if let metadata { try Data(metadata.utf8).write(to: directory.appendingPathComponent(item.filename + ".meta.json")) }
        StubProtocol.state.reset(responses)
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [StubProtocol.self]
        let downloader = AppleDMGDownloader(sessionConfiguration: config)
        let auth = try AppleDownloadAuthorization(downloadURL: item.directCDNURL, cookieHeader: "ADCDownloadAuth=topsecret")
        var output: Data?
        var failure: Error?
        do { output = try Data(contentsOf: await downloader.download(artifact: item, authorization: auth, destinationDirectory: directory)) }
        catch { failure = error }
        let files = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        for file in files {
            let data = try Data(contentsOf: directory.appendingPathComponent(file))
            XCTAssertFalse(String(decoding: data, as: UTF8.self).contains("topsecret"))
        }
        return (output, StubProtocol.state.requests, files, failure)
    }

    private var metadata: String {
        #"{"relativePath":"/Developer_Tools/Game_Porting_Toolkit_4.0_beta_3.dmg","expectedLength":10,"etag":"\"v1\"","lastModified":null}"#
    }

    func testFreshAndResumedDownloads() async throws {
        let fresh = try await runDownload([.init(status: 200, headers: ["Content-Length": "10", "ETag": "\"v1\""], body: "0123456789")])
        XCTAssertNil(fresh.3)
        XCTAssertEqual(fresh.0, Data("0123456789".utf8))
        XCTAssertEqual(fresh.2, [artifact().filename])
        XCTAssertEqual(fresh.1.first?.value(forHTTPHeaderField: "Cookie"), "ADCDownloadAuth=topsecret")
        let resumed = try await runDownload([.init(status: 206, headers: ["Content-Length": "5", "Content-Range": "bytes 5-9/10", "ETag": "\"v1\""], body: "56789")], partial: "01234", metadata: metadata)
        XCTAssertNil(resumed.3)
        XCTAssertEqual(resumed.0, Data("0123456789".utf8))
        XCTAssertEqual(resumed.1.first?.value(forHTTPHeaderField: "Range"), "bytes=5-")
        XCTAssertEqual(resumed.1.first?.value(forHTTPHeaderField: "If-Range"), "\"v1\"")
    }

    func testSymlinkPartialRejectedAndExistingDownloadReplaced() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let item = artifact()
        let target = directory.appendingPathComponent("target")
        let part = directory.appendingPathComponent(item.filename + ".part")
        let final = directory.appendingPathComponent(item.filename)
        try Data("untouched".utf8).write(to: target)
        try FileManager.default.createSymbolicLink(at: part, withDestinationURL: target)
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [StubProtocol.self]
        let downloader = AppleDMGDownloader(sessionConfiguration: config)
        let auth = try AppleDownloadAuthorization(downloadURL: item.directCDNURL, cookieHeader: "ADCDownloadAuth=token")
        StubProtocol.state.reset([.init(status: 200, headers: ["Content-Length": "3"], body: "new")])
        do {
            _ = try await downloader.download(artifact: item, authorization: auth, destinationDirectory: directory)
            XCTFail("Expected symlink rejection")
        } catch let error as AppleDMGDownloadError {
            guard case .ioFailure = error else { return XCTFail("\(error)") }
        }
        XCTAssertEqual(try Data(contentsOf: target), Data("untouched".utf8))
        try FileManager.default.removeItem(at: part)
        try Data("old".utf8).write(to: final)
        StubProtocol.state.reset([.init(status: 200, headers: ["Content-Length": "3"], body: "new")])
        let output = try await downloader.download(artifact: item, authorization: auth, destinationDirectory: directory)
        XCTAssertEqual(output, final)
        XCTAssertEqual(try Data(contentsOf: final), Data("new".utf8))
    }

    func testIgnoredRangeAnd416Restart() async throws {
        let restart = try await runDownload([.init(status: 200, headers: ["Content-Length": "10"], body: "abcdefghij")], partial: "01234", metadata: metadata)
        XCTAssertNil(restart.3)
        XCTAssertEqual(restart.0, Data("abcdefghij".utf8))
        let retried = try await runDownload([.init(status: 416), .init(status: 200, headers: ["Content-Length": "10"], body: "abcdefghij")], partial: "01234", metadata: metadata)
        XCTAssertNil(retried.3)
        XCTAssertEqual(retried.0, Data("abcdefghij".utf8))
        XCTAssertEqual(retried.1.count, 2)
        XCTAssertNil(retried.1.last?.value(forHTTPHeaderField: "Range"))
    }

    func testUntrustedRedirectsAreRejected() async throws {
        for destination in ["https://evil.example/foo.dmg", "http://download.developer.apple.com/foo.dmg"] {
            let result = try await runDownload([.init(status: 302, headers: ["Location": destination])])
            XCTAssertEqual(result.3 as? AppleDMGDownloadError, .untrustedRedirect(destination))
        }
    }

    func testUnauthorizedAndInvalidRanges() async throws {
        for reply in [StubProtocol.Reply(status: 401), .init(status: 403), .init(status: 200, headers: ["Content-Type": "text/html"], body: "<html>login"), .init(status: 200, body: "<!DOCTYPE html>")] {
            let result = try await runDownload([reply])
            XCTAssertEqual(result.3 as? AppleDMGDownloadError, .authorizationExpired)
        }
        let badRange = try await runDownload([.init(status: 206, headers: ["Content-Range": "bytes 4-9/10"], body: "456789")], partial: "01234", metadata: metadata)
        XCTAssertEqual(badRange.3 as? AppleDMGDownloadError, .invalidContentRange)
    }
}

private final class StubProtocol: URLProtocol, @unchecked Sendable {
    struct Reply: Sendable {
        var status: Int
        var headers: [String: String] = [:]
        var body: String = ""
    }
    final class State: @unchecked Sendable {
        private let lock = NSLock()
        private var replies: [Reply] = []
        private var recorded: [URLRequest] = []
        var requests: [URLRequest] { lock.withLock { recorded } }
        func reset(_ values: [Reply]) { lock.withLock { replies = values; recorded = [] } }
        func next(_ request: URLRequest) -> Reply { lock.withLock { recorded.append(request); return replies.removeFirst() } }
    }
    static let state = State()
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        let reply = Self.state.next(request)
        let response = HTTPURLResponse(url: request.url!, statusCode: reply.status, httpVersion: "HTTP/1.1", headerFields: reply.headers)!
        if (300...399).contains(reply.status), let location = reply.headers["Location"], let url = URL(string: location) {
            client?.urlProtocol(self, wasRedirectedTo: URLRequest(url: url), redirectResponse: response)
            client?.urlProtocolDidFinishLoading(self)
            return
        }
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Data(reply.body.utf8))
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}
