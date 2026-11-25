/// State shared by all VM generations in one logical session.
public struct VMSessionPolicy {
    public enum ResetDecision { case stop, abortShrink, tooManyResets, restart }

    public var generation: UInt64 = 0
    public var active = false
    public var requestedStop = false
    public var shrinkPreparation = false
    public var fatalError = false
    public var ready = false
    public private(set) var preReadyResetCount = 0
    public let maxPreReadyResets: Int

    public init(maxPreReadyResets: Int = 3) { self.maxPreReadyResets = maxPreReadyResets }

    public mutating func begin(shrinkPreparation: Bool) {
        active = true
        requestedStop = false
        self.shrinkPreparation = shrinkPreparation
        preReadyResetCount = 0
    }

    public mutating func beginBoot() -> UInt64 {
        generation += 1
        fatalError = false
        ready = false
        return generation
    }

    public func accepts(_ token: UInt64) -> Bool { active && generation == token }
    public var shouldPreserveWindow: Bool {
        !requestedStop && !shrinkPreparation && preReadyResetCount < maxPreReadyResets
    }
    public mutating func markReady() -> Bool {
        guard !requestedStop && !fatalError else { return false }
        ready = true
        preReadyResetCount = 0
        return true
    }
    public mutating func decideReset() -> ResetDecision {
        if requestedStop { return .stop }
        if shrinkPreparation { return .abortShrink }
        if !ready && preReadyResetCount >= maxPreReadyResets { return .tooManyResets }
        if !ready { preReadyResetCount += 1 }
        return .restart
    }
}
