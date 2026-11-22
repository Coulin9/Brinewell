import AppKit

@MainActor
final class VMClipboard {
    private static let maxTextBytes = 64 * 1024
    // FE and FF cannot occur in valid UTF-8. A start byte also discards any
    // incomplete frame left in the console when the desktop session exits.
    private static let start: UInt8 = 0xff
    private static let end: UInt8 = 0xfe
    private static let text: UInt8 = 0x54 // T
    private static let ready: UInt8 = 0x52 // R
    private static let disconnect: UInt8 = 0x44 // D

    private let pasteboard: NSPasteboard
    private let send: ([UInt8]) -> Void
    private var active = false
    private var guestReady = false
    private var changeCount: Int?
    private var frame: [UInt8]?

    init(pasteboard: NSPasteboard = .general, send: @escaping ([UInt8]) -> Void) {
        self.pasteboard = pasteboard
        self.send = send
    }

    func setActive(_ active: Bool) {
        guard self.active != active else { return }
        self.active = active
        if active { sendHostText() }
    }

    private func sendHostText() {
        guard guestReady, active else { return }
        let count = pasteboard.changeCount
        guard changeCount != count else { return }
        changeCount = count
        guard let text = pasteboard.string(forType: .string),
              (1...Self.maxTextBytes).contains(text.utf8.count) else { return }
        send([Self.start, Self.text] + text.utf8 + [Self.end])
    }

    func receive(_ bytes: [UInt8]) {
        for byte in bytes {
            switch byte {
            case Self.start:
                frame = []
            case Self.end:
                if let complete = frame { receiveFrame(complete) }
                frame = nil
            default:
                if let count = frame?.count, count <= Self.maxTextBytes {
                    frame?.append(byte)
                } else {
                    frame = nil
                }
            }
        }
    }

    private func receiveFrame(_ frame: [UInt8]) {
        if frame == [Self.ready] {
            guestReady = true
            changeCount = nil
            sendHostText()
        } else if frame == [Self.disconnect] {
            guestReady = false
        } else if guestReady, frame.first == Self.text, frame.count > 1,
                  let text = String(bytes: frame.dropFirst(), encoding: .utf8) {
            // A delayed guest update must not overwrite a more recent copy
            // made in another Mac window after the VM lost focus.
            guard active || pasteboard.changeCount == changeCount else { return }
            pasteboard.clearContents()
            pasteboard.setString(text, forType: .string)
            changeCount = pasteboard.changeCount
        }
    }
}
