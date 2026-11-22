import CoreGraphics
import Foundation
import Vision

/// UU writes its binding QR as terminal half-block characters in the guest.
public enum UUQRCode {
    public static func bindingCode(fromTerminalData data: Data) -> String? {
        guard data.count <= 16_384, let text = String(data: data, encoding: .utf8) else { return nil }
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        let rows = lines.last?.isEmpty == true ? Array(lines.dropLast()) : lines
        guard (10...128).contains(rows.count), let width = rows.map(\.count).max(),
              (20...256).contains(width) else { return nil }
        var modules = [UInt8](repeating: 255, count: width * rows.count * 2)
        for (y, row) in rows.enumerated() {
            for (x, character) in row.enumerated() {
                let top: UInt8
                let bottom: UInt8
                switch character {
                case " ": (top, bottom) = (255, 255)
                case "█": (top, bottom) = (0, 0)
                case "▀": (top, bottom) = (0, 255)
                case "▄": (top, bottom) = (255, 0)
                default: return nil
                }
                modules[(y * 2) * width + x] = top
                modules[(y * 2 + 1) * width + x] = bottom
            }
        }
        // Terminal themes may invert foreground/background. Give Vision a crisp
        // raster and a quiet border, and accept only the vendor's binding code.
        for inverted in [false, true] {
            guard let image = image(modules: modules, width: width, height: rows.count * 2, inverted: inverted) else { return nil }
            let request = VNDetectBarcodesRequest()
            request.symbologies = [.qr]
            do { try VNImageRequestHandler(cgImage: image).perform([request]) }
            catch { continue }
            if let identifier = request.results?.compactMap(\.payloadStringValue).first(where: isBindingCode) {
                return identifier
            }
        }
        return nil
    }

    static func isBindingCode(_ value: String) -> Bool {
        value.utf8.count == 32 && value.utf8.allSatisfy {
            (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0)
        }
    }

    private static func image(modules: [UInt8], width: Int, height: Int, inverted: Bool) -> CGImage? {
        let scale = 8
        let border = 4
        let pixelWidth = (width + border * 2) * scale
        let pixelHeight = (height + border * 2) * scale
        var pixels = [UInt8](repeating: 255, count: pixelWidth * pixelHeight)
        for y in 0..<height {
            for x in 0..<width {
                let value = inverted ? 255 - modules[y * width + x] : modules[y * width + x]
                for dy in 0..<scale {
                    let offset = ((y + border) * scale + dy) * pixelWidth + (x + border) * scale
                    for dx in 0..<scale { pixels[offset + dx] = value }
                }
            }
        }
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(width: pixelWidth, height: pixelHeight, bitsPerComponent: 8, bitsPerPixel: 8,
                       bytesPerRow: pixelWidth, space: CGColorSpaceCreateDeviceGray(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
