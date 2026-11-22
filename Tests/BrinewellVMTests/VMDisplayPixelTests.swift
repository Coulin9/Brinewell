import AppKit
import CKrun
import Foundation
import Metal
import XCTest
@testable import BrinewellVMRuntime

final class VMDisplayPixelTests: XCTestCase {
    @MainActor
    func testFramebufferFormatsProduceExpectedPNGChannels() throws {
        guard MTLCreateSystemDefaultDevice() != nil else {
            throw XCTSkip("No Metal device is available")
        }
        let cases: [(name: String, format: UInt32, bytes: [UInt8], alpha: UInt8)] = [
            ("BGRA", UInt32(KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM), [0xaa, 0x66, 0x22, 0x80, 0x33, 0x99, 0xdd, 0xff], 0x80),
            ("BGRX", UInt32(KRUN_DISPLAY_FORMAT_B8G8R8X8_UNORM), [0xaa, 0x66, 0x22, 0x00, 0x33, 0x99, 0xdd, 0x00], 0xff),
            ("ARGB", UInt32(KRUN_DISPLAY_FORMAT_A8R8G8B8_UNORM), [0x80, 0x22, 0x66, 0xaa, 0xff, 0xdd, 0x99, 0x33], 0x80),
            ("XRGB", UInt32(KRUN_DISPLAY_FORMAT_X8R8G8B8_UNORM), [0x00, 0x22, 0x66, 0xaa, 0x00, 0xdd, 0x99, 0x33], 0xff),
            ("RGBA", UInt32(KRUN_DISPLAY_FORMAT_R8G8B8A8_UNORM), [0x22, 0x66, 0xaa, 0x80, 0xdd, 0x99, 0x33, 0xff], 0x80),
            ("XBGR", UInt32(KRUN_DISPLAY_FORMAT_X8B8G8R8_UNORM), [0x00, 0xaa, 0x66, 0x22, 0x00, 0x33, 0x99, 0xdd], 0xff),
            ("ABGR", UInt32(KRUN_DISPLAY_FORMAT_A8B8G8R8_UNORM), [0x80, 0xaa, 0x66, 0x22, 0xff, 0x33, 0x99, 0xdd], 0x80),
            ("RGBX", UInt32(KRUN_DISPLAY_FORMAT_R8G8B8X8_UNORM), [0x22, 0x66, 0xaa, 0x00, 0xdd, 0x99, 0x33, 0x00], 0xff),
        ]
        let display = try VMDisplay()
        let backend = display.makeBackend()
        let create = try XCTUnwrap(backend.create)
        let callbacks = backend.vtable.basic_framebuffer
        let configure = try XCTUnwrap(callbacks.configure_scanout)
        let allocate = try XCTUnwrap(callbacks.alloc_frame)
        let present = try XCTUnwrap(callbacks.present_frame)
        var instance: UnsafeMutableRawPointer?
        XCTAssertEqual(create(&instance, backend.create_userdata, nil), 0)
        let created = try XCTUnwrap(instance)
        defer {
            if let destroy = callbacks.destroy {
                XCTAssertEqual(destroy(created), 0)
            }
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        for test in cases {
            XCTAssertEqual(configure(created, 0, 2, 1, 2, 1, test.format), 0, test.name)
            var buffer: UnsafeMutablePointer<UInt8>?
            var size = 0
            let frame = allocate(created, 0, &buffer, &size)
            XCTAssertGreaterThan(frame, 0, test.name)
            XCTAssertEqual(size, 8, test.name)
            guard frame > 0, size == 8 else { continue }
            let pixels = try XCTUnwrap(buffer, test.name)
            for (index, byte) in test.bytes.enumerated() { pixels[index] = byte }
            XCTAssertEqual(present(created, 0, UInt32(frame), nil), 0, test.name)
            let path = directory.appendingPathComponent("\(test.name).png")
            try display.dump(to: path.path)
            let image = try XCTUnwrap(NSBitmapImageRep(data: Data(contentsOf: path)), test.name)
            XCTAssertEqual(image.pixelsWide, 2, test.name)
            XCTAssertEqual(image.pixelsHigh, 1, test.name)
            let expected: [(UInt8, UInt8, UInt8, UInt8)] = [
                (0x22, 0x66, 0xaa, test.alpha), (0xdd, 0x99, 0x33, 0xff),
            ]
            for (x, channels) in expected.enumerated() {
                // Compare stored samples, not display-profile-converted NSColor values.
                var pixel = [UInt](repeating: 0, count: 4)
                image.getPixel(&pixel, atX: x, y: 0)
                XCTAssertEqual(pixel[0], UInt(channels.0), "\(test.name) pixel \(x) red")
                XCTAssertEqual(pixel[1], UInt(channels.1), "\(test.name) pixel \(x) green")
                XCTAssertEqual(pixel[2], UInt(channels.2), "\(test.name) pixel \(x) blue")
                XCTAssertEqual(pixel[3], UInt(channels.3), "\(test.name) pixel \(x) alpha")
            }
        }
    }
}
