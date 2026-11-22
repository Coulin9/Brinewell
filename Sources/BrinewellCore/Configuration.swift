import Darwin
import Foundation
import Metal

public struct HostCapabilities: Codable, Sendable {
    public let logicalCPUCount: Int
    public let physicalMemoryBytes: UInt64
    public let architecture: String
    public let metalDeviceName: String?
    public let fastestCoreCount: Int?

    public init(logicalCPUCount: Int, physicalMemoryBytes: UInt64, architecture: String, metalDeviceName: String?,
                fastestCoreCount: Int? = nil) {
        self.logicalCPUCount = logicalCPUCount
        self.physicalMemoryBytes = physicalMemoryBytes
        self.architecture = architecture
        self.metalDeviceName = metalDeviceName
        self.fastestCoreCount = fastestCoreCount
    }

    public static func detect() -> HostCapabilities {
        #if arch(arm64)
        let architecture = "arm64"
        #elseif arch(x86_64)
        let architecture = "x86_64"
        #else
        let architecture = "unknown"
        #endif
        return HostCapabilities(
            logicalCPUCount: ProcessInfo.processInfo.activeProcessorCount,
            physicalMemoryBytes: ProcessInfo.processInfo.physicalMemory,
            architecture: architecture,
            metalDeviceName: MTLCreateSystemDefaultDevice()?.name,
            fastestCoreCount: detectFastestCoreCount()
        )
    }

    private static func detectFastestCoreCount() -> Int? {
        guard let levelCount = positiveSysctlInt("hw.nperflevels") else { return nil }
        var performanceCoreCount: Int?
        for level in 0..<levelCount {
            let key = "hw.perflevel\(level)"
            let nameKey = "\(key).name"
            var size = 0
            guard sysctlbyname(nameKey, nil, &size, nil, 0) == 0, size > 0 else { continue }
            var name = [UInt8](repeating: 0, count: size)
            guard sysctlbyname(nameKey, &name, &size, nil, 0) == 0 else { continue }
            switch String(decoding: name.prefix { $0 != 0 }, as: UTF8.self) {
            case "Super":
                if let count = positiveSysctlInt("\(key).physicalcpu") { return count }
            case "Performance":
                performanceCoreCount = positiveSysctlInt("\(key).physicalcpu")
            default:
                continue
            }
        }
        return performanceCoreCount
    }

    private static func positiveSysctlInt(_ key: String) -> Int? {
        var value: Int32 = 0
        var size = MemoryLayout.size(ofValue: value)
        guard sysctlbyname(key, &value, &size, nil, 0) == 0, value > 0 else { return nil }
        return Int(value)
    }

    public var maximumCPUs: Int { max(1, logicalCPUCount) }

    public var maximumMemoryGiB: Int {
        let physicalGiB = Int(physicalMemoryBytes / 1_073_741_824)
        return max(1, physicalGiB - 4)
    }
}

public enum GraphicsBackend: String, Codable, Sendable, CaseIterable {
    case venus
    case neptune
    case mullion

    public var title: String { rawValue }

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        let value = try container.decode(String.self)
        // Preserve the rest of older configurations while retiring auto-selection.
        if value == "automatic" {
            self = .neptune
        } else if let backend = Self(rawValue: value) {
            self = backend
        } else {
            throw DecodingError.dataCorruptedError(in: container, debugDescription: "Invalid graphics backend: \(value)")
        }
    }
}

public struct VMSharedFolder: Codable, Equatable, Sendable {
    public let path: String
    public var readOnly: Bool

    public init(path: String, readOnly: Bool = true) {
        self.path = path
        self.readOnly = readOnly
    }

    fileprivate func validatePath() throws {
        guard path.hasPrefix("/"), !path.utf8.contains(0) else {
            throw ConfigurationError.invalidSharedFolderPath(path)
        }
    }

    public func resolvedPath() throws -> String {
        try validatePath()
        let url = URL(fileURLWithPath: path).resolvingSymlinksInPath()
        let files = FileManager.default
        var isDirectory: ObjCBool = false
        guard files.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue,
              files.isReadableFile(atPath: url.path), files.isExecutableFile(atPath: url.path) else {
            throw ConfigurationError.invalidSharedFolderPath(path)
        }
        return url.path
    }
}

public struct PendingDiskShrink: Codable, Equatable, Sendable {
    public var diskImagePath: String
    public var capacityGiB: Int
    public var bootStarted: Bool
    public var identity: SystemDiskIdentity

    public init(diskImagePath: String, capacityGiB: Int, bootStarted: Bool = false,
                identity: SystemDiskIdentity) {
        self.diskImagePath = diskImagePath
        self.capacityGiB = capacityGiB
        self.bootStarted = bootStarted
        self.identity = identity
    }
}

public struct VMConfiguration: Codable, Equatable, Sendable {
    public var diskImagePath: String?
    public var cpuCount: Int
    public var memoryGiB: Int
    public var display: VMDisplayConfiguration
    public var graphicsBackend: GraphicsBackend
    public var d3dMetalFrameworkPath: String?
    public var neptuneApertureMiB: Int
    public var neptuneMultiRing: Bool
    public var gameHUDLevel: GameHUDLevel
    public var gameHUDPosition: GameHUDPosition
    public var sharedFolder: VMSharedFolder?
    public var network: VMNetworkConfiguration
    public var pendingDiskShrink: PendingDiskShrink?

    public init(diskImagePath: String? = nil, cpuCount: Int, memoryGiB: Int,
                graphicsBackend: GraphicsBackend = .neptune,
                d3dMetalFrameworkPath: String? = nil,
                neptuneApertureMiB: Int = 32768,
                neptuneMultiRing: Bool = false,
                gameHUDLevel: GameHUDLevel = .off,
                gameHUDPosition: GameHUDPosition = .topRight,
                display: VMDisplayConfiguration = VMDisplayConfiguration(),
                sharedFolder: VMSharedFolder? = nil,
                network: VMNetworkConfiguration = VMNetworkConfiguration(),
                pendingDiskShrink: PendingDiskShrink? = nil) {
        self.diskImagePath = diskImagePath
        self.cpuCount = cpuCount
        self.memoryGiB = memoryGiB
        self.display = display
        self.graphicsBackend = graphicsBackend
        self.d3dMetalFrameworkPath = d3dMetalFrameworkPath
        self.neptuneApertureMiB = neptuneApertureMiB
        self.neptuneMultiRing = neptuneMultiRing
        self.gameHUDLevel = gameHUDLevel
        self.gameHUDPosition = gameHUDPosition
        self.sharedFolder = sharedFolder
        self.network = network
        self.pendingDiskShrink = pendingDiskShrink
    }

    private enum CodingKeys: String, CodingKey {
        case diskImagePath, cpuCount, memoryGiB, display, graphicsBackend
        case d3dMetalFrameworkPath, neptuneApertureMiB, neptuneMultiRing, sharedFolder, network
        case gameHUDLevel, gameHUDPosition, pendingDiskShrink
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        diskImagePath = try container.decodeIfPresent(String.self, forKey: .diskImagePath)
        cpuCount = try container.decode(Int.self, forKey: .cpuCount)
        memoryGiB = try container.decode(Int.self, forKey: .memoryGiB)
        display = try container.decodeIfPresent(VMDisplayConfiguration.self, forKey: .display) ?? VMDisplayConfiguration()
        graphicsBackend = try container.decodeIfPresent(GraphicsBackend.self, forKey: .graphicsBackend) ?? .neptune
        d3dMetalFrameworkPath = try container.decodeIfPresent(String.self, forKey: .d3dMetalFrameworkPath)
        neptuneApertureMiB = try container.decodeIfPresent(Int.self, forKey: .neptuneApertureMiB) ?? 32768
        neptuneMultiRing = try container.decodeIfPresent(Bool.self, forKey: .neptuneMultiRing) ?? false
        if let level = try container.decodeIfPresent(GameHUDLevel.self, forKey: .gameHUDLevel) {
            gameHUDLevel = level
        } else {
            let legacy = try decoder.container(keyedBy: LegacyHUDKeys.self)
            gameHUDLevel = (try legacy.decodeIfPresent(Bool.self, forKey: .gameHUDEnabled) ?? false) ? .compact : .off
        }
        gameHUDPosition = try container.decodeIfPresent(GameHUDPosition.self, forKey: .gameHUDPosition) ?? .topRight
        sharedFolder = try container.decodeIfPresent(VMSharedFolder.self, forKey: .sharedFolder)
        network = try container.decodeIfPresent(VMNetworkConfiguration.self, forKey: .network) ?? VMNetworkConfiguration()
        // A pending shrink written without a disk identity cannot be resumed.
        pendingDiskShrink = try? container.decodeIfPresent(PendingDiskShrink.self, forKey: .pendingDiskShrink)
    }

    public static func defaults(for host: HostCapabilities) -> VMConfiguration {
        let physicalGiB = Int(host.physicalMemoryBytes / 1_073_741_824)
        let cpuCount = host.fastestCoreCount ?? max(1, host.maximumCPUs - 2)
        return VMConfiguration(
            cpuCount: min(host.maximumCPUs, max(1, cpuCount)),
            memoryGiB: min(host.maximumMemoryGiB, max(1, physicalGiB / 2))
        )
    }

    private enum LegacyHUDKeys: String, CodingKey { case gameHUDEnabled }

    public func validate(for host: HostCapabilities) throws {
        try validateSettings(for: host)
        try network.validate()
        if let diskImagePath {
            let url = URL(fileURLWithPath: diskImagePath)
            let values = try? url.resourceValues(forKeys: [.isRegularFileKey])
            guard values?.isRegularFile == true, FileManager.default.isReadableFile(atPath: diskImagePath) else {
                throw ConfigurationError.invalidDiskImagePath
            }
        }
    }

    // Saved settings remain valid when an external disk is disconnected or moved.
    // Runtime validation checks whether that disk is currently readable.
    fileprivate func validateSettings(for host: HostCapabilities) throws {
        try display.validate()
        try sharedFolder?.validatePath()
        guard (1...host.maximumCPUs).contains(cpuCount) else {
            throw ConfigurationError.invalidCPUCount(maximum: host.maximumCPUs)
        }
        guard (1...host.maximumMemoryGiB).contains(memoryGiB) else {
            throw ConfigurationError.invalidMemoryGiB(maximum: host.maximumMemoryGiB)
        }
        guard (8192...65536).contains(neptuneApertureMiB) else {
            throw ConfigurationError.invalidNeptuneAperture
        }
        if let d3dMetalFrameworkPath {
            guard d3dMetalFrameworkPath.hasPrefix("/") else {
                throw ConfigurationError.invalidD3DMetalFrameworkPath
            }
        }
        if let diskImagePath {
            guard diskImagePath.hasPrefix("/") else {
                throw ConfigurationError.invalidDiskImagePath
            }
        }
    }
}

private enum ConfigurationError: LocalizedError {
    case invalidCPUCount(maximum: Int)
    case invalidMemoryGiB(maximum: Int)
    case invalidDiskImagePath
    case invalidNeptuneAperture
    case invalidD3DMetalFrameworkPath
    case invalidSharedFolderPath(String)

    var errorDescription: String? {
        switch self {
        case .invalidCPUCount(let maximum):
            return String(localized: "CPU count must be between 1 and \(maximum).")
        case .invalidMemoryGiB(let maximum):
            return String(localized: "Memory must be between 1 and \(maximum) GiB.")
        case .invalidDiskImagePath:
            return String(localized: "The disk image must be an absolute path to a readable regular file.")
        case .invalidNeptuneAperture:
            return String(localized: "The Neptune aperture must be between 8192 and 65536 MiB.")
        case .invalidD3DMetalFrameworkPath:
            return String(localized: "The D3DMetal framework path must be absolute.")
        case .invalidSharedFolderPath(let path):
            return String(localized: "The shared folder must be an accessible local directory: \(path)")
        }
    }
}

public struct ConfigurationStore: Sendable {
    public let url: URL

    public init(url: URL) {
        self.url = url
    }

    public static func defaultURL() throws -> URL {
        let applicationSupport = try FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: false
        )
        return applicationSupport.appendingPathComponent("Brinewell", isDirectory: true)
            .appendingPathComponent("configuration.json")
    }

    public func load(for host: HostCapabilities) throws -> VMConfiguration {
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch let error as CocoaError where error.code == .fileReadNoSuchFile {
            return .defaults(for: host)
        }
        let configuration = try JSONDecoder().decode(VMConfiguration.self, from: data)
        try configuration.validateSettings(for: host)
        return configuration
    }

    /// Decode the stored configuration without validating it against this host.
    ///
    /// Settings outside this host's resource limits must still be visible so
    /// their owner can correct and save them instead of losing the stored values.
    public func decode() throws -> VMConfiguration {
        let data = try Data(contentsOf: url)
        return try JSONDecoder().decode(VMConfiguration.self, from: data)
    }

    /// HUD settings can be saved while a VM is running.
    /// Read the stored configuration so unrelated in-memory settings stay untouched.
    public func setGameHUD(level: GameHUDLevel, position: GameHUDPosition, for host: HostCapabilities) throws {
        var configuration = try decode()
        configuration.gameHUDLevel = level
        configuration.gameHUDPosition = position
        try save(configuration, for: host)
    }

    /// The graphics backend preference can be saved while a VM is running.
    /// Read the stored configuration so unrelated in-memory settings stay untouched.
    public func setGraphicsBackend(_ backend: GraphicsBackend, for host: HostCapabilities) throws {
        var configuration = try decode()
        configuration.graphicsBackend = backend
        try save(configuration, for: host)
    }

    public func save(_ configuration: VMConfiguration, for host: HostCapabilities) throws {
        try configuration.validateSettings(for: host)
        let data = try JSONEncoder().encode(configuration)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let temporary = url.deletingLastPathComponent().appendingPathComponent(".configuration-\(UUID().uuidString).json")
        try writePrivateConfiguration(data, to: temporary)
        defer { try? FileManager.default.removeItem(at: temporary) }
        guard rename(temporary.path, url.path) == 0 else { throw CocoaError(.fileWriteUnknown) }
    }
}
