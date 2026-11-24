import Foundation

public struct D3DMetalVersionInfo: Sendable, Equatable, Codable {
    public let shortVersion: String?
    public let bundleVersion: String?
    public let sourceVersion: String?

    public init(shortVersion: String?, bundleVersion: String?, sourceVersion: String?) {
        self.shortVersion = shortVersion
        self.bundleVersion = bundleVersion
        self.sourceVersion = sourceVersion
    }

    public var displayVersion: String { shortVersion ?? bundleVersion ?? "unknown" }
    /// Native context integration also checks the image UUID and bytes at runtime.
    public var isRecommendedContextVersion: Bool {
        displayVersion.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() == "4.0b3"
    }
}

public struct D3DMetalInspection: Sendable, Equatable {
    public let resolvedBundleURL: URL?
    public let binaryURL: URL
    public let version: D3DMetalVersionInfo?
    public let isWholeBundle: Bool

    public init(resolvedBundleURL: URL?, binaryURL: URL, version: D3DMetalVersionInfo?, isWholeBundle: Bool) {
        self.resolvedBundleURL = resolvedBundleURL
        self.binaryURL = binaryURL
        self.version = version
        self.isWholeBundle = isWholeBundle
    }
}

public enum D3DMetalFrameworkError: LocalizedError, Sendable, Equatable {
    case notFound(String), notWholeBundle(String), missingBinary(String), invalidArchitecture(String)
    case escapingSymlink(String), signatureVerificationFailed(String)

    public var errorDescription: String? {
        switch self {
        case .notFound(let path): return "D3DMetal not found: \(path)"
        case .notWholeBundle(let path): return "A complete D3DMetal framework is required: \(path)"
        case .missingBinary(let path): return "D3DMetal framework has no readable binary: \(path)"
        case .invalidArchitecture(let reason): return "D3DMetal requires an x86_64 Mach-O binary: \(reason)"
        case .escapingSymlink(let path): return "Framework symlink escapes the bundle: \(path)"
        case .signatureVerificationFailed(let reason): return "D3DMetal signature verification failed: \(reason)"
        }
    }
}

public typealias D3DMetalCommandRunner = @Sendable (_ executable: URL, _ arguments: [String]) throws -> (exitCode: Int32, stdout: String, stderr: String)

public enum D3DMetalFramework {
    public static let appleSigningRequirement = "=identifier \"com.apple.D3DMetal\" and anchor apple"

    public static func resolve(at url: URL, allowBareBinary: Bool = true) throws -> (bundleURL: URL?, binaryURL: URL, isWholeBundle: Bool) {
        let files = FileManager.default
        var directory: ObjCBool = false
        guard files.fileExists(atPath: url.path, isDirectory: &directory) else {
            throw D3DMetalFrameworkError.notFound(url.path)
        }
        func readableRegular(_ candidate: URL) -> Bool {
            (try? candidate.resolvingSymlinksInPath().resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
                && files.isReadableFile(atPath: candidate.path)
        }
        if directory.boolValue {
            let root = url.resolvingSymlinksInPath().standardizedFileURL.path
            let versions = url.appendingPathComponent("Versions")
            if files.fileExists(atPath: versions.path) {
                let entries = try files.contentsOfDirectory(at: versions, includingPropertiesForKeys: [.isSymbolicLinkKey])
                let versionEntries = try entries.filter {
                    if $0.lastPathComponent != "Current" { return true }
                    return try $0.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink != true
                }
                guard versionEntries.count <= 1 else {
                    throw D3DMetalFrameworkError.signatureVerificationFailed("Ambiguous multi-version framework bundle")
                }
            }
            let current = versions.appendingPathComponent("Current")
            var names = ["D3DMetal", "Versions/A/D3DMetal"]
            if (try? current.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true {
                let target = current.resolvingSymlinksInPath().standardizedFileURL.path
                let versionsPath = versions.resolvingSymlinksInPath().standardizedFileURL.path
                guard target.hasPrefix(versionsPath + "/"), versionsPath.hasPrefix(root + "/") else {
                    throw D3DMetalFrameworkError.escapingSymlink(current.path)
                }
                names.insert("Versions/Current/D3DMetal", at: 0)
            }
            for name in names {
                let candidate = url.appendingPathComponent(name)
                let resolved = candidate.resolvingSymlinksInPath().standardizedFileURL.path
                if resolved.hasPrefix(root + "/"), readableRegular(candidate) {
                    return (url, URL(fileURLWithPath: resolved), true)
                }
            }
            throw D3DMetalFrameworkError.missingBinary(url.path)
        }
        guard readableRegular(url) else { throw D3DMetalFrameworkError.notFound(url.path) }
        guard allowBareBinary else { throw D3DMetalFrameworkError.notWholeBundle(url.path) }
        return (nil, url, false)
    }

    public static func readVersionInfo(from bundleURL: URL) -> D3DMetalVersionInfo? {
        var short: String?, bundle: String?, source: String?
        for path in ["Versions/A/Resources/version.plist", "Resources/version.plist", "Versions/A/Resources/Info.plist", "Resources/Info.plist", "Info.plist"] {
            guard let data = try? Data(contentsOf: bundleURL.appendingPathComponent(path)),
                  let plist = (try? PropertyListSerialization.propertyList(from: data, format: nil)) as? [String: Any] else { continue }
            short = short ?? (plist["CFBundleShortVersionString"] as? String)
            bundle = bundle ?? (plist["CFBundleVersion"] as? String)
            if source == nil, let value = plist["SourceVersion"] {
                if let string = value as? String { source = string }
                else if let number = value as? NSNumber { source = number.stringValue }
            }
        }
        guard short != nil || bundle != nil || source != nil else { return nil }
        return D3DMetalVersionInfo(shortVersion: short, bundleVersion: bundle, sourceVersion: source)
    }

    public static func validateBundleSymlinks(at bundleURL: URL) throws {
        let root = bundleURL.resolvingSymlinksInPath().standardizedFileURL.path
        func walk(_ directory: URL) throws {
            let items = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: [.isSymbolicLinkKey, .isDirectoryKey])
            for item in items {
                let values = try item.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey])
                if values.isSymbolicLink == true {
                    let target = item.resolvingSymlinksInPath().standardizedFileURL.path
                    guard target == root || target.hasPrefix(root + "/") else { throw D3DMetalFrameworkError.escapingSymlink(item.path) }
                } else if values.isDirectory == true {
                    try walk(item)
                }
            }
        }
        try walk(bundleURL)
    }

    public static func verifyAppleCodeSignature(bundleURL: URL, runner: D3DMetalCommandRunner = defaultCommandRunner) throws {
        let binary = try resolve(at: bundleURL, allowBareBinary: false).binaryURL
        let executable = URL(fileURLWithPath: "/usr/bin/codesign")
        for arguments in [["--verify", "--deep", "--strict", "--architecture", "x86_64", bundleURL.path],
                          ["--verify", "--strict", "-R", appleSigningRequirement, bundleURL.path],
                          ["--verify", "--strict", "--architecture", "x86_64", "-R", appleSigningRequirement, binary.path]] {
            let result: (exitCode: Int32, stdout: String, stderr: String)
            do { result = try runner(executable, arguments) }
            catch { throw D3DMetalFrameworkError.signatureVerificationFailed(error.localizedDescription) }
            guard result.exitCode == 0 else {
                let message = result.stderr.unicodeScalars.map { CharacterSet.controlCharacters.contains($0) ? " " : String($0) }.joined()
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                throw D3DMetalFrameworkError.signatureVerificationFailed(String(message.prefix(1000)))
            }
        }
    }

    public static func inspect(at url: URL, allowBareBinary: Bool = true, verifyAppleSignature: Bool = false, runner: D3DMetalCommandRunner = defaultCommandRunner) throws -> D3DMetalInspection {
        let resolved = try resolve(at: url, allowBareBinary: allowBareBinary && !verifyAppleSignature)
        if verifyAppleSignature, let bundle = resolved.bundleURL { try validateBundleSymlinks(at: bundle) }
        do {
            guard try MachOArchitecture.architectures(at: resolved.binaryURL).contains("x86_64") else {
                throw D3DMetalFrameworkError.invalidArchitecture(resolved.binaryURL.path)
            }
        } catch let error as D3DMetalFrameworkError { throw error }
        catch { throw D3DMetalFrameworkError.invalidArchitecture(error.localizedDescription) }
        if verifyAppleSignature, let bundle = resolved.bundleURL { try verifyAppleCodeSignature(bundleURL: bundle, runner: runner) }
        return D3DMetalInspection(resolvedBundleURL: resolved.bundleURL, binaryURL: resolved.binaryURL,
                                  version: resolved.bundleURL.flatMap { readVersionInfo(from: $0) }, isWholeBundle: resolved.isWholeBundle)
    }

    public static func isRosettaAvailable(runner: D3DMetalCommandRunner = defaultCommandRunner) -> Bool {
        (try? runner(URL(fileURLWithPath: "/usr/bin/arch"), ["-x86_64", "/usr/bin/true"]).exitCode) == 0
    }

    public static func defaultCommandRunner(_ executable: URL, _ arguments: [String]) throws -> (exitCode: Int32, stdout: String, stderr: String) {
        // Regular temporary files avoid filling either pipe while the child is running.
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        let output = directory.appendingPathComponent("stdout"), errors = directory.appendingPathComponent("stderr")
        FileManager.default.createFile(atPath: output.path, contents: nil)
        FileManager.default.createFile(atPath: errors.path, contents: nil)
        let out = try FileHandle(forWritingTo: output), err = try FileHandle(forWritingTo: errors)
        defer { try? out.close(); try? err.close() }
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.standardOutput = out
        process.standardError = err
        try process.run()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: try Data(contentsOf: output), as: UTF8.self),
                String(decoding: try Data(contentsOf: errors), as: UTF8.self))
    }
}
