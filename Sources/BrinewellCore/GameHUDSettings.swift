import Foundation

public enum GameHUDLevel: Int, Codable, CaseIterable, Sendable {
    case off = 0, fps = 1, compact = 2, detailed = 3, full = 4

    public var title: String {
        switch self {
        case .off: String(localized: "Off")
        case .fps: String(localized: "1 · FPS")
        case .compact: String(localized: "2 · Compact")
        case .detailed: String(localized: "3 · Detailed")
        case .full: String(localized: "4 · Full")
        }
    }

    public var detail: String {
        switch self {
        case .off: String(localized: "Hide the performance overlay.")
        case .fps: String(localized: "Show the current game frame rate.")
        case .compact: String(localized: "Show game frame rate and frame time, plus host CPU and GPU utilization, power, and memory.")
        case .detailed: String(localized: "Add a frame-time graph, temperatures, clock speeds, GPU memory, battery, and game resolution.")
        case .full: String(localized: "Add 1% low FPS, frame-time range, per-core metrics, swap, fans, and total system power.")
        }
    }
}

public enum GameHUDPosition: String, Codable, CaseIterable, Sendable {
    case topLeft = "top-left", topRight = "top-right"
    case bottomLeft = "bottom-left", bottomRight = "bottom-right"

    public var title: String {
        switch self {
        case .topLeft: String(localized: "Top Left")
        case .topRight: String(localized: "Top Right")
        case .bottomLeft: String(localized: "Bottom Left")
        case .bottomRight: String(localized: "Bottom Right")
        }
    }
}
