import Foundation
import Metal

public struct DisplayColorState: Equatable, Sendable {
    public var colorspace: UInt32 = 0
    public var eotf: UInt32 = 0
    public var flags: UInt32 = 0
    public var maxDisplayMasteringLuminance: UInt16 = 0
    public var minDisplayMasteringLuminance: UInt16 = 0
    public var maxCll: UInt16 = 0
    public var maxFall: UInt16 = 0

    public init(colorspace: UInt32 = 0, eotf: UInt32 = 0, flags: UInt32 = 0,
                maxDisplayMasteringLuminance: UInt16 = 0, minDisplayMasteringLuminance: UInt16 = 0,
                maxCll: UInt16 = 0, maxFall: UInt16 = 0) {
        self.colorspace = colorspace; self.eotf = eotf; self.flags = flags
        self.maxDisplayMasteringLuminance = maxDisplayMasteringLuminance
        self.minDisplayMasteringLuminance = minDisplayMasteringLuminance
        self.maxCll = maxCll; self.maxFall = maxFall
    }
    public static let sdr = DisplayColorState()
    public var hasHdrMetadata: Bool { flags & 1 != 0 }
    public var isPQ: Bool { eotf == 2 }
    public var isHLG: Bool { eotf == 3 }
}

/// A lease on a native Metal scanout texture exported by libkrun/virglrenderer.
///
/// The producer keeps the underlying GPU resource and its backing memory alive
/// until `finish` runs (or the frame is deallocated); `finish` then tells
/// libkrun that the display no longer reads it. The frame holds its own strong
/// reference to the texture, so the resource stays valid even if libkrun drops
/// its reference first.
///
/// `finish` is idempotent and may run on any thread, including a Metal
/// completion thread.
public final class MetalFrame: @unchecked Sendable {
    public let colorState: DisplayColorState
    public let texture: any MTLTexture
    public let sequenceNumber: UInt64
    public let width: Int
    public let height: Int

    private let onRelease: (@Sendable () -> Void)?
    private let lock = NSLock()
    private var released = false

    public init(
        texture: any MTLTexture,
        sequenceNumber: UInt64,
        width: Int,
        height: Int,
        colorState: DisplayColorState = .sdr,
        onRelease: (@Sendable () -> Void)? = nil
    ) {
        self.colorState = colorState
        self.texture = texture
        self.sequenceNumber = sequenceNumber
        self.width = width
        self.height = height
        self.onRelease = onRelease
    }

    /// Releases the producer's frame exactly once.
    public func finish() {
        lock.lock()
        let release = released ? nil : onRelease
        released = true
        lock.unlock()
        release?()
    }

    deinit {
        finish()
    }
}
