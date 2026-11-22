import AppKit
import Foundation

/// Main-thread state for the virtio-input devices; AppKit owns mouse capture.
@MainActor
final class VMInput {
    let keyboard: VMInputDevice
    let mouse: VMInputDevice
    private(set) var active = false
    private var failed = false
    private var pressedKeys: Set<UInt16> = []
    private var pressedButtons: Set<UInt16> = []
    private var capsLock = false
    private var scrollX = 0.0
    private var scrollY = 0.0

    private enum GuestMode: Equatable {
        case gaming(UInt32?)
        case desktop
        case unavailable
    }
    private var guestMode: GuestMode = .gaming(nil)
    private var surfaceSize: NSSize?
    private var motionX = 0.0
    private var motionY = 0.0

    func updateGuestFocus(_ line: String) {
        let parts = line.split(whereSeparator: \.isWhitespace)
        guard parts.count >= 2, parts[0] == "focus" else { return }
        let mode: GuestMode
        let sizeIndex: Int
        switch parts[1] {
        case "steam":
            guard parts.count == 2 || parts.count == 3 else { return }
            mode = .gaming(nil)
            sizeIndex = 2
        case "game":
            guard (parts.count == 3 || parts.count == 4),
                  let appid = UInt32(parts[2]), appid > 0 else { return }
            mode = .gaming(appid)
            sizeIndex = 3
        case "desktop":
            guard parts.count == 2 || parts.count == 3 else { return }
            mode = parts.count == 2 ? .unavailable : .desktop
            sizeIndex = 2
        default:
            return
        }
        var size: NSSize?
        if parts.count > sizeIndex {
            let dimensions = parts[sizeIndex].split(separator: "x", omittingEmptySubsequences: false)
            guard dimensions.count == 2, let width = Int(dimensions[0]), let height = Int(dimensions[1]),
                  (1...65535).contains(width), (1...65535).contains(height) else { return }
            size = NSSize(width: width, height: height)
        }
        guard mode != guestMode || size != surfaceSize else { return }
        if !pressedButtons.isEmpty {
            mouse.releaseAll()
            pressedButtons.removeAll(keepingCapacity: true)
        }
        scrollX = 0
        scrollY = 0
        resetMotion()
        guestMode = mode
        surfaceSize = size
    }

    private func resetMotion() {
        motionX = 0
        motionY = 0
    }

    init() throws {
        keyboard = try VMInputDevice(kind: .keyboard)
        mouse = try VMInputDevice(kind: .mouse)
    }

    func attach(context: UInt32) throws {
        try keyboard.attach(context: context)
        try mouse.attach(context: context)
    }

    func setActive(_ value: Bool, modifiers: NSEvent.ModifierFlags = []) {
        if !value { failed = false }
        guard active != value, !value || !failed else { return }
        active = value
        if value {
            flagsChanged(code: .max, flags: modifiers)
        } else {
            releaseKeyboard()
            mouse.releaseAll()
            pressedButtons.removeAll(keepingCapacity: true)
            scrollX = 0
            scrollY = 0
            resetMotion()
        }
    }

    @discardableResult
    private func send(_ events: [VMInputEvent], to device: VMInputDevice) -> Bool {
        guard !events.isEmpty else { return true }
        guard device.send(events) else {
            active = false
            failed = true
            keyboard.releaseAll()
            mouse.releaseAll()
            pressedKeys.removeAll(keepingCapacity: true)
            pressedButtons.removeAll(keepingCapacity: true)
            scrollX = 0
            scrollY = 0
            resetMotion()
            report("BRINEWELL input error queue saturated; click the window to capture input again")
            return false
        }
        return true
    }

    private func releaseKeyboard() {
        keyboard.releaseAll()
        pressedKeys.removeAll(keepingCapacity: true)
    }

    @discardableResult
    func key(code: UInt16, down: Bool, modifiers: NSEvent.ModifierFlags = []) -> Bool {
        guard active else { return false }
        if modifiers.contains(.command) {
            if !pressedKeys.isEmpty { releaseKeyboard() }
            return false
        }
        guard let linux = Self.keyCodes[code] else { return false }
        if down {
            // EV_REP is advertised: the guest owns repeat timing.
            guard !pressedKeys.contains(linux) else { return true }
            pressedKeys.insert(linux)
            send([VMInputEvent(1, linux, 1)], to: keyboard)
        } else if pressedKeys.remove(linux) != nil {
            send([VMInputEvent(1, linux, 0)], to: keyboard)
        }
        return true
    }

    func flagsChanged(code: UInt16, flags: NSEvent.ModifierFlags) {
        guard active else { return }
        synchronizeCapsLock(flags)
        guard active else { return }
        if flags.contains(.command) {
            if !pressedKeys.isEmpty { releaseKeyboard() }
            return
        }
        var events: [VMInputEvent] = []
        for pair in Self.modifiers {
            let aggregate = flags.contains(pair.flag)
            let raw = flags.rawValue
            let hasDeviceBits = raw & (pair.leftMask | pair.rightMask) != 0
            for (mac, linux, mask) in [(pair.leftMac, pair.leftLinux, pair.leftMask),
                                       (pair.rightMac, pair.rightLinux, pair.rightMask)] {
                let wasDown = pressedKeys.contains(linux)
                let down: Bool
                if !aggregate { down = false }
                else if hasDeviceBits { down = raw & mask != 0 }
                else if code == .max { down = mac == pair.leftMac }
                else if code == mac { down = !wasDown }
                else { down = wasDown }
                if down != wasDown {
                    if down { pressedKeys.insert(linux) } else { pressedKeys.remove(linux) }
                    events.append(VMInputEvent(1, linux, down ? 1 : 0))
                }
            }
        }
        send(events, to: keyboard)
    }

    private func synchronizeCapsLock(_ flags: NSEvent.ModifierFlags) {
        let enabled = flags.contains(.capsLock)
        guard enabled != capsLock else { return }
        if send([VMInputEvent(1, 58, 1), VMInputEvent(1, 58, 0)], to: keyboard) {
            capsLock = enabled
        }
    }

    func move(deltaX: Double, deltaY: Double) {
        guard active, guestMode != .unavailable, deltaX.isFinite, deltaY.isFinite else { return }
        // Preserve subpixel trackpad motion without deriving deltas from cursor position.
        motionX += deltaX
        motionY += deltaY
        let x = Int32(max(Double(Int32.min), min(Double(Int32.max), motionX.rounded(.towardZero))))
        let y = Int32(max(Double(Int32.min), min(Double(Int32.max), motionY.rounded(.towardZero))))
        motionX -= Double(x)
        motionY -= Double(y)
        var events: [VMInputEvent] = []
        if x != 0 { events.append(VMInputEvent(2, 0, x)) }
        if y != 0 { events.append(VMInputEvent(2, 1, y)) }
        send(events, to: mouse)
    }

    func button(_ number: Int, down: Bool) {
        guard active, guestMode != .unavailable, (0...4).contains(number) else { return }
        let code = UInt16(272 + number)
        if down {
            guard pressedButtons.insert(code).inserted else { return }
        } else {
            guard pressedButtons.remove(code) != nil else { return }
        }
        send([VMInputEvent(1, code, down ? 1 : 0)], to: mouse)
    }

    func scroll(deltaX: Double, deltaY: Double, precise: Bool) {
        guard active, guestMode != .unavailable, deltaX.isFinite, deltaY.isFinite else { return }
        scrollX += deltaX / (precise ? 10 : 1)
        scrollY += deltaY / (precise ? 10 : 1)
        let x = Int32(max(-Double(Int32.max), min(Double(Int32.max), scrollX.rounded(.towardZero))))
        let y = Int32(max(Double(Int32.min), min(Double(Int32.max), scrollY.rounded(.towardZero))))
        scrollX -= Double(x)
        scrollY -= Double(y)
        var events: [VMInputEvent] = []
        if x != 0 { events.append(VMInputEvent(2, 6, -x)) }
        if y != 0 { events.append(VMInputEvent(2, 8, y)) }
        send(events, to: mouse)
    }

    private struct ModifierPair {
        let flag: NSEvent.ModifierFlags
        let leftMac: UInt16
        let rightMac: UInt16
        let leftLinux: UInt16
        let rightLinux: UInt16
        let leftMask: UInt
        let rightMask: UInt
    }
    private static let modifiers = [
        ModifierPair(flag: .shift, leftMac: 56, rightMac: 60, leftLinux: 42, rightLinux: 54, leftMask: 0x2, rightMask: 0x4),
        ModifierPair(flag: .control, leftMac: 59, rightMac: 62, leftLinux: 29, rightLinux: 97, leftMask: 0x1, rightMask: 0x2000),
        ModifierPair(flag: .option, leftMac: 58, rightMac: 61, leftLinux: 56, rightLinux: 100, leftMask: 0x20, rightMask: 0x40),
    ]

    // macOS virtual keycodes -> Linux evdev physical keys (guest selects its layout).
    static let keyCodes: [UInt16: UInt16] = [
        0:30, 1:31, 2:32, 3:33, 4:35, 5:34, 6:44, 7:45, 8:46, 9:47, 10:86, 11:48,
        12:16, 13:17, 14:18, 15:19, 16:21, 17:20, 18:2, 19:3, 20:4, 21:5, 22:7, 23:6,
        24:13, 25:10, 26:8, 27:12, 28:9, 29:11, 30:27, 31:24, 32:22, 33:26, 34:23, 35:25,
        36:28, 37:38, 38:36, 39:40, 40:37, 41:39, 42:43, 43:51, 44:53, 45:49, 46:50, 47:52,
        48:15, 49:57, 50:41, 51:14, 53:1,
        64:187, 65:83, 67:55, 69:78, 71:69, 75:98, 76:96, 78:74, 79:188, 80:189, 81:117,
        82:82, 83:79, 84:80, 85:81, 86:75, 87:76, 88:77, 89:71, 90:190, 91:72, 92:73,
        93:124, 94:89, 95:121, 96:63, 97:64, 98:65, 99:61, 100:66, 101:67, 102:94,
        103:87, 104:93, 105:183, 106:186, 107:184, 109:68, 111:88, 113:185,
        114:110, 115:102, 116:104, 117:111, 118:62, 119:107, 120:60, 121:109, 122:59,
        123:105, 124:106, 125:108, 126:103,
    ]
}
