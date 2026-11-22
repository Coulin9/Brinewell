import Foundation
import GameController

@MainActor
final class PhysicalGamepad {
    private let input: GamepadInputState
    private let disabled = ProcessInfo.processInfo.environment["BRINEWELL_DISABLE_PHYSICAL_GAMEPAD"] == "1"
    private var controller: GCController?
    private var observers: [NSObjectProtocol] = []
    private var active = false
    private var savedGestures: [(GCControllerButtonInput, GCControllerElement.SystemGestureState)] = []

    init(input: GamepadInputState) { self.input = input }

    func start() {
        guard !disabled else { return }
        let center = NotificationCenter.default
        for name in [NSNotification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            })
        }
        refresh()
    }

    func setActive(_ active: Bool) {
        guard !disabled else { return }
        guard self.active != active else { return }
        self.active = active
        if active {
            disableSystemGestures()
            input.setActive(true)
        } else {
            input.setActive(false)
            restoreSystemGestures()
        }
    }

    func stop() {
        setController(nil)
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers.removeAll()
    }

    private func refresh() {
        setController(GCController.controllers().first { $0.extendedGamepad != nil })
    }

    func setController(_ next: GCController?) {
        guard next !== controller else { return }
        controller?.extendedGamepad?.valueChangedHandler = nil
        controller?.extendedGamepad?.buttonHome?.pressedChangedHandler = nil
        restoreSystemGestures()
        input.select(false)
        controller = next
        guard let next, let gamepad = next.extendedGamepad else { return }
        next.handlerQueue = .main
        if active { disableSystemGestures() }
        input.select(true, state: Self.snapshot(gamepad))
        gamepad.valueChangedHandler = { [weak self, weak next] gamepad, _ in
            MainActor.assumeIsolated {
                guard let self, let next, self.controller === next else { return }
                self.input.update(Self.snapshot(gamepad))
            }
        }
        gamepad.buttonHome?.pressedChangedHandler = { [weak self, weak next, weak gamepad] _, _, _ in
            MainActor.assumeIsolated {
                guard let self, let next, let gamepad, self.controller === next else { return }
                self.input.update(Self.snapshot(gamepad))
            }
        }
    }

    private func disableSystemGestures() {
        guard let gamepad = controller?.extendedGamepad, savedGestures.isEmpty else { return }
        for button in [gamepad.buttonHome, gamepad.buttonOptions, gamepad.buttonMenu].compactMap({ $0 }) {
            savedGestures.append((button, button.preferredSystemGestureState))
            button.preferredSystemGestureState = .disabled
        }
    }

    private func restoreSystemGestures() {
        for (button, state) in savedGestures { button.preferredSystemGestureState = state }
        savedGestures.removeAll()
    }

    private static func snapshot(_ pad: GCExtendedGamepad) -> GamepadState {
        var state = GamepadState()
        let buttons: [(Int, GCControllerButtonInput?)] = [
            (304, pad.buttonA), (305, pad.buttonB), (307, pad.buttonX), (308, pad.buttonY),
            (310, pad.leftShoulder), (311, pad.rightShoulder), (314, pad.buttonOptions),
            (315, pad.buttonMenu), (316, pad.buttonHome), (317, pad.leftThumbstickButton),
            (318, pad.rightThumbstickButton)
        ]
        for (code, button) in buttons where button?.isPressed == true { state.pressed.insert(code) }
        state.values = [
            0: GamepadState.stick(pad.leftThumbstick.xAxis.value),
            1: GamepadState.stick(pad.leftThumbstick.yAxis.value, inverted: true),
            2: GamepadState.trigger(pad.leftTrigger.value),
            3: GamepadState.stick(pad.rightThumbstick.xAxis.value),
            4: GamepadState.stick(pad.rightThumbstick.yAxis.value, inverted: true),
            5: GamepadState.trigger(pad.rightTrigger.value),
            16: GamepadState.hat(pad.dpad.xAxis.value),
            17: GamepadState.hat(pad.dpad.yAxis.value, inverted: true)
        ]
        return state
    }
}
