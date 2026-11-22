import BrinewellCore
import Darwin
import Foundation
import SwiftUI

@MainActor
final class DiskDeployment: ObservableObject {
    enum Phase { case idle, downloading, verifying, preparing, paused, failed }
    enum Source: String, CaseIterable { case valve, local }

    @Published var source: Source = .valve
    @Published var installerURL: URL?
    @Published var destination: URL
    @Published var capacityGiB: Int = 128
    @Published private(set) var phase: Phase = .idle
    @Published private(set) var progress: Double?
    @Published private(set) var receivedBytes: Int64 = 0
    @Published private(set) var totalBytes: Int64?
    @Published private(set) var detail = ""
    @Published private(set) var error: String?

    var onInstalled: ((URL) -> Void)?
    var busy: Bool { task != nil }
    static let capacitiesGiB = SteamOSDisk.capacitiesGiB

    private var task: Task<Void, Never>?
    private var download: SteamOSDownload?
    private var preparation: SteamOSPreparation?

    init() {
        let support = (try? ConfigurationStore.defaultURL().deletingLastPathComponent()) ??
            FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Brinewell")
        destination = support.appendingPathComponent("VirtualMachines", isDirectory: true)
    }

    func install() {
        guard !busy else { return }
        let selectedSource = source
        let selectedInstaller = installerURL
        let targetDirectory = destination
        let capacity = capacityGiB
        error = nil
        progress = nil
        receivedBytes = 0
        totalBytes = nil
        phase = selectedSource == .valve ? .downloading : .verifying
        detail = selectedSource == .valve ? String(localized: "Downloading SteamOS") : String(localized: "Verifying installation image")
        task = Task { [weak self] in
            guard let self else { return }
            do {
                let result = try await deploy(source: selectedSource, installer: selectedInstaller,
                                              destination: targetDirectory, capacity: capacity)
                phase = .idle
                detail = ""
                progress = nil
                task = nil
                preparation = nil
                download = nil
                onInstalled?(result)
            } catch {
                if Task.isCancelled || error is CancellationError {
                    let wasDownloading = phase == .downloading
                    phase = .paused
                    detail = wasDownloading ? String(localized: "Download paused") : String(localized: "Installation canceled")
                } else {
                    phase = .failed
                    self.error = Self.message(for: error)
                    detail = ""
                }
                task = nil
                preparation = nil
                download = nil
            }
        }
    }

    func cancel() async {
        let active = task
        active?.cancel()
        download?.cancel()
        preparation?.cancel()
        await active?.value
    }

    private func deploy(source: Source, installer: URL?, destination: URL, capacity: Int) async throws -> URL {
        guard SteamOSDisk.capacitiesGiB.contains(capacity) else {
            throw SteamOSDeploymentError(String(localized: "Choose a valid disk capacity."))
        }
        if source == .local, installer == nil { throw SteamOSDeploymentError(String(localized: "Choose an installation image first.")) }
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
        let work = destination.appendingPathComponent(".brinewell-install-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: work) }
        let worker = SteamOSPreparation()
        preparation = worker
        let raw: URL
        if source == .valve {
            let cacheRoot = try FileManager.default.url(for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            let cache = cacheRoot.appendingPathComponent("Brinewell/Installers", isDirectory: true)
            try FileManager.default.createDirectory(at: cache, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            let archive = cache.appendingPathComponent(SteamOSRelease.current.url.lastPathComponent)
            let downloader = SteamOSDownload()
            download = downloader
            _ = try await downloader.download(to: archive) { [weak self] received, total in
                Task { @MainActor [weak self] in
                    guard let self, phase == .downloading else { return }
                    receivedBytes = received
                    totalBytes = total
                    progress = total > 0 ? Double(received) / Double(total) : nil
                }
            }
            download = nil
            try Task.checkCancellation()
            phase = .verifying
            progress = nil
            detail = String(localized: "Verifying installation image")
            raw = cache.appendingPathComponent(archive.deletingPathExtension().lastPathComponent)
            let archiveIdentity = try Data(contentsOf: archive.appendingPathExtension("json"))
            let rawIdentityURL = raw.appendingPathExtension("json")
            let savedIdentity = try? Data(contentsOf: rawIdentityURL)
            let rawIsValid = savedIdentity == archiveIdentity && (try? SteamOSDisk.validateInstaller(at: raw)) != nil
            if !rawIsValid {
                let unpacked = cache.appendingPathComponent("\(UUID().uuidString).unpacking")
                defer { try? FileManager.default.removeItem(at: unpacked) }
                do {
                    try await run(worker) { try worker.extract(archive: archive, to: unpacked) }
                } catch {
                    if !Task.isCancelled {
                        try? FileManager.default.removeItem(at: archive)
                        try? FileManager.default.removeItem(at: archive.appendingPathExtension("json"))
                    }
                    throw error
                }
                // These are app-owned cache files, never the user's selected image.
                if FileManager.default.fileExists(atPath: raw.path) { try FileManager.default.removeItem(at: raw) }
                try FileManager.default.moveItem(at: unpacked, to: raw)
                try archiveIdentity.write(to: rawIdentityURL, options: .atomic)
            }
        } else {
            let selected = installer!
            let name = selected.lastPathComponent.lowercased()
            guard name.hasSuffix(".img") || name.hasSuffix(".raw") || name.hasSuffix(".img.bz2") else {
                throw SteamOSDeploymentError(String(localized: "Choose an installation image in .img, .raw, or .img.bz2 format."))
            }
            if name.hasSuffix(".bz2") {
                raw = work.appendingPathComponent("installer.img")
                try await run(worker) { try worker.extract(archive: selected, to: raw) }
            } else {
                raw = selected
                try await run(worker) { try SteamOSDisk.validateInstaller(at: raw) }
            }
        }
        try Task.checkCancellation()
        phase = .preparing
        progress = nil
        detail = String(localized: "Creating system disk")
        let target = work.appendingPathComponent("SteamOS.img")
        let diskGUID = try SteamOSDisk.createBlank(at: target, capacityGiB: capacity)
        let bundle = Bundle.main
        guard let resources = bundle.resourceURL?.appendingPathComponent("Runtime", isDirectory: true) else {
            throw SteamOSDeploymentError(String(localized: "Brinewell runtime components are missing. Reinstall the app."))
        }
        let helper = bundle.bundleURL.appendingPathComponent("Contents/MacOS/brinewell-vm")
        let owner = self
        try await run(worker) {
            try worker.prepare(source: raw, target: target, diskGUID: diskGUID, helper: helper,
                               resources: resources, runDirectory: work) { message in
                Task { @MainActor in
                    guard owner.phase == .preparing else { return }
                    owner.detail = Self.preparationDetail(message)
                }
            }
        }
        try Task.checkCancellation()
        return try Self.publish(target, into: destination)
    }

    private func run(_ worker: SteamOSPreparation, operation: @escaping @Sendable () throws -> Void) async throws {
        let background = Task.detached(priority: .userInitiated) { try operation() }
        try await withTaskCancellationHandler {
            try await background.value
        } onCancel: {
            worker.cancel()
        }
    }

    private static func publish(_ source: URL, into directory: URL) throws -> URL {
        for number in 1...1000 {
            let name = number == 1 ? "SteamOS.img" : "SteamOS \(number).img"
            let target = directory.appendingPathComponent(name)
            if renamex_np(source.path, target.path, UInt32(RENAME_EXCL)) == 0 { return target }
            if errno != EEXIST { throw SteamOSDeploymentError(String(localized: "Could not save the system disk. Check permissions and available space in the destination folder.")) }
        }
        throw SteamOSDeploymentError(String(localized: "This folder contains too many disks with the same name. Choose another location."))
    }

    private static func preparationDetail(_ message: String) -> String {
        if message.localizedCaseInsensitiveContains("rootfs-b") || message.contains("备用") { return String(localized: "Preparing the fallback system") }
        if message.localizedCaseInsensitiveContains("boot") || message.contains("启动") { return String(localized: "Configuring boot entries") }
        if message.localizedCaseInsensitiveContains("format") { return String(localized: "Formatting the system disk") }
        if message.localizedCaseInsensitiveContains("ready") || message.contains("完成") { return String(localized: "Finishing up") }
        return String(localized: "Installing SteamOS")
    }

    private static func message(for error: Error) -> String {
        if let error = error as? SteamOSDeploymentError { return error.localizedDescription }
        if let error = error as? URLError {
            switch error.code {
            case .notConnectedToInternet, .networkConnectionLost, .timedOut, .cannotConnectToHost, .cannotFindHost:
                return String(localized: "The download was interrupted. Check your connection and try again.")
            default: return String(localized: "Could not download the image from Valve. Try again or choose a local installation image.")
            }
        }
        if let error = error as? CocoaError, error.code == .fileWriteOutOfSpace {
            return String(localized: "There is not enough disk space. Free some space or choose another location.")
        }
        return String(localized: "Installation did not complete: \(error.localizedDescription)")
    }
}
