import AppKit

enum VMWindowGeometry {
    static func contentSize(pixels: NSSize, backingScale: CGFloat, available: NSSize) -> NSSize {
        let natural = NSSize(width: pixels.width / backingScale, height: pixels.height / backingScale)
        let scale = min(1, available.width / natural.width, available.height / natural.height)
        return NSSize(width: natural.width * scale, height: natural.height * scale)
    }

    @MainActor
    static func fit(_ window: VMWindow, pixels: NSSize) {
        guard pixels.width > 0, pixels.height > 0, !window.isFullScreen,
              let screen = window.screen ?? NSScreen.main else { return }
        let visible = screen.visibleFrame
        let decoration = window.frame.height - window.contentRect(forFrameRect: window.frame).height
        let size = contentSize(pixels: pixels, backingScale: screen.backingScaleFactor,
                               available: NSSize(width: visible.width, height: visible.height - decoration))
        let center = NSPoint(x: window.frame.midX, y: window.frame.midY)
        window.contentAspectRatio = pixels
        let minimumWidth = min(320, size.width)
        window.contentMinSize = NSSize(width: minimumWidth, height: minimumWidth * pixels.height / pixels.width)
        // Apply size and position in one step: resizing in place first can leave the
        // window straddling another screen and trigger a screen change mid-fit.
        var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        frame.origin.x = max(visible.minX, min(center.x - frame.width / 2, visible.maxX - frame.width))
        frame.origin.y = max(visible.minY, min(center.y - frame.height / 2, visible.maxY - frame.height))
        window.setFrame(frame, display: true)
    }
}
