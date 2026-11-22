import Darwin
import Foundation

/// Runs the two blocking preparation tools off the UI thread and cancels the active child.
public final class SteamOSPreparation: @unchecked Sendable {
    private let lock = NSLock()
    private var process: Process?
    private var cancelled = false

    public init() {}

    public func cancel() {
        lock.withLock {
            cancelled = true
            if let process, process.isRunning { process.terminate() }
        }
    }

    public func extract(archive: URL, to output: URL) throws {
        try checkCancellation()
        var source = stat()
        guard lstat(archive.path, &source) == 0, source.st_mode & S_IFMT == S_IFREG,
              FileManager.default.isReadableFile(atPath: archive.path) else {
            throw SteamOSDeploymentError(String(localized: "Choose a readable .img.bz2 installation image."))
        }
        let descriptor = open(output.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard descriptor >= 0 else { throw SteamOSDeploymentError(String(localized: "Could not create a temporary image. Check disk space and folder permissions.")) }
        let file = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        var complete = false
        defer {
            try? file.close()
            if !complete { try? FileManager.default.removeItem(at: output) }
        }
        let pipe = Pipe()
        let child = Process()
        child.executableURL = URL(fileURLWithPath: "/usr/bin/bzip2")
        child.arguments = ["-dc", archive.path]
        child.standardOutput = pipe
        child.standardError = FileHandle.nullDevice
        try start(child)
        defer { finish(child) }
        var size: UInt64 = 0
        do {
            while let block = try pipe.fileHandleForReading.read(upToCount: 1024 * 1024), !block.isEmpty {
                try checkCancellation()
                size += UInt64(block.count)
                guard size <= 64 * 1_073_741_824 else {
                    throw SteamOSDeploymentError(String(localized: "The extracted installation image has an unexpected size. Choose an official Valve recovery image."))
                }
                if block.allSatisfy({ $0 == 0 }) {
                    try file.seek(toOffset: size)
                } else {
                    try file.write(contentsOf: block)
                }
            }
            child.waitUntilExit()
            try checkCancellation()
            guard child.terminationReason == .exit, child.terminationStatus == 0 else {
                throw SteamOSDeploymentError(String(localized: "The installation image failed verification or is incomplete. Download it again or choose a different image."))
            }
            try file.truncate(atOffset: size)
            try file.synchronize()
            try SteamOSDisk.validateInstaller(at: output)
            complete = true
        } catch {
            if child.isRunning { child.terminate() }
            child.waitUntilExit()
            throw error
        }
    }

    public func prepare(source: URL, target: URL, diskGUID: UUID, helper: URL, resources: URL,
                        runDirectory: URL, onProgress: @escaping @Sendable (String) -> Void) throws {
        try runDiskHelper(helper: helper, resources: resources, runDirectory: runDirectory,
                          arguments: ["--prepare-steamos", source.path, "--disk", target.path,
                                      "--disk-guid", diskGUID.uuidString.lowercased()],
                          onProgress: onProgress)
        try SteamOSDisk.validateSystem(at: target)
    }

    private func runDiskHelper(helper: URL, resources: URL, runDirectory: URL, arguments: [String],
                               onProgress: @escaping @Sendable (String) -> Void) throws {
        try checkCancellation()
        guard FileManager.default.isExecutableFile(atPath: helper.path),
              FileManager.default.fileExists(atPath: resources.appendingPathComponent("initramfs.cpio").path) ||
              FileManager.default.fileExists(atPath: resources.appendingPathComponent("initramfs.cpio.gz").path) else {
            throw SteamOSDeploymentError(String(localized: "Brinewell runtime components are incomplete. Reinstall the app."))
        }
        let child = Process()
        let pipe = Pipe()
        child.executableURL = helper
        child.arguments = arguments + ["--resources", resources.path, "--run-directory", runDirectory.path]
        child.standardOutput = pipe
        let errorURL = runDirectory.appendingPathComponent("preparation-host.log")
        guard FileManager.default.createFile(atPath: errorURL.path, contents: nil, attributes: [.posixPermissions: 0o600]) else {
            throw SteamOSDeploymentError(String(localized: "Could not save the installation log. Check permissions for the destination folder."))
        }
        let errorLog = try FileHandle(forWritingTo: errorURL)
        defer { try? errorLog.close() }
        child.standardError = errorLog
        try start(child)
        defer { finish(child) }
        var pending = Data()
        var done = false
        var failure: String?
        do {
            while let chunk = try pipe.fileHandleForReading.read(upToCount: 4096), !chunk.isEmpty {
                try checkCancellation()
                pending.append(chunk)
                while let newline = pending.firstIndex(of: 10) {
                    let line = String(decoding: pending[..<newline], as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
                    pending.removeSubrange(...newline)
                    if line == "BRINEWELL prepare done" {
                        done = true
                    } else if line.hasPrefix("BRINEWELL prepare failed ") {
                        failure = String(line.dropFirst("BRINEWELL prepare failed ".count))
                    } else if line.hasPrefix("BRINEWELL error ") {
                        failure = String(line.dropFirst("BRINEWELL error ".count))
                    } else if line.hasPrefix("BRINEWELL prepare ") {
                        onProgress(String(line.dropFirst("BRINEWELL prepare ".count)))
                    }
                }
                // Kernel diagnostics can contain long non-line data; never let the parser grow unbounded.
                if pending.count > 65_536 { pending.removeFirst(pending.count - 4096) }
            }
            child.waitUntilExit()
            try checkCancellation()
            guard child.terminationReason == .exit, child.terminationStatus == 0, done, failure == nil else {
                if failure == nil, let log = try? String(contentsOf: errorURL, encoding: .utf8),
                   let last = log.split(separator: "\n").last, last.hasPrefix("BRINEWELL error ") {
                    failure = String(last.dropFirst("BRINEWELL error ".count))
                }
                let reason = failure.map { String(localized: " (\($0))") } ?? ""
                throw SteamOSDeploymentError(String(localized: "The SteamOS disk operation did not complete\(reason). Try again."))
            }
        } catch {
            if child.isRunning { child.terminate() }
            child.waitUntilExit()
            throw error
        }
    }

    private func start(_ child: Process) throws {
        try lock.withLock {
            guard !cancelled else { throw CancellationError() }
            try child.run()
            process = child
        }
    }

    private func finish(_ child: Process) {
        lock.withLock {
            if process === child { process = nil }
        }
    }

    private func checkCancellation() throws {
        if lock.withLock({ cancelled }) { throw CancellationError() }
    }
}
