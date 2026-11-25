import Foundation

public enum SetupStep: String, Sendable, Equatable, CaseIterable {
    case systemDisk, graphics
}

public enum DiskReadiness: Sendable, Equatable {
    case ready(URL), notConfigured, missingConfigured(URL), invalid(URL, String)
    public var isReady: Bool { if case .ready = self { return true }; return false }
}

public enum GraphicsReadiness: Sendable, Equatable {
    case ready(D3DMetalVersionInfo?), notRequiredForBackend(GraphicsBackend)
    case d3dMetalMissing, d3dMetalInvalid(String), rosettaMissing
    public var isReady: Bool {
        switch self {
        case .ready, .notRequiredForBackend: return true
        default: return false
        }
    }
}

public struct SetupReadiness: Sendable, Equatable {
    public let backend: GraphicsBackend
    public let disk: DiskReadiness
    public let graphics: GraphicsReadiness

    public init(backend: GraphicsBackend, disk: DiskReadiness, graphics: GraphicsReadiness) {
        self.backend = backend
        self.disk = disk
        self.graphics = graphics
    }

    public var neededSteps: [SetupStep] {
        var steps: [SetupStep] = []
        if !disk.isReady { steps.append(.systemDisk) }
        if !graphics.isReady { steps.append(.graphics) }
        return steps
    }
    public var initialStep: SetupStep? { neededSteps.first }
    public var isReadyToStart: Bool { neededSteps.isEmpty }
    public func nextStep(after completedStep: SetupStep) -> SetupStep? {
        SetupStep.allCases.drop(while: { $0 != completedStep }).dropFirst().first { neededSteps.contains($0) }
    }

    public static func evaluate(
        _ configuration: VMConfiguration,
        diskExists: (URL) -> Bool = { FileManager.default.fileExists(atPath: $0.path) },
        validateDisk: (URL) throws -> Void = { url in
            guard FileManager.default.isWritableFile(atPath: url.path) else {
                throw SteamOSDeploymentError("The system disk is not writable: \(url.path)")
            }
            try SteamOSDisk.validateSystem(at: url)
        },
        inspectFramework: (URL) throws -> D3DMetalInspection = { try D3DMetalFramework.inspect(at: $0) },
        rosettaAvailable: () -> Bool = { D3DMetalFramework.isRosettaAvailable() }
    ) -> SetupReadiness {
        let disk: DiskReadiness
        if let path = configuration.diskImagePath, !path.isEmpty {
            let url = URL(fileURLWithPath: path)
            if !diskExists(url) { disk = .missingConfigured(url) }
            else {
                do { try validateDisk(url); disk = .ready(url) }
                catch { disk = .invalid(url, error.localizedDescription) }
            }
        } else { disk = .notConfigured }

        let graphics: GraphicsReadiness
        if !configuration.graphicsBackend.requiresD3DMetalSetup {
            graphics = .notRequiredForBackend(configuration.graphicsBackend)
        } else if let path = configuration.d3dMetalFrameworkPath, !path.isEmpty {
            do {
                let inspection = try inspectFramework(URL(fileURLWithPath: path))
                graphics = rosettaAvailable() ? .ready(inspection.version) : .rosettaMissing
            } catch { graphics = .d3dMetalInvalid(error.localizedDescription) }
        } else { graphics = .d3dMetalMissing }
        return SetupReadiness(backend: configuration.graphicsBackend, disk: disk, graphics: graphics)
    }
}
