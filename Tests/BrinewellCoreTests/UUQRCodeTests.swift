import CoreImage.CIFilterBuiltins
import XCTest
@testable import BrinewellCore

final class UUQRCodeTests: XCTestCase {
    func testDecodesTerminalHalfBlocksWithoutChangingBindingCode() throws {
        let identifier = "0123456789abcdef0123456789abcdef"
        let terminal = try terminalQR(identifier)
        XCTAssertEqual(UUQRCode.bindingCode(fromTerminalData: terminal), identifier)
    }

    func testRejectsUnsupportedQRCodesAndMalformedTerminalData() throws {
        XCTAssertNil(UUQRCode.bindingCode(fromTerminalData: try terminalQR("https://example.com")))
        XCTAssertNil(UUQRCode.bindingCode(fromTerminalData: Data("not a QR code".utf8)))
        XCTAssertNil(UUQRCode.bindingCode(fromTerminalData: Data(repeating: 0x20, count: 16_385)))
    }

    private func terminalQR(_ payload: String) throws -> Data {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(payload.utf8)
        filter.correctionLevel = "M"
        let image = try XCTUnwrap(filter.outputImage)
        let width = Int(image.extent.width), height = Int(image.extent.height)
        var pixels = [UInt8](repeating: 255, count: width * height)
        CIContext(options: [.useSoftwareRenderer: true]).render(
            image, toBitmap: &pixels, rowBytes: width, bounds: image.extent,
            format: .L8, colorSpace: CGColorSpaceCreateDeviceGray()
        )
        var text = ""
        for y in stride(from: 0, to: height, by: 2) {
            for x in 0..<width {
                let top = pixels[y * width + x] < 128
                let bottom = y + 1 < height && pixels[(y + 1) * width + x] < 128
                switch (top, bottom) {
                case (true, true): text += "█"
                case (true, false): text += "▀"
                case (false, true): text += "▄"
                case (false, false): text += " "
                }
            }
            text += "\n"
        }
        return Data(text.utf8)
    }
}
