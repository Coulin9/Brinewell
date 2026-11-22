import BrinewellCore
import Darwin
import Foundation

struct RuntimeError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

// When the VM runs inside the launcher process (single-process mode), the
// launcher installs this handler to receive state lines alongside stdout logging.
// The standalone brinewell-vm executable leaves it nil and reports only to stdout.
// report() is called from both the main thread and the VMM thread, so the
// handler is @Sendable and hops to the main actor itself.
public nonisolated(unsafe) var reportHandler: (@Sendable (String) -> Void)?

public nonisolated func report(_ line: String) {
    let sanitized = line.replacingOccurrences(of: "\n", with: " ")
    reportHandler?(sanitized)
    let bytes = Array((sanitized + "\n").utf8)
    bytes.withUnsafeBytes { buffer in
        var offset = 0
        while offset < buffer.count {
            let n = Darwin.write(STDOUT_FILENO, buffer.baseAddress!.advanced(by: offset), buffer.count - offset)
            if n < 0 && errno == EINTR { continue }
            guard n > 0 else { return }
            offset += n
        }
    }
}

// Confirms a just-locked descriptor still refers to the file at `path`; a disk
// swapped by rename between open and lock would otherwise go unnoticed.
func diskFDMatchesPath(_ fd: Int32, path: String) -> Bool {
    var opened = stat()
    var current = stat()
    return fstat(fd, &opened) == 0 && stat(path, &current) == 0
        && opened.st_dev == current.st_dev && opened.st_ino == current.st_ino
}

struct NeptuneSupport {
    enum Backend {
        case d3dmetal(framework: URL, version: String?)
        case mullion
    }

    let backend: Backend
    let adapter: URL
    let server: URL
    let apertureMiB: Int
    let multiRing: Bool
}

public struct RuntimeOptions {
    let resources: URL
    let disk: String
    let cpus: UInt8
    let memoryMiB: UInt32
    let display: VMDisplayConfiguration
    let hostDisplayID: UInt32?
    let runDirectory: URL
    let sshPort: Int
    public let diskFD: Int32
    let graphicsBackend: GraphicsBackend
    let gameHUDLevel: GameHUDLevel
    let gameHUDPosition: GameHUDPosition
    // Support for the backend the VM was asked to start with (or an available
    // Neptune fallback for Venus so standalone stays internally consistent).
    let neptune: NeptuneSupport?
    // Both Neptune implementations are provisioned once, independently from the
    // initial preference, so live switching only needs the per-launch tag.
    let neptuneD3DMetal: NeptuneSupport?
    let neptuneMullion: NeptuneSupport?
    let neptuneReasons: [GraphicsBackend: String]
    let sharedFolder: VMSharedFolder?
    let network: VMNetworkConfiguration
    // Target size for the 'home' ext4 filesystem in KiB; the initramfs shrinks
    // it on the next boot so the host can shrink the GPT after the VM exits.
    let shrinkHomeKiB: UInt64?

    var contents: URL { resources.deletingLastPathComponent().deletingLastPathComponent() }
    var renderer: URL { contents.appendingPathComponent("Helpers/virgl_render_server") }
    var vulkanDriver: URL { contents.appendingPathComponent("Frameworks/libvulkan_kosmickrisp.dylib") }

    public init(arguments: [String]) throws {
        let accepted: Set<String> = [
            "--resources", "--disk", "--cpus", "--memory-gib", "--run-directory", "--ssh-port",
            "--graphics-backend", "--d3dmetal-framework", "--neptune-aperture-mib", "--neptune-multi-ring",
            "--game-hud", "--game-hud-level", "--game-hud-position",
            "--display-width", "--display-height", "--display-refresh-rate",
            "--host-display-id", "--shared-folder", "--shared-folder-read-only", "--network-config",
            "--shrink-home-kib",
        ]
        var values: [String: String] = [:]
        var index = 0
        while index < arguments.count {
            let key = arguments[index]
            guard accepted.contains(key), values[key] == nil, index + 1 < arguments.count,
                  !arguments[index + 1].hasPrefix("--") else {
                throw RuntimeError("invalid or duplicate option: \(key)")
            }
            values[key] = arguments[index + 1]
            index += 2
        }
        guard let resourcePath = values["--resources"], resourcePath.hasPrefix("/"),
              let diskPath = values["--disk"], diskPath.hasPrefix("/"),
              let runPath = values["--run-directory"], runPath.hasPrefix("/"),
              let cpuText = values["--cpus"], let cpu = Int(cpuText), let cpuByte = UInt8(exactly: cpu), cpuByte > 0,
              let memoryText = values["--memory-gib"], let memory = Int(memoryText),
              let port = Int(values["--ssh-port"] ?? "2222"), (1...65535).contains(port) else {
            throw RuntimeError("missing or invalid runtime options")
        }
        let defaultDisplay = VMDisplayConfiguration()
        guard let width = Int(values["--display-width"] ?? "\(defaultDisplay.width)"),
              let height = Int(values["--display-height"] ?? "\(defaultDisplay.height)"),
              let refreshRate = Int(values["--display-refresh-rate"] ?? "\(defaultDisplay.refreshRate)") else {
            throw RuntimeError("invalid display options: expected integer width, height and refresh rate")
        }
        let display = VMDisplayConfiguration(width: width, height: height, refreshRate: refreshRate)
        let config = VMConfiguration(diskImagePath: diskPath, cpuCount: cpu, memoryGiB: memory, display: display)
        try config.validate(for: HostCapabilities.detect())
        if let path = values["--network-config"] {
            guard path.hasPrefix("/"), !path.utf8.contains(0) else {
                throw RuntimeError("--network-config requires an absolute file path")
            }
            do {
                network = try JSONDecoder().decode(VMNetworkConfiguration.self, from: Data(contentsOf: URL(fileURLWithPath: path)))
            } catch {
                throw RuntimeError("cannot read network configuration file")
            }
        } else {
            network = VMNetworkConfiguration()
        }
        try network.validate()
        let shareMode = values["--shared-folder-read-only"]
        if let path = values["--shared-folder"] {
            guard shareMode == nil || shareMode == "0" || shareMode == "1" else {
                throw RuntimeError("--shared-folder-read-only expects 0 or 1")
            }
            let folder = VMSharedFolder(path: path, readOnly: shareMode != "0")
            sharedFolder = VMSharedFolder(path: try folder.resolvedPath(), readOnly: folder.readOnly)
        } else {
            guard shareMode == nil else { throw RuntimeError("--shared-folder-read-only requires --shared-folder") }
            sharedFolder = nil
        }
        if let text = values["--host-display-id"] {
            guard let id = UInt32(text), id > 0 else { throw RuntimeError("invalid host display id: \(text)") }
            hostDisplayID = id
        } else { hostDisplayID = nil }
        if let text = values["--shrink-home-kib"] {
            guard let kib = UInt64(text), kib > 0, kib.isMultiple(of: 4) else {
                throw RuntimeError("--shrink-home-kib expects a positive multiple of 4: \(text)")
            }
            shrinkHomeKiB = kib
        } else { shrinkHomeKiB = nil }
        guard let mib = UInt32(exactly: memory.multipliedReportingOverflow(by: 1024).partialValue),
              !memory.multipliedReportingOverflow(by: 1024).overflow else {
            throw RuntimeError("memory size is too large")
        }

        let backend: GraphicsBackend
        if let value = values["--graphics-backend"] {
            guard let parsed = GraphicsBackend(rawValue: value) else {
                throw RuntimeError("invalid graphics backend: \(value)")
            }
            backend = parsed
        } else {
            backend = .neptune
        }

        let aperture: Int
        if let value = values["--neptune-aperture-mib"] {
            guard let parsed = Int(value), (8192...65536).contains(parsed) else {
                throw RuntimeError("invalid Neptune aperture: \(value)")
            }
            aperture = parsed
        } else {
            aperture = 32768
        }

        let multiRing: Bool
        if let value = values["--neptune-multi-ring"] {
            guard value == "0" || value == "1" else {
                throw RuntimeError("invalid Neptune multi-ring value: \(value)")
            }
            multiRing = value == "1"
        } else {
            multiRing = false
        }

        if let text = values["--game-hud-level"] {
            guard values["--game-hud"] == nil, let number = Int(text),
                  let level = GameHUDLevel(rawValue: number), text == String(number) else {
                throw RuntimeError("--game-hud-level expects 0...4; do not combine with --game-hud")
            }
            gameHUDLevel = level
        } else {
            let legacy = values["--game-hud"] ?? "0"
            guard legacy == "0" || legacy == "1" else { throw RuntimeError("--game-hud expects 0 or 1") }
            gameHUDLevel = legacy == "1" ? .compact : .off
        }
        guard let position = GameHUDPosition(rawValue: values["--game-hud-position"] ?? "top-right") else {
            throw RuntimeError("--game-hud-position expects top-left, top-right, bottom-left or bottom-right")
        }
        gameHUDPosition = position

        let resourceURL = URL(fileURLWithPath: resourcePath, isDirectory: true)
        for name in ["Image", "initramfs.cpio.gz", "brinewell-layer.img", "gvproxy"] {
            let path = resourceURL.appendingPathComponent(name).path
            guard FileManager.default.isReadableFile(atPath: path),
                  (try? URL(fileURLWithPath: path).resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
                throw RuntimeError("required resource is not a readable regular file: \(path)")
            }
        }
        guard FileManager.default.isExecutableFile(atPath: resourceURL.appendingPathComponent("gvproxy").path) else {
            throw RuntimeError("gvproxy is not executable")
        }
        let contents = resourceURL.deletingLastPathComponent().deletingLastPathComponent()
        let renderer = contents.appendingPathComponent("Helpers/virgl_render_server")
        let vulkanDriver = contents.appendingPathComponent("Frameworks/libvulkan_kosmickrisp.dylib")
        guard FileManager.default.isExecutableFile(atPath: renderer.path),
              (try? renderer.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
            throw RuntimeError("renderer is not an executable regular file: \(renderer.path)")
        }
        guard FileManager.default.isReadableFile(atPath: vulkanDriver.path),
              (try? vulkanDriver.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
            throw RuntimeError("Vulkan driver is not a readable regular file: \(vulkanDriver.path)")
        }
        var runStat = stat()
        guard lstat(runPath, &runStat) == 0, (runStat.st_mode & S_IFMT) == S_IFDIR,
              runStat.st_uid == getuid(), (runStat.st_mode & 0o777) == 0o700 else {
            throw RuntimeError("run directory must be an existing owner-only directory")
        }
        let fd = open(diskPath, O_RDWR | O_CLOEXEC)
        guard fd >= 0 else { throw RuntimeError("cannot open disk: \(String(cString: strerror(errno)))") }
        var diskStat = stat()
        guard fstat(fd, &diskStat) == 0, (diskStat.st_mode & S_IFMT) == S_IFREG else {
            close(fd)
            throw RuntimeError("disk is not a regular file")
        }
        guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
            let error = errno
            close(fd)
            if error == EWOULDBLOCK || error == EAGAIN {
                throw RuntimeError("disk is already in use by another VM")
            }
            throw RuntimeError("cannot lock disk: \(String(cString: strerror(error)))")
        }
        guard diskFDMatchesPath(fd, path: diskPath) else {
            close(fd)
            throw RuntimeError("disk was replaced while it was being opened; try again")
        }
        resources = resourceURL
        disk = diskPath
        cpus = cpuByte
        memoryMiB = mib
        self.display = display
        runDirectory = URL(fileURLWithPath: runPath, isDirectory: true)
        sshPort = port
        diskFD = fd
        graphicsBackend = backend

        let d3dmetal = Self.resolveNeptune(
            backend: .neptune,
            contents: contents,
            renderer: renderer,
            frameworkPath: values["--d3dmetal-framework"],
            aperture: aperture,
            multiRing: multiRing
        )
        let mullion = Self.resolveNeptune(
            backend: .mullion,
            contents: contents,
            renderer: renderer,
            frameworkPath: values["--d3dmetal-framework"],
            aperture: aperture,
            multiRing: multiRing
        )
        neptuneD3DMetal = d3dmetal.support
        neptuneMullion = mullion.support
        var reasons: [GraphicsBackend: String] = [:]
        if let reason = d3dmetal.reason { reasons[.neptune] = reason }
        if let reason = mullion.reason { reasons[.mullion] = reason }
        neptuneReasons = reasons
        switch backend {
        case .neptune:
            neptune = d3dmetal.support
        case .mullion:
            neptune = mullion.support
        case .venus:
            // Venus never depends on Rosetta or the D3DMetal framework, but a
            // validated Neptune support still drives capability advertisement
            // and live switching.
            neptune = d3dmetal.support ?? mullion.support
        }
        if backend != .venus, neptune == nil {
            close(fd)
            throw RuntimeError("Neptune is not available: \(reasons[backend] ?? "unknown reason")")
        }
    }

    func neptuneSupport(for backend: GraphicsBackend) -> NeptuneSupport? {
        switch backend {
        case .neptune: return neptuneD3DMetal
        case .mullion: return neptuneMullion
        case .venus: return nil
        }
    }

    var availableBackends: Set<GraphicsBackend> {
        var set: Set<GraphicsBackend> = [.venus]
        if neptuneD3DMetal != nil { set.insert(.neptune) }
        if neptuneMullion != nil { set.insert(.mullion) }
        return set
    }

    private static func resolveNeptune(
        backend: GraphicsBackend,
        contents: URL,
        renderer: URL,
        frameworkPath: String?,
        aperture: Int,
        multiRing: Bool
    ) -> (support: NeptuneSupport?, reason: String?) {
        guard backend != .venus else { return (nil, nil) }
        if backend == .mullion {
            let adapter = contents.appendingPathComponent("Frameworks/libmullion-native.dylib")
            guard FileManager.default.isReadableFile(atPath: adapter.path),
                  (try? adapter.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
                return (nil, "libmullion-native.dylib is missing")
            }
            do {
                guard try MachOArchitecture.architectures(at: adapter).contains("arm64") else {
                    return (nil, "libmullion-native.dylib is not arm64")
                }
                guard try MachOArchitecture.architectures(at: renderer).contains("arm64") else {
                    return (nil, "virgl_render_server has no arm64 slice for Mullion")
                }
            } catch {
                return (nil, "cannot inspect Mullion backend: \(error.localizedDescription)")
            }
            return (NeptuneSupport(backend: .mullion, adapter: adapter, server: renderer,
                                   apertureMiB: aperture, multiRing: multiRing), nil)
        }
        guard let frameworkPath, frameworkPath.hasPrefix("/") else {
            return (nil, "D3DMetal framework path is not configured")
        }
        let frameworkURL = URL(fileURLWithPath: frameworkPath)
        guard let frameworkBinary = resolveFrameworkBinary(frameworkURL) else {
            return (nil, "D3DMetal framework has no D3DMetal binary: \(frameworkPath)")
        }
        do {
            let frameworkArchitectures = try MachOArchitecture.architectures(at: frameworkBinary)
            guard frameworkArchitectures.contains("x86_64") else {
                return (nil, "D3DMetal framework is not x86_64: \(frameworkBinary.path)")
            }
        } catch {
            return (nil, "cannot read D3DMetal framework: \(error.localizedDescription)")
        }
        let adapter = contents.appendingPathComponent("Frameworks/libd3dmetal-native.dylib")
        guard FileManager.default.isReadableFile(atPath: adapter.path) else {
            return (nil, "libd3dmetal-native.dylib is missing")
        }
        do {
            let adapterArchitectures = try MachOArchitecture.architectures(at: adapter)
            guard adapterArchitectures.contains("x86_64") else {
                return (nil, "libd3dmetal-native.dylib is not x86_64")
            }
        } catch {
            return (nil, "cannot read libd3dmetal-native.dylib: \(error.localizedDescription)")
        }
        do {
            let serverArchitectures = try MachOArchitecture.architectures(at: renderer)
            guard serverArchitectures.contains("arm64"), serverArchitectures.contains("x86_64") else {
                return (nil, "virgl_render_server is not universal (arm64 + x86_64)")
            }
        } catch {
            return (nil, "cannot read virgl_render_server: \(error.localizedDescription)")
        }
        guard rosettaAvailable() else {
            return (nil, "Rosetta 2 is not available")
        }
        let version = frameworkVersion(frameworkURL)
        return (NeptuneSupport(
            backend: .d3dmetal(framework: frameworkBinary, version: version),
            adapter: adapter,
            server: renderer,
            apertureMiB: aperture,
            multiRing: multiRing
        ), nil)
    }

    private static func resolveFrameworkBinary(_ url: URL) -> URL? {
        let manager = FileManager.default
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: url.path, isDirectory: &isDirectory) else { return nil }
        if isDirectory.boolValue {
            for candidate in [
                url.appendingPathComponent("Versions/A/D3DMetal"),
                url.appendingPathComponent("D3DMetal"),
            ] where manager.isReadableFile(atPath: candidate.path) {
                return candidate
            }
            return nil
        }
        return manager.isReadableFile(atPath: url.path) ? url : nil
    }

    private static func frameworkVersion(_ url: URL) -> String? {
        let manager = FileManager.default
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            return nil
        }
        for candidate in [
            url.appendingPathComponent("Versions/A/Resources/Info.plist"),
            url.appendingPathComponent("Resources/Info.plist"),
        ] where manager.isReadableFile(atPath: candidate.path) {
            if let data = try? Data(contentsOf: candidate),
               let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] {
                return plist["CFBundleShortVersionString"] as? String
            }
        }
        return nil
    }

    private static func rosettaAvailable() -> Bool {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/arch")
        process.arguments = ["-x86_64", "/usr/bin/true"]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }
}
