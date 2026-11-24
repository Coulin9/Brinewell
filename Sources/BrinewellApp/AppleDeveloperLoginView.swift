import AppKit
import BrinewellCore
import SwiftUI
import WebKit

struct AppleDeveloperLoginView: View {
    enum CompletionEvent: Sendable {
        case authorizedForNativeDownload(AppleDownloadArtifact, AppleDownloadAuthorization)
        case downloadedToLocalFile(URL, artifactName: String)
        case cancelled
        case failed(String)
    }

    @StateObject private var session: LoginSession
    let deployment: D3DMetalDeployment

    init(stagingDirectory: URL, deployment: D3DMetalDeployment, onEvent: @escaping @MainActor (CompletionEvent) -> Void) {
        self.deployment = deployment
        _session = StateObject(wrappedValue: LoginSession(stagingDirectory: stagingDirectory, onEvent: onEvent))
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "lock.fill").accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    Text(session.host).font(.headline).textSelection(.enabled)
                    Text(session.status).font(.callout).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Cancel") { session.cancel() }.keyboardShortcut(.cancelAction)
            }.padding(16)
            Divider()
            LoginWebView(session: session).frame(minWidth: 620, minHeight: 560)
        }
        .onAppear { deployment.cancelWebDownload = { [weak session] in await session?.cancelAndWait() } }
        .onDisappear { session.teardown(); deployment.cancelWebDownload = nil }
    }
}

@MainActor
private final class LoginSession: NSObject, ObservableObject, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate {
    @Published var host = AppleDeveloperCatalog.downloadsPageURL.host ?? "developer.apple.com"
    @Published var status = LoginSession.signInStatus

    private static var signInStatus: String {
        String(localized: "Sign in with a free Apple Account to automatically download and configure D3DMetal.")
    }

    private let stagingDirectory: URL
    private let onEvent: @MainActor (AppleDeveloperLoginView.CompletionEvent) -> Void
    private let dataStore = WKWebsiteDataStore.nonPersistent()
    private weak var webView: WKWebView?
    private var queryTask: Task<Void, Never>?
    private var queryInFlight = false
    private var manualSelectionFallback = false
    private var selectedArtifact: AppleDownloadArtifact?
    private var activeDownload: WKDownload?
    private var progressObservation: NSKeyValueObservation?
    private var destination: URL?
    private var artifactName = ""
    private var hasEmittedTerminalEvent = false
    private var stopped = false

    init(stagingDirectory: URL, onEvent: @escaping @MainActor (AppleDeveloperLoginView.CompletionEvent) -> Void) {
        self.stagingDirectory = stagingDirectory
        self.onEvent = onEvent
    }

    func makeWebView() -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = dataStore
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.navigationDelegate = self
        view.uiDelegate = self
        webView = view
        view.load(URLRequest(url: AppleDeveloperCatalog.downloadsPageURL))
        return view
    }

    private func allowed(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "https", let host = url.host?.lowercased(),
              url.user == nil, url.password == nil, url.port == nil || url.port == 443 else { return false }
        return host == "apple.com" || host.hasSuffix(".apple.com") || host.hasSuffix(".cdn-apple.com") ||
            host == "icloud.com" || host.hasSuffix(".icloud.com")
    }

    private func isDownloadsPage(_ url: URL?) -> Bool {
        guard let url else { return false }
        return allowed(url) && url.host?.lowercased() == "developer.apple.com" && url.path.hasPrefix("/download")
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                 decisionHandler: @escaping @MainActor (WKNavigationActionPolicy) -> Void) {
        guard !stopped, let url = navigationAction.request.url, allowed(url) else { decisionHandler(.cancel); return }
        decisionHandler(navigationAction.shouldPerformDownload ? .download : .allow)
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse,
                 decisionHandler: @escaping @MainActor (WKNavigationResponsePolicy) -> Void) {
        guard !stopped, let url = navigationResponse.response.url, allowed(url) else { decisionHandler(.cancel); return }
        let isDMG = url.host?.lowercased() == AppleDeveloperCatalog.allowedDownloadHost && url.path.lowercased().hasSuffix(".dmg")
        decisionHandler(!navigationResponse.canShowMIMEType || isDMG ? .download : .allow)
    }

    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                 for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if let url = navigationAction.request.url, allowed(url), !stopped { webView.load(navigationAction.request) }
        return nil
    }

    func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
        if let url = webView.url, allowed(url), let currentHost = url.host { host = currentHost }
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        guard !stopped else { return }
        if let url = webView.url, allowed(url), let currentHost = url.host { host = currentHost }
        guard isDownloadsPage(webView.url), !manualSelectionFallback, selectedArtifact == nil, !queryInFlight else { return }
        queryTask?.cancel()
        queryTask = Task { [weak self, weak webView] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled, let self, let webView, self.isDownloadsPage(webView.url), !self.stopped else { return }
            await self.queryCatalog(in: webView)
        }
    }

    private func queryCatalog(in webView: WKWebView) async {
        guard !queryInFlight else { return }
        queryInFlight = true
        status = String(localized: "Matching the official Apple installation package…")
        defer { queryInFlight = false }
        do {
            let result = try await webView.callAsyncJavaScript("""
                const response = await fetch("https://developer.apple.com/services-account/QH65B2/downloadws/listDownloads.action", {
                    method: "POST", credentials: "include",
                    headers: { "Accept": "application/json, text/plain, */*", "X-Requested-With": "XMLHttpRequest" }
                });
                const text = await response.text();
                return { status: response.status, text: text };
                """, arguments: [:], in: nil, contentWorld: .page)
            guard !stopped, !Task.isCancelled, isDownloadsPage(webView.url),
                  let values = result as? [String: Any], let code = values["status"] as? Int,
                  let text = values["text"] as? String else { return }
            guard code == 200 else { resetStatus(); return }
            do {
                let artifacts = try AppleDeveloperCatalog.parseGamePortingToolkitArtifacts(from: Data(text.utf8))
                guard let artifact = AppleDeveloperCatalog.selectBestArtifact(from: artifacts) else { return }
                selectedArtifact = artifact
                webView.load(URLRequest(url: artifact.portalDownloadURL))
            } catch AppleDeveloperCatalogError.authenticationRequired {
                resetStatus()
            } catch AppleDeveloperCatalogError.unsupportedSchema {
                showFallback()
            } catch AppleDeveloperCatalogError.noMatchingArtifacts {
                showFallback()
            } catch AppleDeveloperCatalogError.invalidJSON {
                showFallback()
            } catch {
                resetStatus()
            }
        } catch {
            if !stopped { resetStatus() }
        }
    }

    private func resetStatus() {
        status = Self.signInStatus
    }

    private func showFallback() {
        manualSelectionFallback = true
        status = String(localized: "The package could not be matched automatically. Click a Game Porting Toolkit DMG download link below and the app will take over installation.")
    }

    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) {
        attach(download)
    }

    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) {
        attach(download)
    }

    private func attach(_ download: WKDownload) {
        guard !stopped, activeDownload == nil else { download.cancel(); return }
        activeDownload = download
        download.delegate = self
    }

    func download(_ download: WKDownload, willPerformHTTPRedirection response: HTTPURLResponse,
                  newRequest request: URLRequest,
                  decisionHandler: @escaping @MainActor (WKDownload.RedirectPolicy) -> Void) {
        guard !stopped, let url = request.url, url.scheme?.lowercased() == "https",
              let host = url.host?.lowercased(),
              host == "developer.apple.com" || host == "download.developer.apple.com",
              url.user == nil, url.password == nil, url.port == nil || url.port == 443 else {
            decisionHandler(.cancel)
            if !stopped { emit(.failed(String(localized: "The Apple download redirect could not be verified."))) }
            return
        }
        decisionHandler(.allow)
    }

    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String,
                  completionHandler: @escaping @MainActor (URL?) -> Void) {
        guard !stopped, let url = response.url, (try? AppleDeveloperCatalog.validateDownloadURL(url)) != nil else {
            completionHandler(nil)
            if !stopped { emit(.failed(String(localized: "The Apple download URL could not be verified."))) }
            return
        }
        guard let http = response as? HTTPURLResponse, http.statusCode == 200,
              response.mimeType?.lowercased() != "text/html" else {
            completionHandler(nil)
            emit(.failed(String(localized: "The Apple download returned an invalid response.")))
            return
        }
        guard response.expectedContentLength <= AppleDMGDownloader.maximumDMGBytes else {
            completionHandler(nil)
            emit(.failed(String(localized: "The Apple download exceeds the maximum permitted size.")))
            return
        }
        Task { @MainActor [weak self] in
            guard let self else { completionHandler(nil); return }
            let cookies = await self.dataStore.httpCookieStore.allCookies()
            guard !self.stopped else { completionHandler(nil); return }
            if let header = AppleDeveloperCatalog.extractDownloadAuthCookieHeader(from: cookies, for: url),
               let authorization = try? AppleDownloadAuthorization(downloadURL: url, cookieHeader: header) {
                let name = URL(fileURLWithPath: suggestedFilename).lastPathComponent
                let path = (try? AppleDeveloperCatalog.validateRelativeDownloadPath(url.path)) ?? url.path
                let artifact = self.selectedArtifact ?? AppleDownloadArtifact(
                    id: path, releaseName: name, filename: name, relativePath: path, fileSize: nil,
                    datePublished: nil, version: GPTKVersion.parse(from: name) ?? .recommended)
                completionHandler(nil)
                self.emit(.authorizedForNativeDownload(artifact, authorization))
                return
            }
            do {
                try FileManager.default.createDirectory(at: self.stagingDirectory, withIntermediateDirectories: true,
                                                        attributes: [.posixPermissions: 0o700])
                let destination = self.stagingDirectory.appendingPathComponent(UUID().uuidString + ".dmg")
                self.destination = destination
                self.artifactName = URL(fileURLWithPath: suggestedFilename).lastPathComponent
                self.progressObservation = download.progress.observe(\.completedUnitCount, options: [.initial, .new]) { [weak self] progress, _ in
                    guard progress.completedUnitCount > AppleDMGDownloader.maximumDMGBytes else { return }
                    Task { @MainActor [weak self] in
                        guard let self, !self.stopped else { return }
                        self.emit(.failed(String(localized: "The Apple download exceeds the maximum permitted size.")))
                    }
                }
                completionHandler(destination)
            } catch {
                completionHandler(nil)
                self.emit(.failed(error.localizedDescription))
            }
        }
    }

    func downloadDidFinish(_ download: WKDownload) {
        guard !stopped, let destination else { return }
        do {
            let attributes = try FileManager.default.attributesOfItem(atPath: destination.path)
            guard let size = (attributes[.size] as? NSNumber)?.int64Value,
                  size <= AppleDMGDownloader.maximumDMGBytes else {
                emit(.failed(String(localized: "The Apple download exceeds the maximum permitted size.")))
                return
            }
            let file = try FileHandle(forReadingFrom: destination)
            defer { try? file.close() }
            let prefix = try file.read(upToCount: 512) ?? Data()
            let text = String(decoding: prefix, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            guard !text.hasPrefix("<!doctype"), !text.hasPrefix("<html") else {
                emit(.failed(String(localized: "The Apple download returned an invalid response.")))
                return
            }
            self.destination = nil
            emit(.downloadedToLocalFile(destination, artifactName: artifactName))
        } catch {
            emit(.failed(error.localizedDescription))
        }
    }

    func download(_ download: WKDownload, didFailWithError error: Error, resumeData: Data?) {
        removePartialFile()
        if !stopped { emit(.failed(error.localizedDescription)) }
    }

    func cancel() { emit(.cancelled) }

    func cancelAndWait() async {
        let download = activeDownload
        activeDownload = nil
        teardown()
        if let download {
            await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
                download.cancel { _ in continuation.resume() }
            }
        }
    }

    private func emit(_ event: AppleDeveloperLoginView.CompletionEvent) {
        guard !hasEmittedTerminalEvent else { return }
        hasEmittedTerminalEvent = true
        teardown()
        onEvent(event)
    }

    func teardown() {
        guard !stopped else { return }
        stopped = true
        queryTask?.cancel()
        webView?.stopLoading()
        webView?.navigationDelegate = nil
        webView?.uiDelegate = nil
        progressObservation = nil
        activeDownload?.cancel()
        activeDownload = nil
        removePartialFile()
    }

    private func removePartialFile() {
        if let destination { try? FileManager.default.removeItem(at: destination) }
        destination = nil
    }
}

private struct LoginWebView: NSViewRepresentable {
    let session: LoginSession

    func makeNSView(context: Context) -> WKWebView { session.makeWebView() }
    func updateNSView(_ nsView: WKWebView, context: Context) {}
    static func dismantleNSView(_ nsView: WKWebView, coordinator: ()) { nsView.stopLoading() }
}
