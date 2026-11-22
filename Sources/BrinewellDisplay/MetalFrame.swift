import Foundation
import Metal

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
        onRelease: (@Sendable () -> Void)? = nil
    ) {
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
