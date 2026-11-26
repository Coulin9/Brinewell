import Foundation

struct GamepadState: Equatable {
    static let buttons = [304, 305, 307, 308, 310, 311, 314, 315, 316, 317, 318]
    static let axes = [0, 1, 2, 3, 4, 5, 16, 17]
    var pressed: Set<Int> = []
    var values: [Int: Int] = [:]

    static func stick(_ value: Float, inverted: Bool = false) -> Int {
        let value = max(-1, min(1, inverted ? -value : value))
        return value < 0 ? Int(value * 32768) : Int(value * 32767)
    }

    static func trigger(_ value: Float) -> Int { Int(max(0, min(1, value)) * 255) }
    static func hat(_ value: Float, inverted: Bool = false) -> Int {
        let value = inverted ? -value : value
        return value > 0 ? 1 : (value < 0 ? -1 : 0)
    }

    var frame: String {
        let keys = Self.buttons.map { "1:\($0):\(pressed.contains($0) ? 1 : 0)" }
        let abs = Self.axes.map { "3:\($0):\(values[$0, default: 0])" }
        return "ev " + (keys + abs).joined(separator: " ")
    }
}

@MainActor
final class GamepadInputState {
    private(set) var selected = false
    private(set) var active = false
    private(set) var script: (type: Int, code: Int)?
    private(set) var token: UInt64 = 0
    private var current = GamepadState()
    private let emit: (GamepadState, Bool) -> Void
    private let sendScript: (String, Bool) -> Bool
    private let cancelRelease: () -> Void
    private let physicalPending: () -> Bool
    var onSelectionChange: ((Bool) -> Void)?

    init(emit: @escaping (GamepadState, Bool) -> Void,
         sendScript: @escaping (String, Bool) -> Bool,
         cancelRelease: @escaping () -> Void,
         physicalPending: @escaping () -> Bool = { false }) {
        self.emit = emit; self.sendScript = sendScript; self.cancelRelease = cancelRelease
        self.physicalPending = physicalPending
    }

    var physical: Bool { selected && active }

    func select(_ selected: Bool, state: GamepadState = GamepadState()) {
        let changed = self.selected != selected
        if physical { emit(GamepadState(), true) }
        self.selected = selected
        current = selected ? state : GamepadState()
        if changed { onSelectionChange?(selected) }
        if physical { _ = resetScript(); emit(current, false) }
    }

    func setActive(_ active: Bool) {
        guard self.active != active else { return }
        if physical { emit(GamepadState(), true) }
        self.active = active
        if physical { _ = resetScript(); emit(current, false) }
    }

    func update(_ state: GamepadState) {
        guard state != current else { return }
        current = state
        if physical { emit(state, false) }
    }

    func synchronize() { if physical { emit(current, false) } }

    func press(type: Int, code: Int, value: Int) -> UInt64? {
        guard !physical, !physicalPending(), script == nil,
              sendScript("ev \(type):\(code):\(value)", false) else { return nil }
        token &+= 1
        script = (type, code)
        return token
    }

    @discardableResult func neutralScript() -> Bool {
        guard !physical, !physicalPending() else { return false }
        return resetScript()
    }

    @discardableResult func resetScript() -> Bool {
        if let script, !sendScript("ev \(script.type):\(script.code):0", true) { return false }
        token &+= 1
        cancelRelease()
        script = nil
        return true
    }

    func invalidateScript() {
        token &+= 1
        cancelRelease()
        script = nil
    }

    func release(_ token: UInt64) {
        guard token == self.token, !physical else { return }
        _ = neutralScript()
    }
}
