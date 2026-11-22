import Foundation

public struct DisplayResolution: Codable, Hashable, Sendable {
    public let width: Int
    public let height: Int

    public init(width: Int, height: Int) {
        self.width = width
        self.height = height
    }

    public var label: String { "\(width) × \(height)" }
}

public struct VMDisplayConfiguration: Codable, Equatable, Sendable {
    // The shipped Linux virtio-gpu DRM driver limits each mode dimension to 8192.
    public static let maximumDimension = 8192
    public var width: Int
    public var height: Int
    public var refreshRate: Int

    public var resolution: DisplayResolution {
        get { DisplayResolution(width: width, height: height) }
        set { width = newValue.width; height = newValue.height }
    }

    public init(width: Int = 1280, height: Int = 800, refreshRate: Int = 60) {
        self.width = width
        self.height = height
        self.refreshRate = refreshRate
    }

    public func validate() throws {
        guard (1...Self.maximumDimension).contains(width), (1...Self.maximumDimension).contains(height) else {
            throw ValidationError.dimensions
        }
        guard refreshRate > 0 else { throw ValidationError.refreshRate }
        // DisplayID Type I has a 24-bit pixel clock in 10 kHz units. Match
        // libkrun's 100 kHz timing precision and reduced blanking totals.
        let maximumRefreshRate = 167_772_100_000 / ((width + 160) * (height + 50))
        guard refreshRate <= maximumRefreshRate else {
            throw ValidationError.pixelClock(maximumRefreshRate: maximumRefreshRate)
        }
    }

    private enum ValidationError: LocalizedError {
        case dimensions
        case refreshRate
        case pixelClock(maximumRefreshRate: Int)

        var errorDescription: String? {
            switch self {
            case .dimensions:
                return String(localized: "Display width and height must be between 1 and \(VMDisplayConfiguration.maximumDimension) pixels.")
            case .refreshRate:
                return String(localized: "The refresh rate must be a positive integer in Hz.")
            case .pixelClock(let maximum):
                return String(localized: "The maximum refresh rate for this resolution is \(maximum) Hz. Lower the refresh rate or resolution.")
            }
        }
    }
}
