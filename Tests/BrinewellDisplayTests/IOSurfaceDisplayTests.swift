import Foundation
import IOSurface
import Metal
import XCTest
@testable import BrinewellDisplay

private final class ReleaseCounter: @unchecked Sendable {
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

final class IOSurfaceDisplayTests: XCTestCase {
    private func surface(format: UInt32 = 0x42475241) throws -> IOSurfaceRef {
        let properties: [String: Any] = [
            kIOSurfaceWidth as String: 32,
            kIOSurfaceHeight as String: 16,
            kIOSurfaceBytesPerElement as String: 4,
            kIOSurfaceBytesPerRow as String: 128,
            kIOSurfaceAllocSize as String: 2048,
            kIOSurfacePixelFormat as String: format,
        ]
        return try XCTUnwrap(IOSurfaceCreate(properties as CFDictionary))
    }

    func testRejectsUnsupportedPixelFormat() throws {
        XCTAssertThrowsError(try IOSurfaceFrame(surface: surface(format: 0x52474241), sequenceNumber: 0))
    }

    func testRejectsPlanarSurface() throws {
        let plane: [String: Any] = [
            kIOSurfacePlaneWidth as String: 32,
            kIOSurfacePlaneHeight as String: 16,
            kIOSurfacePlaneBytesPerElement as String: 4,
            kIOSurfacePlaneBytesPerRow as String: 128,
            kIOSurfacePlaneOffset as String: 0,
            kIOSurfacePlaneSize as String: 2048,
        ]
        let properties: [String: Any] = [
            kIOSurfaceWidth as String: 32,
            kIOSurfaceHeight as String: 16,
            kIOSurfaceBytesPerElement as String: 4,
            kIOSurfaceBytesPerRow as String: 128,
            kIOSurfaceAllocSize as String: 2048,
            kIOSurfacePixelFormat as String: 0x42475241,
            kIOSurfacePlaneInfo as String: [plane],
        ]
        guard let planar = IOSurfaceCreate(properties as CFDictionary),
              IOSurfaceGetPlaneCount(planar) > 0 else {
            throw XCTSkip("This system did not create the requested planar IOSurface")
        }
        XCTAssertThrowsError(try IOSurfaceFrame(surface: planar, sequenceNumber: 0))
    }

    func testRejectsInvalidDimensionsWhenSurfaceCanBeCreated() throws {
        let properties: [String: Any] = [
            kIOSurfaceWidth as String: 0,
            kIOSurfaceHeight as String: 16,
            kIOSurfaceBytesPerElement as String: 4,
            kIOSurfaceBytesPerRow as String: 128,
            kIOSurfaceAllocSize as String: 2048,
            kIOSurfacePixelFormat as String: 0x42475241,
        ]
        guard let invalid = IOSurfaceCreate(properties as CFDictionary) else {
            throw XCTSkip("IOSurfaceCreate rejected zero width before frame validation")
        }
        XCTAssertThrowsError(try IOSurfaceFrame(surface: invalid, sequenceNumber: 0))
    }

    func testReleaseCallbackWaitsForFinalReference() throws {
        let counter = ReleaseCounter()
        var first: IOSurfaceFrame? = try IOSurfaceFrame(surface: surface(), sequenceNumber: 1) {
            counter.increment()
        }
        var second = first
        first = nil
        XCTAssertEqual(counter.value, 0)
        XCTAssertNotNil(second)
        second = nil
        XCTAssertEqual(counter.value, 1)
    }

    @MainActor
    func testPresentBindsSurfaceAndClearReleasesLease() throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("No Metal device is available")
        }
        let renderer = try IOSurfaceRenderer(device: device)
        let counter = ReleaseCounter()
        var frame: IOSurfaceFrame? = try IOSurfaceFrame(surface: surface(), sequenceNumber: 1) {
            counter.increment()
        }
        try renderer.present(.surface(try XCTUnwrap(frame)))
        frame = nil
        XCTAssertEqual(counter.value, 0)
        XCTAssertNil(renderer.lastError)
        renderer.clear()
        XCTAssertEqual(counter.value, 1)
    }
}
