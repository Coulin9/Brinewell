import BrinewellCore
import Foundation
import SwiftUI

@MainActor
final class D3DMetalDeployment: ObservableObject {
    enum Phase: Equatable {
        case idle, signingIn, fetchingCatalog
        case downloading(artifactName: String, receivedBytes: Int64, totalBytes: Int64?)
        case installing(D3DMetalInstallStage)
        case failed(String)
    }

    @Published var phase: Phase = .idle {
        didSet {
            if case .failed = phase { onUnsuccessfulFinish?() }
        }
    }
    @Published var isPresentingLoginSheet = false
    @Published var rosettaAvailable = D3DMetalFramework.isRosettaAvailable()
    @Published var lastInstalledInspection: D3DMetalInspection?
    var onInstalled: ((D3DMetalInspection) -> Void)?
    var onUnsuccessfulFinish: (() -> Void)?
    var cancelWebDownload: (() async -> Void)?
    var configurationStore: ConfigurationStore?

    private var task: Task<Void, Never>?

    var isActive: Bool {
        switch phase {
        case .signingIn, .fetchingCatalog, .downloading, .installing: true
        case .idle, .failed: false
        }
    }

    static var downloadDirectory: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Caches/Brinewell/D3DMetalDownloads", isDirectory: true)
    }

    func startAppleDownload() {
        guard !isActive, task == nil else { return }
        phase = .signingIn
        isPresentingLoginSheet = true
    }

    func handleAuthorizedDownload(artifact: AppleDownloadArtifact, authorization: AppleDownloadAuthorization) {
        guard task == nil else { return }
        isPresentingLoginSheet = false
        phase = .downloading(artifactName: artifact.releaseName, receivedBytes: 0, totalBytes: artifact.fileSize)
        task = Task { [weak self] in
            guard let self else { return }
            do {
                let url = try await AppleDMGDownloader().download(
                    artifact: artifact, authorization: authorization, destinationDirectory: Self.downloadDirectory
                ) { [weak self] progress in
                    Task { @MainActor [weak self] in
                        guard let self, case .downloading = self.phase else { return }
                        self.phase = .downloading(artifactName: artifact.releaseName,
                                                  receivedBytes: progress.bytesReceived,
                                                  totalBytes: progress.totalBytes)
                    }
                }
                try Task.checkCancellation()
                try await install(url, deleteSourceOnSuccess: true)
            } catch { finish(error: error) }
        }
    }

    func handleWebKitDownloadedDMG(fileURL: URL, artifactName: String) {
        guard task == nil else { return }
        isPresentingLoginSheet = false
        beginInstall(fileURL, deleteSourceOnSuccess: true)
    }

    func importLocalItem(at url: URL) {
        guard !isActive, task == nil else { return }
        beginInstall(url, deleteSourceOnSuccess: false)
    }

    func refreshRosettaStatus() {
        rosettaAvailable = D3DMetalFramework.isRosettaAvailable()
    }

    func cancelAndWait() async {
        let active = task
        active?.cancel()
        await cancelWebDownload?()
        cancelWebDownload = nil
        isPresentingLoginSheet = false
        await active?.value
        phase = .idle
        onUnsuccessfulFinish?()
    }

    private func beginInstall(_ url: URL, deleteSourceOnSuccess: Bool) {
        phase = .installing(.mounting)
        task = Task { [weak self] in
            guard let self else { return }
            do { try await install(url, deleteSourceOnSuccess: deleteSourceOnSuccess) }
            catch { finish(error: error) }
        }
    }

    private func install(_ url: URL, deleteSourceOnSuccess: Bool) async throws {
        guard let configurationStore else { throw CocoaError(.fileNoSuchFile) }
        phase = .installing(.mounting)
        let result = try await D3DMetalInstaller().install(
            from: url, configurationStore: configurationStore, deleteSourceOnSuccess: deleteSourceOnSuccess
        ) { [weak self] stage in
            Task { @MainActor [weak self] in
                guard let self, case .installing = self.phase else { return }
                self.phase = .installing(stage)
            }
        }
        lastInstalledInspection = result.inspection
        task = nil
        phase = .idle
        onInstalled?(result.inspection)
    }

    private func finish(error: Error) {
        let cancelled = Task.isCancelled || error is CancellationError
            || (error as? AppleDMGDownloadError) == .cancelled
            || (error as? D3DMetalInstallerError) == .cancelled
        task = nil
        phase = cancelled ? .idle : .failed(error.localizedDescription)
        if cancelled { onUnsuccessfulFinish?() }
    }
}
