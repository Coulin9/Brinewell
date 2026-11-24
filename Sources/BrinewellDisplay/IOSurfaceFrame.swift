import Foundation
import IOSurface

/// A lease on a non-planar BGRA IOSurface. The producer must not write to or
/// reuse the surface until the last reference to this frame has been released.
/// `onRelease` may run on any thread, including a Metal completion thread.
public final class IOSurfaceFrame: @unchecked Sendable {
    public let colorState: DisplayColorState
    public let surface: IOSurfaceRef
    public let sequenceNumber: UInt64
    private let onRelease: (@Sendable () -> Void)?

    public init(
        surface: IOSurfaceRef,
        sequenceNumber: UInt64,
        colorState: DisplayColorState = .sdr,
        onRelease: (@Sendable () -> Void)? = nil
    ) throws {
        let width = IOSurfaceGetWidth(surface)
        let height = IOSurfaceGetHeight(surface)
        let bytesPerElement = IOSurfaceGetBytesPerElement(surface)
        let (minimumStride, overflow) = width.multipliedReportingOverflow(by: 4)
        guard IOSurfaceGetPlaneCount(surface) == 0,
              IOSurfaceGetPixelFormat(surface) == 0x42475241,
              bytesPerElement == 4,
              width > 0, height > 0,
              !overflow, IOSurfaceGetBytesPerRow(surface) >= minimumStride else {
            throw DisplayError.invalidSurface
        }
        self.colorState = colorState
        self.surface = surface
        self.sequenceNumber = sequenceNumber
        self.onRelease = onRelease
    }

    deinit {
        onRelease?()
    }
}

public enum DisplayError: LocalizedError {
    case invalidSurface
    case noMetalDevice
    case commandQueueUnavailable
    case shaderLibrary(String)
    case pipeline(String)
    case textureUnavailable
    case commandBufferUnavailable
    case renderEncoderUnavailable
    case gpu(String)

    public var errorDescription: String? {
        switch self {
        case .invalidSurface: "IOSurface must be non-planar BGRA with four-byte pixels, positive dimensions, and a sufficient row stride."
        case .noMetalDevice: "No Metal device is available."
        case .commandQueueUnavailable: "Unable to create a Metal command queue."
        case .shaderLibrary(let detail): "Unable to create the Metal shader library: \(detail)"
        case .pipeline(let detail): "Unable to create the Metal render pipeline: \(detail)"
        case .textureUnavailable: "Unable to bind the IOSurface as a Metal texture."
        case .commandBufferUnavailable: "Unable to create a Metal command buffer."
        case .renderEncoderUnavailable: "Unable to create a Metal render encoder."
        case .gpu(let detail): "Metal rendering failed: \(detail)"
        }
    }
}
