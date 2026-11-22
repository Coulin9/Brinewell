import AppKit
import CKrun
import Darwin
import XCTest
@testable import BrinewellVMRuntime

final class VMInputBackendTests: XCTestCase {
    private func readable(_ fd: Int32) -> Bool {
        var descriptor = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        return poll(&descriptor, 1, 0) == 1 && descriptor.revents & Int16(POLLIN) != 0
    }

    func testConfigAndProviderCallbacks() throws {
        for kind in [VMInputDevice.Kind.keyboard, .mouse] {
            let device = try VMInputDevice(kind: kind)
            let config = device.makeConfig()
            let provider = device.makeProvider()
            var configInstance: UnsafeMutableRawPointer?
            var providerInstance: UnsafeMutableRawPointer?
            XCTAssertEqual(config.create!(&configInstance, config.create_userdata, nil), 0)
            XCTAssertEqual(provider.create!(&providerInstance, provider.create_userdata, nil), 0)
            let configPtr = try XCTUnwrap(configInstance)
            let providerPtr = try XCTUnwrap(providerInstance)
            defer {
                XCTAssertEqual(config.vtable.destroy!(configPtr), 0)
                XCTAssertEqual(provider.vtable.destroy!(providerPtr), 0)
            }
            XCTAssertEqual(provider.vtable.get_ready_efd!(providerPtr), device.readyFD)
            var bytes = [UInt8](repeating: 255, count: 64)
            let length = bytes.withUnsafeMutableBufferPointer {
                config.vtable.query_event_capabilities!(configPtr, 1, $0.baseAddress, $0.count)
            }
            XCTAssertGreaterThan(length, 0)
            if kind == .keyboard {
                XCTAssertNotEqual(bytes[30 / 8] & (1 << (30 % 8)), 0)
                XCTAssertEqual(bytes[272 / 8] & (1 << (272 % 8)), 0)
            }
            var info = krun_input_absinfo()
            XCTAssertEqual(config.vtable.query_abs_info!(configPtr, 0, &info), Int32(KRUN_INPUT_ERR_METHOD_UNSUPPORTED))
            XCTAssertEqual(config.vtable.query_abs_info!(configPtr, 2, &info), Int32(KRUN_INPUT_ERR_METHOD_UNSUPPORTED))
            let properties = bytes.withUnsafeMutableBufferPointer {
                config.vtable.query_properties!(configPtr, $0.baseAddress, $0.count)
            }
            XCTAssertEqual(properties, 0)
            var event = krun_input_event()
            XCTAssertEqual(provider.vtable.next_event!(providerPtr, &event), 0)
        }
    }

    @MainActor
    func testRelativeMouseCapabilitiesAndPointerFrames() throws {
        let input = try VMInput()
        func capabilities(_ device: VMInputDevice) throws -> [Int: Set<Int>] {
            let config = device.makeConfig()
            var instance: UnsafeMutableRawPointer?
            XCTAssertEqual(config.create!(&instance, config.create_userdata, nil), 0)
            let ptr = try XCTUnwrap(instance)
            defer { XCTAssertEqual(config.vtable.destroy!(ptr), 0) }
            var result: [Int: Set<Int>] = [:]
            for type in 1...31 {
                var bytes = [UInt8](repeating: 0, count: 128)
                let length = bytes.withUnsafeMutableBufferPointer {
                    config.vtable.query_event_capabilities!(ptr, UInt8(type), $0.baseAddress, $0.count)
                }
                XCTAssertGreaterThanOrEqual(length, 0)
                if length > 0 {
                    var codes: Set<Int> = []
                    for index in 0..<Int(length) {
                        for bit in 0..<8 where bytes[index] & UInt8(1 << bit) != 0 {
                            codes.insert(index * 8 + bit)
                        }
                    }
                    result[type] = codes
                }
            }
            return result
        }
        let mouse = try capabilities(input.mouse)
        XCTAssertNil(mouse[3])
        XCTAssertNil(mouse[4])
        XCTAssertEqual(mouse[1], Set([272, 273, 274, 275, 276]))
        XCTAssertEqual(mouse[2], Set([0, 1, 6, 8]))

        let syn = VMInputEvent(0, 0, 0)
        input.setActive(true)
        for focus in ["focus steam 1920x1200", "focus desktop 1280x800"] {
            input.updateGuestFocus(focus)
            input.move(deltaX: 13, deltaY: -9)
            input.button(0, down: true)
            input.scroll(deltaX: 1, deltaY: 1, precise: false)
            input.button(0, down: false)
            var events: [VMInputEvent] = []
            while let event = input.mouse.nextEvent() { events.append(event) }
            XCTAssertEqual(events, [VMInputEvent(2, 0, 13), VMInputEvent(2, 1, -9), syn,
                                    VMInputEvent(1, 272, 1), syn,
                                    VMInputEvent(2, 6, -1), VMInputEvent(2, 8, 1), syn,
                                    VMInputEvent(1, 272, 0), syn])
            for event in events where event.type != 0 {
                XCTAssertTrue(mouse[Int(event.type)]?.contains(Int(event.code)) == true)
            }
        }
    }

    func testReadinessTracksFrames() throws {
        let device = try VMInputDevice(kind: .keyboard)
        let first = VMInputEvent(1, 30, 1), second = VMInputEvent(1, 31, 1)
        XCTAssertFalse(readable(device.readyFD))
        XCTAssertTrue(device.send([first, second]))
        XCTAssertTrue(readable(device.readyFD))
        XCTAssertEqual(device.nextEvent(), first)
        XCTAssertEqual(device.nextEvent(), second)
        XCTAssertEqual(device.nextEvent(), VMInputEvent(0, 0, 0))
        XCTAssertNil(device.nextEvent())
        XCTAssertFalse(readable(device.readyFD))
        XCTAssertTrue(device.send([VMInputEvent(1, 30, 0)]))
        XCTAssertTrue(readable(device.readyFD))
    }

    func testSaturationReleasesKeyboardAndMouse() throws {
        for (kind, code, keys) in [(VMInputDevice.Kind.keyboard, UInt16(30), VMInputDevice.supportedKeyboardCodes),
                                   (.mouse, UInt16(275), Array(UInt16(272)...UInt16(276)))] {
            let device = try VMInputDevice(kind: kind, capacity: 256)
            XCTAssertTrue(device.send([VMInputEvent(1, code, 1)]))
            XCTAssertEqual(device.nextEvent(), VMInputEvent(1, code, 1))
            XCTAssertEqual(device.nextEvent(), VMInputEvent(0, 0, 0))
            var accepted = true
            for _ in 0..<257 {
                accepted = device.send([VMInputEvent(1, code, 1)])
                if !accepted { break }
            }
            XCTAssertFalse(accepted)
            XCTAssertTrue(readable(device.readyFD))
            var events: [VMInputEvent] = []
            while let event = device.nextEvent() { events.append(event) }
            XCTAssertLessThanOrEqual(events.count, 256)
            XCTAssertEqual(events.last, VMInputEvent(0, 0, 0))
            for key in keys {
                XCTAssertEqual(events.last(where: { $0.type == 1 && $0.code == key })?.value, 0)
            }
            XCTAssertFalse(readable(device.readyFD))
        }
    }

    func testSaturationPreservesPendingCapsLockToggleParity() throws {
        for (toggles, consumeDown, expectedParity) in [(1, false, 1), (1, true, 0), (2, false, 0)] {
            let device = try VMInputDevice(kind: .keyboard, capacity: 256)
            for _ in 0..<toggles {
                XCTAssertTrue(device.send([VMInputEvent(1, 58, 1), VMInputEvent(1, 58, 0)]))
            }
            if consumeDown {
                XCTAssertEqual(device.nextEvent(), VMInputEvent(1, 58, 1))
            }
            var accepted = true
            for _ in 0..<257 {
                accepted = device.send([VMInputEvent(1, 30, 1)])
                if !accepted { break }
            }
            XCTAssertFalse(accepted)
            var capsEvents: [VMInputEvent] = []
            while let event = device.nextEvent() {
                if event.type == 1 && event.code == 58 { capsEvents.append(event) }
            }
            XCTAssertEqual(capsEvents.filter { $0.value == 1 }.count % 2, expectedParity)
            XCTAssertEqual(capsEvents.last?.value, 0)
        }
    }

    func testReleaseAllPreservesPendingCapsLockToggleWhenQueueIsFull() throws {
        let device = try VMInputDevice(kind: .keyboard, capacity: 256)
        XCTAssertTrue(device.send([VMInputEvent(1, 58, 1), VMInputEvent(1, 58, 0)]))
        for _ in 0..<60 {
            XCTAssertTrue(device.send([VMInputEvent(1, 30, 1)]))
        }
        device.releaseAll()
        var capsEvents: [VMInputEvent] = []
        while let event = device.nextEvent() {
            if event.type == 1 && event.code == 58 { capsEvents.append(event) }
        }
        XCTAssertEqual(capsEvents.filter { $0.value == 1 }.count, 1)
        XCTAssertEqual(capsEvents.last?.value, 0)
    }

    func testRepeatedEmptyTransitionsKeepWakeups() throws {
        let device = try VMInputDevice(kind: .keyboard)
        for _ in 0..<1000 {
            XCTAssertFalse(readable(device.readyFD))
            XCTAssertTrue(device.send([VMInputEvent(1, 30, 1)]))
            XCTAssertTrue(readable(device.readyFD))
            XCTAssertNotNil(device.nextEvent())
            XCTAssertTrue(readable(device.readyFD))
            XCTAssertEqual(device.nextEvent(), VMInputEvent(0, 0, 0))
            XCTAssertNil(device.nextEvent())
            XCTAssertFalse(readable(device.readyFD))
        }
    }
}
