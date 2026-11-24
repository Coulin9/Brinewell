import Darwin
import Foundation

public enum D3DMetalInstallStage: String, Sendable, Equatable {
    case mounting, extracting, verifying, installing
}

public struct D3DMetalInstallResult: Sendable, Equatable {
    public let installedFrameworkURL: URL
    public let inspection: D3DMetalInspection
    public let replacedExisting: Bool
}

public enum D3DMetalInstallerError: LocalizedError, Sendable, Equatable {
    case unsupportedSource(String), mountFailed(String), frameworkNotFound, ambiguousCandidate(String)
    case invalidBundle(String), bundleTooLarge(Int64), copyFailed(String), verificationFailed(String)
    case commitFailed(String), cancelled

    public var errorDescription: String? {
        switch self {
        case .unsupportedSource(let s): return "Unsupported source: \(s)"
        case .mountFailed(let s): return "Mount failed: \(s)"
        case .frameworkNotFound: return "D3DMetal.framework not found."
        case .ambiguousCandidate(let s): return "Multiple candidates: \(s)"
        case .invalidBundle(let s): return "Invalid framework: \(s)"
        case .bundleTooLarge(let n): return "Framework exceeds size limit: \(n) bytes"
        case .copyFailed(let s): return "Copy failed: \(s)"
        case .verificationFailed(let s): return "Verification failed: \(s)"
        case .commitFailed(let s): return "Installation commit failed: \(s)"
        case .cancelled: return "Installation cancelled."
        }
    }
}

public typealias D3DMetalProcessRunner = @Sendable (_ executable: URL, _ arguments: [String], _ stdinData: Data?) throws -> (exitCode: Int32, stdout: Data, stderr: Data)

public struct D3DMetalInstaller: Sendable {
    public static let maximumBundleBytes: Int64 = 2 * 1024 * 1024 * 1024
    public static let maximumMountDepth = 2
    private let processRunner: D3DMetalProcessRunner
    private let signatureVerifier: @Sendable (URL) throws -> Void
    private let requireAppleSignature: Bool

    public init(processRunner: @escaping D3DMetalProcessRunner = D3DMetalInstaller.defaultProcessRunner,
                signatureVerifier: (@Sendable (URL) throws -> Void)? = nil, requireAppleSignature: Bool = true) {
        self.processRunner = processRunner
        self.signatureVerifier = signatureVerifier ?? { try D3DMetalFramework.verifyAppleCodeSignature(bundleURL: $0) }
        self.requireAppleSignature = requireAppleSignature
    }

    public func install(from sourceURL: URL, configurationStore: ConfigurationStore, deleteSourceOnSuccess: Bool = false,
                        onStage: (@Sendable (D3DMetalInstallStage) -> Void)? = nil) async throws -> D3DMetalInstallResult {
        let result = try await installFramework(from: sourceURL, into: ConfigurationStore.managedFrameworksDirectory, onStage: onStage) { url in
            try configurationStore.update { $0.d3dMetalFrameworkPath = url.path }
        }
        if deleteSourceOnSuccess, sourceURL.standardizedFileURL != result.installedFrameworkURL.standardizedFileURL {
            try? FileManager.default.removeItem(at: sourceURL)
        }
        return result
    }

    public func installFramework(from sourceURL: URL, into frameworksDirectory: URL,
                                 onStage: (@Sendable (D3DMetalInstallStage) -> Void)? = nil,
                                 commitConfiguration: @Sendable (URL) throws -> Void) async throws -> D3DMetalInstallResult {
        let cancellation = InstallerCancellation()
        return try await withTaskCancellationHandler {
            try InstallerProcessContext.$cancellation.withValue(cancellation) {
        try checkCancellation()
        let fm = FileManager.default
        var isDirectory: ObjCBool = false
        guard fm.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory),
              (isDirectory.boolValue && sourceURL.lastPathComponent == "D3DMetal.framework") ||
                (!isDirectory.boolValue && sourceURL.pathExtension.lowercased() == "dmg") else {
            throw D3DMetalInstallerError.unsupportedSource(sourceURL.path)
        }
        let job = fm.temporaryDirectory.appendingPathComponent("D3DMetal-\(UUID().uuidString)", isDirectory: true)
        try fm.createDirectory(at: job, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        var devices: [String] = []
        var images: [URL] = []
        var preAttachDevices: [String: Set<String>] = [:]
        defer {
            let tool = URL(fileURLWithPath: "/usr/bin/hdiutil")
            func reportedMounts() -> [(String, String)]? {
                guard let result = try? processRunner(tool, ["info", "-plist"], nil), result.exitCode == 0,
                      let plist = try? PropertyListSerialization.propertyList(from: result.stdout, format: nil) as? [String: Any],
                      let entries = plist["images"] as? [[String: Any]] else { return nil }
                let base = job.resolvingSymlinksInPath().standardizedFileURL.path
                let imagePaths = Set(images.map { $0.resolvingSymlinksInPath().standardizedFileURL.path })
                var found: [(String, String)] = []
                for entry in entries {
                    let imagePath = (entry["image-path"] as? String).map { URL(fileURLWithPath: $0).resolvingSymlinksInPath().standardizedFileURL.path }
                    let entities = entry["system-entities"] as? [[String: Any]] ?? []
                    let privateMounts = entities.compactMap { entity -> String? in
                        guard let mount = entity["mount-point"] as? String else { return nil }
                        let path = URL(fileURLWithPath: mount).resolvingSymlinksInPath().standardizedFileURL.path
                        return path.hasPrefix(base + "/mount") ? path : nil
                    }
                    let ownedImage = imagePath.map { imagePaths.contains($0) } == true
                    let jobImage = imagePath.map { $0.hasPrefix(base + "/") } == true
                    if ownedImage && (!privateMounts.isEmpty || (jobImage && entities.allSatisfy { $0["mount-point"] == nil })) {
                        for entity in entities {
                            if let device = entity["dev-entry"] as? String {
                                found.append((device, privateMounts.first ?? ""))
                            }
                        }
                    } else {
                        if let imagePath, let previous = preAttachDevices[imagePath] {
                            for entity in entities {
                                if let device = entity["dev-entry"] as? String, !previous.contains(device) {
                                    found.append((device, ""))
                                }
                            }
                        }
                        for path in privateMounts { found.append(("", path)) }
                    }
                }
                return found
            }
            if !isDirectory.boolValue {
                if let mounts = reportedMounts() {
                    for (device, _) in mounts where !device.isEmpty && !devices.contains(device) { devices.append(device) }
                }
                for device in devices.reversed() {
                    let result = try? processRunner(tool, ["detach", device], nil)
                    if result?.exitCode != 0 { _ = try? processRunner(tool, ["detach", "-force", device], nil) }
                }
                // Fail closed if mount state cannot be established or any private mount remains.
                if let remaining = reportedMounts(), remaining.isEmpty { try? fm.removeItem(at: job) }
            } else { try? fm.removeItem(at: job) }
        }
        var source = sourceURL
        if !isDirectory.boolValue {
            for depth in 0..<Self.maximumMountDepth {
                try checkCancellation(); onStage?(.mounting)
                let root = job.appendingPathComponent("mount\(depth)", isDirectory: true)
                try fm.createDirectory(at: root, withIntermediateDirectories: false)
                let sourcePath = source.resolvingSymlinksInPath().standardizedFileURL.path
                let jobPath = job.resolvingSymlinksInPath().standardizedFileURL.path
                if !sourcePath.hasPrefix(jobPath + "/"), preAttachDevices[sourcePath] == nil,
                   let result = try? processRunner(URL(fileURLWithPath: "/usr/bin/hdiutil"), ["info", "-plist"], nil),
                   result.exitCode == 0,
                   let plist = try? PropertyListSerialization.propertyList(from: result.stdout, format: nil) as? [String: Any],
                   let entries = plist["images"] as? [[String: Any]] {
                    preAttachDevices[sourcePath] = Set(entries.flatMap { $0["system-entities"] as? [[String: Any]] ?? [] }
                        .compactMap { $0["dev-entry"] as? String })
                }
                let mount = try attach(source, root: root, job: job, devices: &devices, images: &images)
                try checkCancellation(); onStage?(.extracting)
                let candidates = try search(root: mount, depth: 5, matching: { $0.lastPathComponent == "D3DMetal.framework" && $1 })
                let preferred = [mount.appendingPathComponent("D3DMetal.framework"), mount.appendingPathComponent("redist/lib/external/D3DMetal.framework")]
                let known = preferred.filter { path in candidates.contains { $0.standardizedFileURL.path == path.standardizedFileURL.path } }
                if candidates.count > 1 { throw D3DMetalInstallerError.ambiguousCandidate("frameworks") }
                if let candidate = known.first ?? candidates.first { source = candidate; break }
                guard depth + 1 < Self.maximumMountDepth else { throw D3DMetalInstallerError.frameworkNotFound }
                let images = try search(root: mount, depth: 2, matching: { $0.pathExtension.lowercased() == "dmg" && !$1 })
                let preferredImages = images.filter { $0.lastPathComponent.lowercased().contains("evaluation environment for windows games") }
                let choices = images
                _ = preferredImages
                if choices.count > 1 { throw D3DMetalInstallerError.ambiguousCandidate("disk images") }
                guard let image = choices.first else { throw D3DMetalInstallerError.frameworkNotFound }
                source = image
            }
        } else { onStage?(.extracting) }
        try checkCancellation()
        do { try validate(source) }
        catch let error as D3DMetalInstallerError { throw error }
        catch { throw D3DMetalInstallerError.invalidBundle(error.localizedDescription) }
        try fm.createDirectory(at: frameworksDirectory, withIntermediateDirectories: true)
        let directory = frameworksDirectory.resolvingSymlinksInPath().standardizedFileURL
        let target = directory.appendingPathComponent("D3DMetal.framework")
        let staged = directory.appendingPathComponent(".D3DMetal.staging.\(UUID().uuidString).framework")
        var keepStaged = false
        defer { if !keepStaged { try? fm.removeItem(at: staged) } }
        do { try fm.copyItem(at: source, to: staged); try validate(staged) }
        catch let error as D3DMetalInstallerError { throw error }
        catch { throw D3DMetalInstallerError.copyFailed(error.localizedDescription) }
        try checkCancellation(); onStage?(.verifying)
        let inspection: D3DMetalInspection
        do {
            inspection = try D3DMetalFramework.inspect(at: staged, allowBareBinary: false, verifyAppleSignature: false)
            if requireAppleSignature { try signatureVerifier(staged) }
        } catch { throw D3DMetalInstallerError.verificationFailed(error.localizedDescription) }
        try checkCancellation(); onStage?(.installing)
        let replaced = fm.fileExists(atPath: target.path)
        if replaced {
            guard renameatx_np(AT_FDCWD, staged.path, AT_FDCWD, target.path, UInt32(RENAME_SWAP)) == 0 else {
                throw D3DMetalInstallerError.commitFailed(String(cString: strerror(errno)))
            }
        } else {
            do { try fm.moveItem(at: staged, to: target) }
            catch { throw D3DMetalInstallerError.commitFailed(error.localizedDescription) }
        }
        do { try commitConfiguration(target) }
        catch {
            if replaced {
                if renameatx_np(AT_FDCWD, staged.path, AT_FDCWD, target.path, UInt32(RENAME_SWAP)) != 0 {
                    keepStaged = true
                    throw D3DMetalInstallerError.commitFailed("Rollback failed; previous bundle retained at \(staged.path): \(String(cString: strerror(errno)))")
                }
            } else { try? fm.removeItem(at: target) }
            throw D3DMetalInstallerError.commitFailed(error.localizedDescription)
        }
        let installedInspection = D3DMetalInspection(resolvedBundleURL: target, binaryURL: target.appendingPathComponent(inspection.binaryURL.path.replacingOccurrences(of: staged.path + "/", with: "")), version: inspection.version, isWholeBundle: true)
        return D3DMetalInstallResult(installedFrameworkURL: target, inspection: installedInspection, replacedExisting: replaced)
            }
        } onCancel: { cancellation.cancel() }
    }

    private func checkCancellation() throws {
        if Task.isCancelled { throw D3DMetalInstallerError.cancelled }
    }

    private func attach(_ image: URL, root: URL, job: URL, devices: inout [String], images: inout [URL]) throws -> URL {
        let tool = URL(fileURLWithPath: "/usr/bin/hdiutil")
        func runAttach(_ path: URL) throws -> (exitCode: Int32, stdout: Data, stderr: Data) {
            images.append(path)
            return try processRunner(tool, ["attach", "-plist", "-nobrowse", "-readonly", "-noidme", "-mountrandom", root.path, path.path], Data("qn\n".utf8))
        }
        var result = try runAttach(image)
        if result.exitCode != 0 {
            try checkCancellation()
            let base = job.appendingPathComponent(UUID().uuidString)
            let converted = try processRunner(tool, ["convert", "-quiet", image.path, "-format", "UDTO", "-o", base.path], nil)
            guard converted.exitCode == 0 else { throw D3DMetalInstallerError.mountFailed(String(decoding: converted.stderr, as: UTF8.self)) }
            let cdr = URL(fileURLWithPath: base.path + ".cdr")
            images.append(cdr)
            result = try runAttach(FileManager.default.fileExists(atPath: cdr.path) ? cdr : base)
        }
        guard result.exitCode == 0 else { throw D3DMetalInstallerError.mountFailed(String(decoding: result.stderr, as: UTF8.self)) }
        guard let plist = try? PropertyListSerialization.propertyList(from: result.stdout, format: nil) as? [String: Any],
              let entities = plist["system-entities"] as? [[String: Any]] else { throw D3DMetalInstallerError.mountFailed("Invalid attach output") }
        var mounts: [URL] = []
        for entity in entities {
            if let device = entity["dev-entry"] as? String, !devices.contains(device) { devices.append(device) }
        }
        for entity in entities {
            if let path = entity["mount-point"] as? String {
                let mount = URL(fileURLWithPath: path).resolvingSymlinksInPath().standardizedFileURL
                let base = root.resolvingSymlinksInPath().standardizedFileURL.path
                guard mount.path.hasPrefix(base + "/") else { throw D3DMetalInstallerError.mountFailed("Mount outside private directory") }
                mounts.append(mount)
            }
        }
        guard let mount = mounts.first else { throw D3DMetalInstallerError.mountFailed("No mounted volume") }
        return mount
    }

    private func search(root: URL, depth: Int, matching: (URL, Bool) -> Bool) throws -> [URL] {
        var found: [URL] = []
        var rootStat = stat()
        guard lstat(root.path, &rootStat) == 0 else { throw D3DMetalInstallerError.mountFailed("Cannot inspect mount") }
        func walk(_ dir: URL, _ level: Int) throws {
            guard level <= depth else { return }
            for item in try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) {
                var info = stat()
                guard lstat(item.path, &info) == 0, info.st_dev == rootStat.st_dev else { continue }
                let type = info.st_mode & S_IFMT
                if type == S_IFLNK { continue }
                let directory = type == S_IFDIR
                if matching(item, directory) { found.append(item); if directory { continue } }
                if directory && level < depth { try walk(item, level + 1) }
            }
        }
        try walk(root, 1)
        return found
    }

    private func validate(_ bundle: URL) throws {
        var rootInfo = stat()
        guard lstat(bundle.path, &rootInfo) == 0, rootInfo.st_mode & S_IFMT == S_IFDIR else {
            throw D3DMetalInstallerError.invalidBundle("Framework root must be a directory")
        }
        try D3DMetalFramework.validateBundleSymlinks(at: bundle)
        var total: Int64 = 0
        func walk(_ url: URL) throws {
            var info = stat()
            guard lstat(url.path, &info) == 0 else { throw D3DMetalInstallerError.invalidBundle(url.path) }
            switch info.st_mode & S_IFMT {
            case S_IFDIR:
                for item in try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil) { try walk(item) }
            case S_IFREG:
                guard info.st_size >= 0, info.st_size <= Self.maximumBundleBytes else { throw D3DMetalInstallerError.bundleTooLarge(info.st_size) }
                let (sum, overflow) = total.addingReportingOverflow(info.st_size)
                guard !overflow, sum <= Self.maximumBundleBytes else { throw D3DMetalInstallerError.bundleTooLarge(overflow ? Int64.max : sum) }
                total = sum
            case S_IFLNK:
                let destination = try FileManager.default.destinationOfSymbolicLink(atPath: url.path)
                guard !destination.hasPrefix("/") else { throw D3DMetalInstallerError.invalidBundle("Absolute symlink: \(url.path)") }
            default: throw D3DMetalInstallerError.invalidBundle("Special file: \(url.path)")
            }
        }
        try walk(bundle)
    }

    public static func defaultProcessRunner(_ executable: URL, _ arguments: [String], _ stdinData: Data?) throws -> (exitCode: Int32, stdout: Data, stderr: Data) {
        guard executable.isFileURL, executable.path.hasPrefix("/") else { throw D3DMetalInstallerError.mountFailed("Executable must be absolute") }
        let process = Process(), output = Pipe(), errors = Pipe(), input = Pipe()
        process.executableURL = executable; process.arguments = arguments
        process.standardOutput = output; process.standardError = errors; process.standardInput = input
        let cancellable = arguments.first == "attach" || arguments.first == "convert"
        if cancellable, let cancellation = InstallerProcessContext.cancellation {
            try cancellation.start(process)
        } else { try process.run() }
        defer { if cancellable { InstallerProcessContext.cancellation?.finish(process) } }
        let group = DispatchGroup()
        let result = ProcessOutput()
        group.enter(); DispatchQueue.global().async { result.setOut(output.fileHandleForReading.readDataToEndOfFile()); group.leave() }
        group.enter(); DispatchQueue.global().async { result.setErr(errors.fileHandleForReading.readDataToEndOfFile()); group.leave() }
        if let stdinData { try? input.fileHandleForWriting.write(contentsOf: stdinData) }
        try? input.fileHandleForWriting.close()
        process.waitUntilExit(); group.wait()
        if cancellable && Task<Never, Never>.isCancelled { throw D3DMetalInstallerError.cancelled }
        return (process.terminationStatus, result.out, result.err)
    }
}

private final class ProcessOutput: @unchecked Sendable {
    private let lock = NSLock()
    private var output = Data(), errors = Data()
    func setOut(_ data: Data) { lock.withLock { output = data } }
    func setErr(_ data: Data) { lock.withLock { errors = data } }
    var out: Data { lock.withLock { output } }
    var err: Data { lock.withLock { errors } }
}

private enum InstallerProcessContext {
    @TaskLocal static var cancellation: InstallerCancellation?
}

private final class InstallerCancellation: @unchecked Sendable {
    private let lock = NSLock()
    private var process: Process?
    private var cancelled = false
    func start(_ child: Process) throws {
        try lock.withLock {
            guard !cancelled else { throw D3DMetalInstallerError.cancelled }
            try child.run()
            process = child
        }
    }
    func finish(_ child: Process) { lock.withLock { if process === child { process = nil } } }
    func cancel() { lock.withLock { cancelled = true; if let process, process.isRunning { process.terminate() } } }
}
