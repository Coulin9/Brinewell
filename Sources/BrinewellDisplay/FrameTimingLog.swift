import Foundation
import QuartzCore

/// Opt-in per-frame timing log for the display path.
///
/// Enabled with `BRINEWELL_FRAME_TIMING=<path>` (or `1` for the default path);
/// every entry point is a no-op when disabled, so the normal display path is
/// untouched.  Records the three points that can stall the guest frame lease:
/// frame arrival, the MTKView draw, and GPU completion (lease release).
final class FrameTimingLog: @unchecked Sendable {
    static let shared = FrameTimingLog()

    private let queue = DispatchQueue(label: "brinewell.frame-timing")
    private let handle: FileHandle?
    private let iso = ISO8601DateFormatter()

    private init() {
        iso.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let env = ProcessInfo.processInfo.environment["BRINEWELL_FRAME_TIMING"] ?? ""
        guard !env.isEmpty else {
            handle = nil
            return
        }
        let path = env == "1" ? "/tmp/brinewell-frame-timing.csv" : env
        FileManager.default.createFile(atPath: path, contents: nil)
        handle = FileHandle(forWritingAtPath: path)
        write("time,uptime_ms,event,seq,width,height,drawable_w,drawable_h,fullscreen\n")
    }

    private func write(_ line: String) {
        guard let handle else { return }
        queue.async { try? handle.write(contentsOf: Data(line.utf8)) }
    }

    func log(_ event: String, seq: UInt64, width: Int, height: Int,
             drawable: CGSize = .zero, fullScreen: Bool? = nil) {
        guard let handle else { return }
        let time = iso.string(from: Date())
        let uptime = CACurrentMediaTime() * 1000
        let full = fullScreen.map { $0 ? "1" : "0" } ?? ""
        let line = String(
            format: "%@,%.1f,%@,%llu,%d,%d,%d,%d,%@\n",
            time, uptime, event, seq, width, height,
            Int(drawable.width), Int(drawable.height), full
        )
        queue.async {
            try? handle.write(contentsOf: Data(line.utf8))
        }
    }
}
