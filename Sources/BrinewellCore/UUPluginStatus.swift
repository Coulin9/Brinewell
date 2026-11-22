import Foundation

/// State reported by the guest's dedicated UU installation service.
public struct UUPluginStatus: Equatable, Sendable {
    public enum Phase: Sendable { case unavailable, checking, absent, installing, installed, failed }

    public private(set) var phase: Phase = .unavailable
    public private(set) var channelReady = false
    public private(set) var progress = 0
    public private(set) var message = String(localized: "Start the virtual machine to install UU.")
    public private(set) var bindingCode: String?

    public init() {}

    public var canInstall: Bool { channelReady && (phase == .absent || phase == .failed) }
    public var canRefresh: Bool { channelReady && phase != .installing && phase != .checking }

    public mutating func beginInstallation() {
        phase = .installing
        progress = 0
        bindingCode = nil
        message = String(localized: "Checking installation requirements…")
    }

    public mutating func fail(_ message: String) {
        phase = .failed
        bindingCode = nil
        self.message = message
    }

    @discardableResult
    public mutating func receive(_ line: String) -> Bool {
        guard line.utf8.count <= 4096, !line.contains("\n"), !line.contains("\r") else { return false }
        if line == "hello" {
            channelReady = true
            phase = .checking
            bindingCode = nil
            message = String(localized: "Checking UU installation status…")
            return true
        }
        guard channelReady else { return false }
        let parts = line.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true)
        guard parts.count >= 2 else { return false }
        switch parts[0] {
        case "progress":
            guard parts.count == 3, let value = Int(parts[1]), (0...100).contains(value) else { return false }
            phase = .installing
            progress = value
            bindingCode = nil
            message = String(parts[2])
        case "status":
            guard parts.count == 3 else { return false }
            switch parts[1] {
            case "absent":
                phase = .absent
                progress = 0
                bindingCode = nil
            case "installed":
                phase = .installed
                progress = 100
                bindingCode = nil
            case "error":
                fail(String(parts[2]))
            default: return false
            }
            message = String(parts[2])
        case "binding":
            guard parts.count == 2, phase == .installed,
                  UUQRCode.isBindingCode(String(parts[1])) else { return false }
            // UU rotates this binding code; its persistent device UUID stays in the guest.
            bindingCode = String(parts[1])
        default: return false
        }
        return true
    }
}
