import AppKit
import Foundation
import IOSurface
import Metal
@preconcurrency import MetalKit
import SwiftUI

/// A frame the renderer can display.
///
/// `.surface` carries a CPU-visible BGRA IOSurface (the virtio-gpu fallback
/// path). `.metal` carries a native GPU texture handed over by libkrun, which
/// the renderer samples directly with no CPU pixel copy.
public enum DisplayFrame: Sendable {
    case surface(IOSurfaceFrame)
    case metal(MetalFrame)

    public var pixelSize: (width: Int, height: Int) {
        switch self {
        case .surface(let frame):
            (IOSurfaceGetWidth(frame.surface), IOSurfaceGetHeight(frame.surface))
        case .metal(let frame):
            (frame.width, frame.height)
        }
    }

    public var sequenceNumber: UInt64 {
        switch self {
        case .surface(let frame): frame.sequenceNumber
        case .metal(let frame): frame.sequenceNumber
        }
    }

    public var colorState: DisplayColorState {
        switch self {
        case .surface(let frame): frame.colorState
        case .metal(let frame): frame.colorState
        }
    }

    /// Marks the frame as no longer read by the GPU. Native frames release
    /// their producer lease here; the texture stays valid through this frame's
    /// own strong reference until the frame is deallocated.
    public func finish() {
        if case .metal(let frame) = self {
            frame.finish()
        }
    }
}

/// A CPU-readable copy of the frame currently bound to the renderer.
public struct FrameSnapshot {
    public let width: Int
    public let height: Int
    /// True when the pixels are BGRA and the caller must swap red and blue.
    public let swapRedBlue: Bool
    public let pixels: [UInt8]

    public init(width: Int, height: Int, swapRedBlue: Bool, pixels: [UInt8]) {
        self.width = width
        self.height = height
        self.swapRedBlue = swapRedBlue
        self.pixels = pixels
    }
}

/// Owns one display view at a time. Keep the renderer alive for the view's
/// lifetime; do not attach the same renderer to multiple views concurrently.
@MainActor
public final class IOSurfaceRenderer: NSObject, MTKViewDelegate {
    public private(set) var lastError: String?

    private final class MetalLease: @unchecked Sendable {
        let frame: MetalFrame
        private let lock = NSLock()
        private var inFlightDraws = 0
        private var retired = false

        init(_ frame: MetalFrame) { self.frame = frame }
        deinit { frame.finish() }

        func beginDraw() {
            lock.lock()
            inFlightDraws += 1
            lock.unlock()
        }

        func endDraw() {
            lock.lock()
            inFlightDraws -= 1
            let shouldFinish = retired && inFlightDraws == 0
            lock.unlock()
            if shouldFinish { frame.finish() }
        }

        func retire() {
            lock.lock()
            retired = true
            let shouldFinish = inFlightDraws == 0
            lock.unlock()
            if shouldFinish { frame.finish() }
        }
    }

    private let device: any MTLDevice
    private let queue: any MTLCommandQueue
    private let pipeline: any MTLRenderPipelineState
    private let textureDescriptor: MTLTextureDescriptor
    private weak var view: MTKView?
    private var frame: DisplayFrame?
    private var currentMetalLease: MetalLease?
    private var texture: (any MTLTexture)?
    private var appliedColorSpaceName: CFString?
    private var appliedEDR: Bool?

    public init(device: (any MTLDevice)? = MTLCreateSystemDefaultDevice()) throws {
        guard let device else {
            throw DisplayError.noMetalDevice
        }
        guard let queue = device.makeCommandQueue() else {
            throw DisplayError.commandQueueUnavailable
        }
        let library: any MTLLibrary
        do {
            library = try device.makeLibrary(source: Self.shaderSource, options: nil)
        } catch {
            throw DisplayError.shaderLibrary(error.localizedDescription)
        }
        guard let vertex = library.makeFunction(name: "displayVertex"),
              let fragment = library.makeFunction(name: "displayFragment") else {
            throw DisplayError.pipeline("Shader functions are unavailable.")
        }
        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = vertex
        pipelineDescriptor.fragmentFunction = fragment
        pipelineDescriptor.colorAttachments[0].pixelFormat = .rgba16Float
        let pipeline: any MTLRenderPipelineState
        do {
            pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        } catch {
            throw DisplayError.pipeline(error.localizedDescription)
        }
        let descriptor = MTLTextureDescriptor()
        descriptor.textureType = .type2D
        descriptor.pixelFormat = .bgra8Unorm
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        self.device = device
        self.queue = queue
        self.pipeline = pipeline
        self.textureDescriptor = descriptor
        super.init()
    }

    /// Binds the supplied frame. For an IOSurface this creates a texture view
    /// over the surface; for a native frame the GPU texture is sampled as is.
    /// No pixel data is copied by the CPU in either case.
    public func present(_ frame: DisplayFrame) throws {
        switch frame {
        case .surface(let surfaceFrame):
            textureDescriptor.width = IOSurfaceGetWidth(surfaceFrame.surface)
            textureDescriptor.height = IOSurfaceGetHeight(surfaceFrame.surface)
            guard let texture = device.makeTexture(
                descriptor: textureDescriptor, iosurface: surfaceFrame.surface, plane: 0
            ) else {
                lastError = DisplayError.textureUnavailable.localizedDescription
                throw DisplayError.textureUnavailable
            }
            self.texture = texture
        case .metal(let metalFrame):
            self.texture = metalFrame.texture
        }
        let previousLease = currentMetalLease
        if case .metal(let metalFrame) = frame {
            if previousLease?.frame !== metalFrame {
                currentMetalLease = MetalLease(metalFrame)
                previousLease?.retire()
            }
        } else {
            currentMetalLease = nil
            previousLease?.retire()
        }
        self.frame = frame
        updateLayerColor()
        lastError = nil
        let size = frame.pixelSize
        FrameTimingLog.shared.log(
            "arrive", seq: frame.sequenceNumber, width: size.width, height: size.height,
            drawable: view?.drawableSize ?? .zero,
            fullScreen: view?.window?.styleMask.contains(.fullScreen)
        )
        view?.needsDisplay = true
    }

    /// Drops the current lease and requests a black frame. In-flight GPU work
    /// retains its own lease until its command buffer completes.
    public func clear() {
        currentMetalLease?.retire()
        currentMetalLease = nil
        frame = nil
        texture = nil
        updateLayerColor()
        lastError = nil
        view?.needsDisplay = true
    }

    /// Copies the currently bound frame into CPU-readable memory. This is a
    /// diagnostic path (frame dumps); the display path never calls it.
    public func readback() throws -> FrameSnapshot {
        guard let texture else {
            throw DisplayError.textureUnavailable
        }
        let width = texture.width
        let height = texture.height
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: texture.pixelFormat, width: width, height: height, mipmapped: false
        )
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        guard let destination = device.makeTexture(descriptor: descriptor) else {
            throw DisplayError.textureUnavailable
        }
        guard let commandBuffer = queue.makeCommandBuffer() else {
            throw DisplayError.commandBufferUnavailable
        }
        guard let blit = commandBuffer.makeBlitCommandEncoder() else {
            throw DisplayError.renderEncoderUnavailable
        }
        blit.copy(
            from: texture, sourceSlice: 0, sourceLevel: 0,
            sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: width, height: height, depth: 1),
            to: destination, destinationSlice: 0, destinationLevel: 0,
            destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0)
        )
        blit.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
        if let error = commandBuffer.error {
            throw DisplayError.gpu(error.localizedDescription)
        }
        switch texture.pixelFormat {
        case .rgba16Float, .rgb10a2Unorm, .bgr10a2Unorm, .rgba8Unorm, .rgba8Unorm_srgb, .bgra8Unorm, .bgra8Unorm_srgb: break
        default: throw DisplayError.textureUnavailable
        }
        let bytesPerPixel = texture.pixelFormat == .rgba16Float ? 8 : 4
        let bytesPerRow = width * bytesPerPixel
        var raw = [UInt8](repeating: 0, count: bytesPerRow * height)
        raw.withUnsafeMutableBytes { buffer in
            guard let base = buffer.baseAddress else { return }
            destination.getBytes(base, bytesPerRow: bytesPerRow,
                                 from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        }
        var pixels = [UInt8](repeating: 0, count: width * 4 * height)
        var swap = texture.pixelFormat == .bgra8Unorm || texture.pixelFormat == .bgra8Unorm_srgb
        switch texture.pixelFormat {
        case .rgb10a2Unorm, .bgr10a2Unorm:
            for i in 0..<(width * height) {
                let j = i * 4
                let word = UInt32(raw[j]) | UInt32(raw[j + 1]) << 8 | UInt32(raw[j + 2]) << 16 | UInt32(raw[j + 3]) << 24
                pixels[j] = UInt8(((word & 1023) * 255 + 511) / 1023)
                pixels[j + 1] = UInt8((((word >> 10) & UInt32(1023)) * UInt32(255) + UInt32(511)) / UInt32(1023))
                pixels[j + 2] = UInt8((((word >> 20) & UInt32(1023)) * UInt32(255) + UInt32(511)) / UInt32(1023))
                pixels[j + 3] = UInt8((word >> 30) * 85)
            }
            swap = texture.pixelFormat == .bgr10a2Unorm
        case .rgba16Float:
            for i in 0..<(width * height * 4) {
                let bits = UInt16(raw[i * 2]) | UInt16(raw[i * 2 + 1]) << 8
                let value = Float(Float16(bitPattern: bits))
                pixels[i] = value.isFinite ? UInt8((min(1, max(0, value)) * 255).rounded()) : 0
            }
        case .rgba8Unorm, .rgba8Unorm_srgb, .bgra8Unorm, .bgra8Unorm_srgb:
            pixels = raw
        default:
            throw DisplayError.textureUnavailable
        }
        return FrameSnapshot(width: width, height: height, swapRedBlue: swap, pixels: pixels)
    }

    fileprivate func attach(_ view: MTKView) {
        precondition(self.view == nil || self.view === view, "A renderer supports one view at a time")
        self.view = view
        view.device = device
        view.colorPixelFormat = .rgba16Float
        updateLayerColor()
        view.clearColor = MTLClearColorMake(0, 0, 0, 1)
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        // Diagnostic: BRINEWELL_DISPLAY_SYNC=0 lets presents skip the host
        // display refresh, separating a vsync stall from a GPU stall.
        (view.layer as? CAMetalLayer)?.displaySyncEnabled =
            ProcessInfo.processInfo.environment["BRINEWELL_DISPLAY_SYNC"] != "0"
        view.delegate = self
        view.needsDisplay = true
    }

    fileprivate func detach(_ view: MTKView) {
        if self.view === view {
            view.delegate = nil
            self.view = nil
            appliedColorSpaceName = nil
            appliedEDR = nil
            clear()
        }
    }

    private func updateLayerColor() {
        guard let layer = view?.layer as? CAMetalLayer else { return }
        let color = frame?.colorState ?? .sdr
        let hdr = color.isPQ || color.isHLG
        let name = color.isPQ ? CGColorSpace.itur_2100_PQ :
            (color.isHLG ? CGColorSpace.itur_2100_HLG : CGColorSpace.extendedLinearSRGB)
        let colorspace = CGColorSpace(name: name)
        let changed = layer.wantsExtendedDynamicRangeContent != hdr || layer.colorspace != colorspace || layer.edrMetadata != nil
        guard changed || appliedColorSpaceName != name || appliedEDR != hdr else { return }
        layer.wantsExtendedDynamicRangeContent = hdr
        layer.colorspace = colorspace
        layer.edrMetadata = nil
        appliedColorSpaceName = name
        appliedEDR = hdr
    }

    public func requestRedraw() { view?.needsDisplay = true }

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        view.needsDisplay = true
    }

    public func draw(in view: MTKView) {
        updateLayerColor()
        // A hidden or occluded window may have no drawable; this is not an error.
        guard let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable else { return }
        guard let commandBuffer = queue.makeCommandBuffer() else {
            lastError = DisplayError.commandBufferUnavailable.localizedDescription
            return
        }
        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
            lastError = DisplayError.renderEncoderUnavailable.localizedDescription
            return
        }
        let currentFrame = frame
        if let currentFrame {
            let size = currentFrame.pixelSize
            FrameTimingLog.shared.log(
                "draw", seq: currentFrame.sequenceNumber, width: size.width, height: size.height,
                drawable: view.drawableSize,
                fullScreen: view.window?.styleMask.contains(.fullScreen)
            )
        }
        if let texture, currentFrame != nil {
            let width = Double(view.drawableSize.width)
            let height = Double(view.drawableSize.height)
            let scale = min(width / Double(texture.width), height / Double(texture.height))
            let displayWidth = Double(texture.width) * scale
            let displayHeight = Double(texture.height) * scale
            encoder.setViewport(MTLViewport(
                originX: (width - displayWidth) / 2,
                originY: (height - displayHeight) / 2,
                width: displayWidth, height: displayHeight, znear: 0, zfar: 1
            ))
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(texture, index: 0)
            let color = currentFrame?.colorState ?? .sdr
            var uniforms = DisplayColorUniforms(eotf: color.eotf, colorspace: color.colorspace)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<DisplayColorUniforms>.size, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        encoder.endEncoding()
        commandBuffer.present(drawable)
        let lease = currentMetalLease
        lease?.beginDraw()
        commandBuffer.addCompletedHandler { [currentFrame, lease] buffer in
            lease?.endDraw()
            if let currentFrame {
                let size = currentFrame.pixelSize
                FrameTimingLog.shared.log(
                    "gpuDone", seq: currentFrame.sequenceNumber,
                    width: size.width, height: size.height
                )
            }
            withExtendedLifetime(currentFrame) {}
            if let error = buffer.error {
                let message = DisplayError.gpu(error.localizedDescription).localizedDescription
                Task { @MainActor [weak self] in
                    self?.lastError = message
                }
            }
        }
        commandBuffer.commit()
    }

    private struct DisplayColorUniforms {
        var eotf: UInt32
        var colorspace: UInt32
    }

    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct VertexOut { float4 position [[position]]; float2 uv; };
    vertex VertexOut displayVertex(uint id [[vertex_id]]) {
        const float2 positions[] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };
        const float2 uvs[] = { float2(0, 1), float2(2, 1), float2(0, -1) };
        return { float4(positions[id], 0, 1), uvs[id] };
    }
    struct DisplayColorUniforms { uint eotf; uint colorspace; };
    fragment float4 displayFragment(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                                    constant DisplayColorUniforms &u [[buffer(0)]]) {
        constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
        float3 c = tex.sample(s, in.uv).rgb;
        if (u.eotf == 3 || (u.eotf == 2 && u.colorspace == 9)) return float4(c, 1.0);
        if (u.eotf == 2) {
            const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
            const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
            float3 p = pow(clamp(c, 0.0, 1.0), float3(1.0 / m2));
            float3 x = pow(max(p - c1, 0.0) / (c2 - c3 * p), float3(1.0 / m1));
            c = float3(0.6274040*x.r + 0.3292820*x.g + 0.0433136*x.b,
                       0.0690970*x.r + 0.9195400*x.g + 0.0113612*x.b,
                       0.0163916*x.r + 0.0880132*x.g + 0.8955950*x.b);
            p = pow(max(c, 0.0), float3(m1));
            c = pow((c1 + c2 * p) / (1.0 + c3 * p), float3(m2));
        } else {
            c = select(pow(max((c + 0.055) / 1.055, 0.0), float3(2.4)), c / 12.92, c <= 0.04045);
            if (u.colorspace == 9) {
                float3 x = c;
                c = float3(1.6604910*x.r - 0.5876411*x.g - 0.0728499*x.b,
                          -0.1245505*x.r + 1.1328999*x.g - 0.0083494*x.b,
                          -0.0181508*x.r - 0.1005789*x.g + 1.1187297*x.b);
            }
        }
        return float4(c, 1.0);
    }
    """
}

/// A SwiftUI host for a renderer. One renderer must only be hosted by one view.
public struct IOSurfaceDisplay: NSViewRepresentable {
    private let renderer: IOSurfaceRenderer

    public init(renderer: IOSurfaceRenderer) {
        self.renderer = renderer
    }

    public func makeNSView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero)
        renderer.attach(view)
        return view
    }

    public func updateNSView(_ view: MTKView, context: Context) {
        renderer.attach(view)
    }

    public static func dismantleNSView(_ view: MTKView, coordinator: ()) {
        (view.delegate as? IOSurfaceRenderer)?.detach(view)
    }
}
