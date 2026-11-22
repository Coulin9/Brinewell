import AppKit
import XCTest
@testable import BrinewellVMRuntime

final class DesktopInputTests: XCTestCase {
    private func drainFrames(_ device: VMInputDevice) -> [VMInputEvent] {
        var events: [VMInputEvent] = []
        while let event = device.nextEvent() { events.append(event) }
        return events
    }

    private func drain(_ device: VMInputDevice) -> [VMInputEvent] {
        drainFrames(device).filter { $0.type != 0 }
    }

    @MainActor
    func testHeldKeysRepeatAndFocusRelease() throws {
        let input = try VMInput()
        input.setActive(true)
        input.key(code: 0, down: true)
        input.key(code: 0, down: true) // AppKit autorepeat must not duplicate guest EV_REP.
        input.key(code: 0, down: false)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 30, 1), VMInputEvent(1, 30, 0)])

        input.key(code: 0, down: true)
        input.flagsChanged(code: 56, flags: NSEvent.ModifierFlags(rawValue: NSEvent.ModifierFlags.shift.rawValue | 0x2))
        _ = drainFrames(input.mouse)
        input.button(0, down: true)
        _ = drain(input.keyboard)
        XCTAssertTrue(drain(input.mouse).contains(VMInputEvent(1, 272, 1)))
        input.setActive(false)
        let released = drain(input.keyboard)
        XCTAssertTrue(released.contains(VMInputEvent(1, 30, 0)))
        XCTAssertTrue(released.contains(VMInputEvent(1, 42, 0)))
        XCTAssertTrue(drain(input.mouse).contains(VMInputEvent(1, 272, 0)))
        input.key(code: 0, down: true)
        XCTAssertEqual(drain(input.keyboard), [])
        input.setActive(true)
        input.key(code: 0, down: true)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 30, 1)])
    }

    @MainActor
    func testIndependentModifierSidesAndHostCommandShortcuts() throws {
        let input = try VMInput()
        input.setActive(true)
        let shift = NSEvent.ModifierFlags.shift.rawValue
        input.flagsChanged(code: 56, flags: .init(rawValue: shift | 0x2))
        input.flagsChanged(code: 60, flags: .init(rawValue: shift | 0x6))
        input.flagsChanged(code: 56, flags: .init(rawValue: shift | 0x4))
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 42, 1), VMInputEvent(1, 54, 1), VMInputEvent(1, 42, 0)])
        input.flagsChanged(code: 60, flags: [])
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 54, 0)])
        input.key(code: 0, down: true)
        _ = drain(input.keyboard)
        input.flagsChanged(code: 55, flags: .command)
        let events = drain(input.keyboard)
        XCTAssertTrue(events.contains(VMInputEvent(1, 30, 0)))
        XCTAssertFalse(events.contains { $0.value != 0 })
        XCTAssertFalse(input.key(code: 12, down: true, modifiers: .command))
        input.key(code: 0, down: false)
        XCTAssertEqual(drain(input.keyboard), [])
    }

    @MainActor
    func testCapsLockUsesBalancedToggleAndResynchronizesOnFocus() throws {
        let input = try VMInput()
        input.setActive(true, modifiers: .capsLock)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 58, 1), VMInputEvent(1, 58, 0)])
        input.flagsChanged(code: 57, flags: .capsLock)
        XCTAssertEqual(drain(input.keyboard), [])
        input.setActive(false)
        _ = drain(input.keyboard)
        input.setActive(true, modifiers: [])
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 58, 1), VMInputEvent(1, 58, 0)])
    }

    @MainActor
    func testRelativeMotionHasNoHomingOrResolutionScaling() throws {
        let input = try VMInput()
        input.setActive(true)
        let syn = VMInputEvent(0, 0, 0)
        for focus in ["focus steam 1920x1200", "focus game 123 2560x1440", "focus desktop 1280x800"] {
            input.updateGuestFocus(focus)
            input.move(deltaX: 12, deltaY: -7)
            XCTAssertEqual(drainFrames(input.mouse), [VMInputEvent(2, 0, 12), VMInputEvent(2, 1, -7), syn])
            // A long pause, a guest cursor warp or host screen edge cannot change the next delta.
            input.move(deltaX: 12, deltaY: -7)
            XCTAssertEqual(drainFrames(input.mouse), [VMInputEvent(2, 0, 12), VMInputEvent(2, 1, -7), syn])
        }
    }

    @MainActor
    func testSubpixelMotionAndCaptureReleaseReset() throws {
        let input = try VMInput()
        input.setActive(true)
        input.move(deltaX: 0.25, deltaY: -0.25)
        input.move(deltaX: 0.25, deltaY: -0.25)
        input.move(deltaX: 0.25, deltaY: -0.25)
        XCTAssertEqual(drain(input.mouse), [])
        input.move(deltaX: 0.25, deltaY: -0.25)
        XCTAssertEqual(drain(input.mouse), [VMInputEvent(2, 0, 1), VMInputEvent(2, 1, -1)])
        input.move(deltaX: 0.75, deltaY: -0.75)
        input.setActive(false)
        _ = drainFrames(input.mouse)
        input.setActive(true)
        input.move(deltaX: 0.25, deltaY: -0.25)
        XCTAssertEqual(drain(input.mouse), [])
        input.move(deltaX: .nan, deltaY: 1)
        input.move(deltaX: 1, deltaY: .infinity)
        input.move(deltaX: 0, deltaY: 0)
        XCTAssertEqual(drain(input.mouse), [])
    }

    @MainActor
    func testButtonFramesAndIndependentSideMouse() throws {
        let input = try VMInput()
        input.setActive(true)
        let syn = VMInputEvent(0, 0, 0)
        for number in 0...4 {
            let code = UInt16(272 + number)
            input.button(number, down: true)
            input.button(number, down: true)
            XCTAssertEqual(drainFrames(input.mouse), [VMInputEvent(1, code, 1), syn])
            input.button(number, down: false)
            input.button(number, down: false)
            XCTAssertEqual(drainFrames(input.mouse), [VMInputEvent(1, code, 0), syn])
        }
        input.button(5, down: true)
        input.button(-1, down: true)
        XCTAssertEqual(drainFrames(input.mouse), [])
        input.button(0, down: true)
        input.button(3, down: true)
        _ = drainFrames(input.mouse)
        input.setActive(false)
        let released = drain(input.mouse)
        XCTAssertTrue(released.contains(VMInputEvent(1, 272, 0)))
        XCTAssertTrue(released.contains(VMInputEvent(1, 275, 0)))
        input.move(deltaX: 100, deltaY: 100)
        input.button(0, down: true)
        input.scroll(deltaX: 1, deltaY: 1, precise: false)
        XCTAssertEqual(drainFrames(input.mouse), [])
    }

    @MainActor
    func testTrackpadFractionalScrollAndFocusReset() throws {
        let input = try VMInput()
        input.setActive(true)
        for _ in 0..<4 { input.scroll(deltaX: 0, deltaY: 3, precise: true) }
        XCTAssertEqual(drain(input.mouse), [VMInputEvent(2, 8, 1)])
        input.scroll(deltaX: 0, deltaY: -3, precise: true)
        XCTAssertEqual(drain(input.mouse), [])
        input.scroll(deltaX: 10, deltaY: -9, precise: true)
        XCTAssertEqual(drain(input.mouse), [VMInputEvent(2, 6, -1), VMInputEvent(2, 8, -1)])
        input.scroll(deltaX: 0, deltaY: 9, precise: true)
        input.setActive(false)
        _ = drain(input.mouse)
        input.setActive(true)
        input.scroll(deltaX: 0, deltaY: 3, precise: true)
        XCTAssertEqual(drain(input.mouse), [])
        input.scroll(deltaX: -2, deltaY: -2, precise: false)
        XCTAssertEqual(drain(input.mouse), [VMInputEvent(2, 6, 2), VMInputEvent(2, 8, -1)])
    }

    @MainActor
    func testGuestFocusReleasesPointerAndResetsFractionsWithoutMovingIt() throws {
        let input = try VMInput()
        input.updateGuestFocus("focus desktop 1280x800")
        input.setActive(true)
        input.key(code: 0, down: true)
        _ = drain(input.keyboard)
        input.button(0, down: true)
        input.move(deltaX: 0.75, deltaY: 0.75)
        input.scroll(deltaX: 5, deltaY: 5, precise: true)
        _ = drain(input.mouse)
        input.updateGuestFocus("focus game 123 1920x1080")
        let released = drain(input.mouse)
        XCTAssertTrue(released.contains(VMInputEvent(1, 272, 0)))
        XCTAssertTrue(released.allSatisfy { $0.type == 1 && $0.value == 0 })
        XCTAssertEqual(drain(input.keyboard), [])
        input.move(deltaX: 0.25, deltaY: 0.25)
        input.scroll(deltaX: 5, deltaY: 5, precise: true)
        XCTAssertEqual(drain(input.mouse), [])
        input.key(code: 0, down: false)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 30, 0)])
        input.updateGuestFocus("focus desktop")
        input.move(deltaX: 3, deltaY: 4)
        input.button(0, down: true)
        input.scroll(deltaX: 1, deltaY: 1, precise: false)
        XCTAssertEqual(drain(input.mouse), [])
        input.updateGuestFocus("focus game 0 1920x1080") // Invalid focus must not enable input.
        input.move(deltaX: 3, deltaY: 4)
        XCTAssertEqual(drain(input.mouse), [])
        input.updateGuestFocus("focus steam 1280x800")
        input.move(deltaX: 3, deltaY: 4)
        XCTAssertEqual(drain(input.mouse), [VMInputEvent(2, 0, 3), VMInputEvent(2, 1, 4)])
    }

    @MainActor
    func testSaturationDisablesUntilRealFocusCycle() throws {
        let input = try VMInput()
        input.setActive(true)
        input.key(code: 0, down: true)
        input.button(0, down: true)
        for index in 0..<3000 {
            input.move(deltaX: index.isMultiple(of: 2) ? -20 : 20, deltaY: 1)
        }
        XCTAssertFalse(input.active)
        XCTAssertEqual(drain(input.keyboard).last { $0.code == 30 }?.value, 0)
        XCTAssertEqual(drain(input.mouse).last { $0.code == 272 }?.value, 0)
        input.setActive(true)
        input.key(code: 0, down: true)
        XCTAssertEqual(drain(input.keyboard), [])
        input.setActive(false)
        input.setActive(true)
        input.key(code: 0, down: true)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 30, 1)])
    }

    @MainActor
    func testFocusRestoresHeldModifierBeforeOrdinaryKey() throws {
        let input = try VMInput()
        input.setActive(true)
        input.setActive(false)
        _ = drain(input.keyboard)
        let flags = NSEvent.ModifierFlags(rawValue: NSEvent.ModifierFlags.shift.rawValue | 0x4)
        input.setActive(true, modifiers: flags)
        input.key(code: 0, down: true, modifiers: flags)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 54, 1), VMInputEvent(1, 30, 1)])
    }

    @MainActor
    func testRejectedCapsToggleIsRetriedOnRefocus() throws {
        let input = try VMInput()
        input.setActive(true)
        // Leave two slots: a CapsLock down/up/SYN frame cannot fit.
        XCTAssertTrue(input.keyboard.send(Array(repeating: VMInputEvent(1, 30, 1), count: 4093)))
        input.flagsChanged(code: 57, flags: .capsLock)
        XCTAssertFalse(input.active)
        XCTAssertFalse(drain(input.keyboard).contains(VMInputEvent(1, 58, 1)))
        input.setActive(false)
        input.setActive(true, modifiers: .capsLock)
        XCTAssertEqual(drain(input.keyboard), [VMInputEvent(1, 58, 1), VMInputEvent(1, 58, 0)])
    }
}
