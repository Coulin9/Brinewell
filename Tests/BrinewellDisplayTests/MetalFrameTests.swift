import Metal
import XCTest
@testable import BrinewellDisplay

private final class MetalReleaseCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0

    func increment() {
        lock.lock()
        count += 1
        lock.unlock()
    }

    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }
}

final class MetalFrameTests: XCTestCase {
    private func makeTexture(
        device: any MTLDevice,
        width: Int,
        height: Int,
        pixelFormat: MTLPixelFormat = .bgra8Unorm,
        pixels: [UInt8]
    ) throws -> any MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: pixelFormat, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.shaderRead, .shaderWrite]
        descriptor.storageMode = .shared
        let texture = try XCTUnwrap(device.makeTexture(descriptor: descriptor))
        try pixels.withUnsafeBytes { buffer in
            guard let base = buffer.baseAddress else { throw XCTSkip("Empty pixel buffer") }
            texture.replace(
                region: MTLRegionMake2D(0, 0, width, height),
                mipmapLevel: 0,
                withBytes: base,
                bytesPerRow: width * 4
            )
        }
        return texture
    }

    func testReleaseRunsExactlyOnce() throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("No Metal device is available")
        }
        let texture = try makeTexture(device: device, width: 1, height: 1, pixels: [0, 0, 0, 255])
        let counter = MetalReleaseCounter()
        var frame: MetalFrame? = MetalFrame(
            texture: texture, sequenceNumber: 7, width: 1, height: 1
        ) {
            counter.increment()
        }
        XCTAssertEqual(frame?.sequenceNumber, 7)
        frame?.finish()
        frame?.finish()
        XCTAssertEqual(counter.value, 1)
        frame = nil
        XCTAssertEqual(counter.value, 1)
    }

    @MainActor
    func testNativeFramePresentsAndClears() throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("No Metal device is available")
        }
        let renderer = try IOSurfaceRenderer(device: device)
        let texture = try makeTexture(device: device, width: 2, height: 2, pixels: [UInt8](repeating: 0, count: 16))
        let counter = MetalReleaseCounter()
        var frame: MetalFrame? = MetalFrame(
            texture: texture, sequenceNumber: 1, width: 2, height: 2
        ) {
            counter.increment()
        }
        try renderer.present(.metal(try XCTUnwrap(frame)))
        XCTAssertNil(renderer.lastError)
        frame = nil
        XCTAssertEqual(counter.value, 0)
        renderer.clear()
        XCTAssertEqual(counter.value, 1)
    }

    @MainActor
    func testReadbackReturnsNativeTexturePixels() throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("No Metal device is available")
        }
        let pixels: [UInt8] = [0, 0, 255, 255, 0, 255, 0, 255]
        let texture = try makeTexture(device: device, width: 2, height: 1, pixels: pixels)
        let renderer = try IOSurfaceRenderer(device: device)
        let frame = MetalFrame(texture: texture, sequenceNumber: 3, width: 2, height: 1)
        try renderer.present(.metal(frame))
        let snapshot = try renderer.readback()
        XCTAssertEqual(snapshot.width, 2)
        XCTAssertEqual(snapshot.height, 1)
        XCTAssertTrue(snapshot.swapRedBlue)
        XCTAssertEqual(snapshot.pixels, pixels)
    }
}
