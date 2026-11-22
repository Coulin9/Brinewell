import AppKit
import XCTest
@testable import BrinewellVMRuntime

final class VMClipboardTests: XCTestCase {
    private func frame(_ message: String) -> [UInt8] { [0xff] + Array(message.utf8) + [0xfe] }

    @MainActor
    func testHostSendsOnFocusAndDesktopReconnect() {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        var sent: [[UInt8]] = []
        let clipboard = VMClipboard(pasteboard: pasteboard) { sent.append($0) }
        pasteboard.setString("一行\n第二行", forType: .string)

        clipboard.receive(frame("R"))
        XCTAssertTrue(sent.isEmpty)
        clipboard.setActive(true)
        XCTAssertEqual(sent, [frame("T一行\n第二行")])

        pasteboard.clearContents()
        pasteboard.setString("Mac updated", forType: .string)
        clipboard.setActive(true)
        XCTAssertEqual(sent.count, 1)
        clipboard.setActive(false)
        clipboard.setActive(true)
        XCTAssertEqual(sent.last, frame("TMac updated"))

        clipboard.receive(frame("D"))
        clipboard.setActive(false)
        clipboard.setActive(true)
        XCTAssertEqual(sent.count, 2)
        clipboard.receive(frame("R"))
        XCTAssertEqual(sent, [frame("T一行\n第二行"), frame("TMac updated"), frame("TMac updated")])
    }

    @MainActor
    func testGuestTextIsReassembledWithoutEchoOrOverwritingNewHostCopy() {
        let pasteboard = NSPasteboard.withUniqueName()
        defer { pasteboard.releaseGlobally() }
        var sent: [[UInt8]] = []
        let clipboard = VMClipboard(pasteboard: pasteboard) { sent.append($0) }
        clipboard.setActive(true)
        clipboard.receive(frame("R"))

        let message = frame("TSteamOS 中文\n🕹️")
        clipboard.receive([0xff, 0x54, 0x61] + message.prefix(5))
        XCTAssertNil(pasteboard.string(forType: .string))
        clipboard.receive(Array(message.dropFirst(5)))
        XCTAssertEqual(pasteboard.string(forType: .string), "SteamOS 中文\n🕹️")

        clipboard.setActive(false)
        clipboard.setActive(true)
        XCTAssertTrue(sent.isEmpty)

        clipboard.setActive(false)
        pasteboard.clearContents()
        pasteboard.setString("New Mac copy", forType: .string)
        clipboard.receive(frame("TLate guest frame"))
        XCTAssertEqual(pasteboard.string(forType: .string), "New Mac copy")
        clipboard.setActive(true)
        XCTAssertEqual(sent, [frame("TNew Mac copy")])
    }
}
