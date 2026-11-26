import GameController
import XCTest
@testable import BrinewellVMRuntime

final class PhysicalGamepadTests: XCTestCase {
    @MainActor
    func testHomeEventsAndGestureLifecycle() throws {
        let controller = GCController.withExtendedGamepad()
        let pad = try XCTUnwrap(controller.extendedGamepad)
        let home = try XCTUnwrap(pad.buttonHome)
        let options = try XCTUnwrap(pad.buttonOptions)
        let menu = pad.buttonMenu
        let buttons = [home, options, menu]
        home.preferredSystemGestureState = .alwaysReceive
        options.preferredSystemGestureState = .disabled
        menu.preferredSystemGestureState = .enabled
        let original = buttons.map(\.preferredSystemGestureState)
        var frames: [GamepadState] = []
        var onEmit: ((GamepadState) -> Void)?
        let input = GamepadInputState(emit: { state, _ in
            frames.append(state)
            onEmit?(state)
        }, sendScript: { _, _ in true }, cancelRelease: {})
        var selections: [Bool] = []
        input.onSelectionChange = { selections.append($0) }
        let bridge = PhysicalGamepad(input: input)
        defer { bridge.stop() }

        bridge.setController(controller)
        bridge.setActive(true)
        XCTAssertTrue(buttons.allSatisfy { $0.preferredSystemGestureState == .disabled })
        frames.removeAll()

        func expectHomeChange(_ value: Float, pressed: Bool) {
            let changed = expectation(description: "Home state changed")
            let duplicate = expectation(description: "no duplicate Home state")
            duplicate.isInverted = true
            var matches = 0
            onEmit = { state in
                guard state.pressed.contains(316) == pressed else { return }
                matches += 1
                if matches == 1 { changed.fulfill() } else { duplicate.fulfill() }
            }
            home.setValue(value)
            wait(for: [changed, duplicate], timeout: 1)
            onEmit = nil
            XCTAssertEqual(matches, 1)
        }

        expectHomeChange(1, pressed: true)
        expectHomeChange(0, pressed: false)
        XCTAssertEqual(frames.map { $0.pressed.contains(316) }, [true, false])

        bridge.setActive(false)
        XCTAssertEqual(frames.last, GamepadState())
        XCTAssertEqual(buttons.map(\.preferredSystemGestureState), original)
        frames.removeAll()
        let inactiveEmit = expectation(description: "no inactive input")
        inactiveEmit.isInverted = true
        onEmit = { _ in inactiveEmit.fulfill() }
        home.setValue(1)
        wait(for: [inactiveEmit], timeout: 1)
        onEmit = nil
        XCTAssertTrue(frames.isEmpty)
        bridge.setActive(true)
        XCTAssertEqual(frames.last?.pressed.contains(316), true)
        frames.removeAll()
        expectHomeChange(0, pressed: false)

        let oldHandler = try XCTUnwrap(home.pressedChangedHandler)
        let oldProfileHandler = try XCTUnwrap(pad.valueChangedHandler)
        let replacement = GCController.withExtendedGamepad()
        let replacementPad = try XCTUnwrap(replacement.extendedGamepad)
        let replacementHome = try XCTUnwrap(replacementPad.buttonHome)
        let replacementOptions = try XCTUnwrap(replacementPad.buttonOptions)
        let replacementButtons = [replacementHome, replacementOptions, replacementPad.buttonMenu]
        let replacementOriginal = replacementButtons.map(\.preferredSystemGestureState)
        home.setValue(1)
        bridge.setController(replacement)
        XCTAssertEqual(selections, [true])
        XCTAssertEqual(buttons.map(\.preferredSystemGestureState), original)
        XCTAssertTrue(replacementButtons.allSatisfy { $0.preferredSystemGestureState == .disabled })
        frames.removeAll()
        oldHandler(home, 1, true)
        oldProfileHandler(pad, home)
        XCTAssertTrue(frames.isEmpty)

        let replacementHandler = try XCTUnwrap(replacementHome.pressedChangedHandler)
        replacementHome.setValue(1)
        bridge.stop()
        XCTAssertEqual(frames.last, GamepadState())
        XCTAssertEqual(replacementButtons.map(\.preferredSystemGestureState), replacementOriginal)
        frames.removeAll()
        replacementHandler(replacementHome, 1, true)
        XCTAssertTrue(frames.isEmpty)
    }
}
