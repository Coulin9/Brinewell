import CKrun
import Darwin
import Foundation

struct VMInputEvent: Equatable, Sendable {
    let type: UInt16
    let code: UInt16
    let value: Int32

    init(_ type: UInt16, _ code: UInt16, _ value: Int32) {
        self.type = type
        self.code = code
        self.value = value
    }
}

final class VMInputDevice: @unchecked Sendable {
    enum Kind { case keyboard, mouse }
    static let supportedKeyboardCodes: [UInt16] = Array(1...127) + Array(183...194)
    private static let mouseCodes: [UInt16] = Array(272...276)
    private static let syn = krun_input_event(type: 0, code: 0, value: 0)

    private let kind: Kind
    private let keys: [UInt16]
    private let lock = NSLock()
    private var ring: [krun_input_event]
    private var head = 0
    private var count = 0
    let readyFD: Int32
    private let writeFD: Int32

    init(kind: Kind, capacity: Int = 4096) throws {
        let keys: [UInt16]
        switch kind {
        case .keyboard: keys = Self.supportedKeyboardCodes
        case .mouse: keys = Self.mouseCodes
        }
        guard capacity >= keys.count + (kind == .keyboard ? 4 : 1) else {
            throw NSError(domain: "Brinewell.VMInput", code: Int(EINVAL), userInfo: [NSLocalizedDescriptionKey: "Input capacity cannot hold a release frame"])
        }
        var fds: [Int32] = [-1, -1]
        guard pipe(&fds) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
        }
        for fd in fds {
            let flags = fcntl(fd, F_GETFL)
            guard flags >= 0, fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
                  fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 else {
                let code = errno
                Darwin.close(fds[0]); Darwin.close(fds[1])
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(code))
            }
        }
        self.kind = kind
        self.keys = keys
        self.ring = Array(repeating: Self.syn, count: capacity)
        self.readyFD = fds[0]
        self.writeFD = fds[1]
    }

    deinit {
        Darwin.close(readyFD)
        Darwin.close(writeFD)
    }

    func attach(context: UInt32) throws {
        var config = makeConfig()
        var provider = makeProvider()
        let result = krun_add_input_device(context, &config, MemoryLayout<krun_input_config>.size,
                                           &provider, MemoryLayout<krun_input_event_provider>.size)
        guard result >= 0 else {
            throw NSError(domain: "Brinewell.VMInput", code: Int(result), userInfo: [NSLocalizedDescriptionKey: "krun_add_input_device failed: \(result)"])
        }
    }

    func send(_ events: [VMInputEvent]) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard events.count < ring.count - count else {
            rebuildReleaseFrame()
            return false
        }
        let wasEmpty = count == 0
        for event in events {
            push(krun_input_event(type: event.type, code: event.code, value: UInt32(bitPattern: event.value)))
        }
        push(Self.syn)
        if wasEmpty { signal() }
        return true
    }

    func releaseAll() {
        lock.lock(); defer { lock.unlock() }
        guard keys.count + 1 <= ring.count - count else {
            rebuildReleaseFrame()
            return
        }
        let wasEmpty = count == 0
        appendReleaseFrame()
        if wasEmpty { signal() }
    }

    func nextEvent() -> VMInputEvent? {
        lock.lock(); defer { lock.unlock() }
        guard count > 0 else { return nil }
        let event = ring[head]
        head = (head + 1) % ring.count
        count -= 1
        if count == 0 {
            var byte: UInt8 = 0
            while Darwin.read(readyFD, &byte, 1) < 0 && errno == EINTR {}
        }
        return VMInputEvent(event.type, event.code, Int32(bitPattern: event.value))
    }

    private func push(_ event: krun_input_event) {
        ring[(head + count) % ring.count] = event
        count += 1
    }

    private func appendReleaseFrame() {
        for key in keys { push(krun_input_event(type: 1, code: key, value: 0)) }
        push(Self.syn)
    }

    private func rebuildReleaseFrame() {
        let wasEmpty = count == 0
        var pendingCapsToggle = false
        if kind == .keyboard {
            for offset in 0..<count {
                let event = ring[(head + offset) % ring.count]
                if event.type == 1 && event.code == 58 && event.value == 1 {
                    pendingCapsToggle.toggle()
                }
            }
        }
        head = 0
        count = 0
        appendReleaseFrame()
        if pendingCapsToggle {
            push(krun_input_event(type: 1, code: 58, value: 1))
            push(krun_input_event(type: 1, code: 58, value: 0))
            push(Self.syn)
        }
        if wasEmpty { signal() }
    }

    // Only poll the read end externally; the queue owns the notification byte.
    private func signal() {
        var byte: UInt8 = 1
        while Darwin.write(writeFD, &byte, 1) < 0 && errno == EINTR {}
    }

    private static func device(_ instance: UnsafeMutableRawPointer) -> VMInputDevice {
        Unmanaged<VMInputDevice>.fromOpaque(instance).takeUnretainedValue()
    }

    private static let create: krun_input_create_fn = { instance, userdata, _ in
        guard let instance, let userdata else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
        let device = Unmanaged<VMInputDevice>.fromOpaque(userdata).takeUnretainedValue()
        instance.pointee = Unmanaged.passRetained(device).toOpaque()
        return 0
    }
    private static let destroy: krun_input_destroy_fn = { instance in
        guard let instance else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
        Unmanaged<VMInputDevice>.fromOpaque(instance).release()
        return 0
    }
    private static func copyString(_ text: String, _ buffer: UnsafeMutablePointer<UInt8>?, _ length: Int) -> Int32 {
        guard let buffer else {
            return length == 0 ? 0 : Int32(KRUN_INPUT_ERR_INVALID_PARAM)
        }
        let bytes = Array(text.utf8)
        let n = min(length, bytes.count)
        for i in 0..<n { buffer[i] = bytes[i] }
        return Int32(n)
    }

    private static func bitmap(_ codes: [UInt16], _ buffer: UnsafeMutablePointer<UInt8>?, _ length: Int) -> Int32 {
        guard let buffer else { return length == 0 ? 0 : Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
        buffer.initialize(repeating: 0, count: length)
        var used = 0
        for code in codes {
            let index = Int(code) / 8
            if index < length {
                buffer[index] |= UInt8(1 << (Int(code) % 8))
                used = max(used, index + 1)
            }
        }
        return Int32(used)
    }

    func makeConfig() -> krun_input_config {
        var config = krun_input_config()
        config.features = UInt64(KRUN_INPUT_CONFIG_FEATURE_QUERY)
        config.create_userdata = Unmanaged.passUnretained(self).toOpaque()
        config.create = VMInputDevice.create
        config.vtable.destroy = VMInputDevice.destroy
        config.vtable.query_device_name = { instance, buffer, length in
            guard let instance else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            let device = VMInputDevice.device(instance)
            let name: String
            switch device.kind {
            case .keyboard: name = "Brinewell Keyboard"
            case .mouse: name = "Brinewell Mouse"
            }
            return VMInputDevice.copyString(name, buffer, length)
        }
        config.vtable.query_serial_name = { instance, buffer, length in
            guard let instance else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            let device = VMInputDevice.device(instance)
            let name: String
            switch device.kind {
            case .keyboard: name = "brinewell-keyboard"
            case .mouse: name = "brinewell-mouse"
            }
            return VMInputDevice.copyString(name, buffer, length)
        }
        config.vtable.query_device_ids = { instance, ids in
            guard let instance, let ids else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            let device = VMInputDevice.device(instance)
            let product: UInt16
            switch device.kind {
            case .keyboard: product = 1
            case .mouse: product = 3
            }
            ids.pointee = krun_input_device_ids(bustype: 6, vendor: 0x1d6b, product: product, version: 1)
            return 0
        }
        config.vtable.query_event_capabilities = { instance, type, buffer, length in
            guard let instance else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            let device = VMInputDevice.device(instance)
            let codes: [UInt16]
            switch (device.kind, type) {
            case (_, 1): codes = device.keys
            case (.keyboard, 20): codes = [0, 1]
            case (.mouse, 2): codes = [0, 1, 6, 8]
            default: codes = []
            }
            return VMInputDevice.bitmap(codes, buffer, length)
        }
        config.vtable.query_abs_info = { instance, _, info in
            guard instance != nil, info != nil else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            return Int32(KRUN_INPUT_ERR_METHOD_UNSUPPORTED)
        }
        config.vtable.query_properties = { instance, buffer, length in
            guard instance != nil else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            return VMInputDevice.bitmap([], buffer, length)
        }
        return config
    }

    func makeProvider() -> krun_input_event_provider {
        var provider = krun_input_event_provider()
        provider.features = UInt64(KRUN_INPUT_EVENT_PROVIDER_FEATURE_QUEUE)
        provider.create_userdata = Unmanaged.passUnretained(self).toOpaque()
        provider.create = VMInputDevice.create
        provider.vtable.destroy = VMInputDevice.destroy
        provider.vtable.get_ready_efd = { instance in
            guard let instance else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            return VMInputDevice.device(instance).readyFD
        }
        provider.vtable.next_event = { instance, output in
            guard let instance, let output else { return Int32(KRUN_INPUT_ERR_INVALID_PARAM) }
            guard let event = VMInputDevice.device(instance).nextEvent() else { return 0 }
            output.pointee = krun_input_event(type: event.type, code: event.code, value: UInt32(bitPattern: event.value))
            return 1
        }
        return provider
    }
}
