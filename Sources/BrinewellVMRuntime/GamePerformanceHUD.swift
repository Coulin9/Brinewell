import AppKit
import BrinewellCore
import Foundation

/// Presentation keeps game submissions and whole-host counters visibly separate.
struct GameHUDPresentation: Equatable {
    struct Row: Equatable {
        let title: String
        let value: String
        var section = false
    }
    let level: GameHUDLevel
    let headline: String
    let rows: [Row]
    let frameTimes: [Double]
    let cores: [String]

    static func make(level: GameHUDLevel, game: GamePerformanceSnapshot?,
                     hardware: HostHardwareTelemetry.Snapshot?, refreshRate: Double?) -> Self {
        let sample = game?.sample
        let headline = sample.map { String(format: "%.1f FPS", $0.framesPerSecond) } ?? String(localized: "No frames yet")
        guard level.rawValue >= 2 else {
            return Self(level: level, headline: headline, rows: [], frameTimes: [], cores: [])
        }
        var rows: [Row] = []
        func add(_ title: String, _ value: String, section: Bool = false) {
            rows.append(Row(title: title, value: value, section: section))
        }
        func number(_ value: Int64?, divisor: Double = 1, format: String, unit: String) -> String {
            value.map { String(format: format, Double($0) / divisor) + unit } ?? "—"
        }
        func memory(_ value: Int64?) -> String {
            number(value, divisor: 1_073_741_824, format: "%.1f", unit: " GiB")
        }
        func utilization(_ value: Int64?) -> String {
            number(value, divisor: 100, format: "%.0f", unit: "%")
        }
        func power(_ value: Int64?) -> String {
            number(value, divisor: 1_000_000, format: "%.1f", unit: " W")
        }
        func temperature(_ value: Int64?) -> String {
            number(value, divisor: 1000, format: "%.0f", unit: "°C")
        }
        let detailed = level.rawValue >= 3
        add(String(localized: "Game Frame Time"), sample.map { String(format: "%.2f ms", $0.frameTimeMS) } ?? "—")
        if detailed {
            let backend = sample?.source.backend == "neptune" ? "Neptune" : "Vulkan"
            add(String(localized: "Frame Source"), sample.map { "\(backend) · \($0.width) × \($0.height)" } ?? String(localized: "Waiting for a supported foreground game"))
        }
        if level == .full {
            add("1% Low", game?.history?.onePercentLowFPS.map { String(format: "%.1f FPS", $0) } ?? String(localized: "— · at least 100 frames required"))
            add(String(localized: "Frame-Time Range"), game?.history.map {
                String(format: "%.2f – %.2f ms", $0.minimumFrameTimeMS, $0.maximumFrameTimeMS)
            } ?? "—")
            add(String(localized: "Sample Window"), game?.history.map {
                String(format: String(localized: "Last %.1f s · %d frames"), $0.durationMS / 1000, $0.sampleCount)
            } ?? String(localized: "Waiting for full frame data"))
        }
        add(String(localized: "Host Hardware · Whole Mac"), hardware == nil ? String(localized: "Data unavailable") : "", section: true)
        if detailed {
            add("CPU", "\(utilization(hardware?.cpuBusyBasisPoints))  ·  \(power(hardware?.cpuMicrowatts))  ·  \(temperature(hardware?.cpuMillidegrees))")
            add(String(localized: "Busiest CPU Cluster"), number(hardware?.cpuMegahertz, divisor: 1000, format: "%.2f", unit: " GHz"))
            add("GPU", "\(utilization(hardware?.gpuBusyBasisPoints))  ·  \(power(hardware?.gpuMicrowatts))  ·  \(temperature(hardware?.gpuMillidegrees))")
            add(String(localized: "GPU Clock"), number(hardware?.gpuHertz, divisor: 1_000_000, format: "%.0f", unit: " MHz"))
        } else {
            add("CPU", "\(utilization(hardware?.cpuBusyBasisPoints))  ·  \(power(hardware?.cpuMicrowatts))")
            add("GPU", "\(utilization(hardware?.gpuBusyBasisPoints))  ·  \(power(hardware?.gpuMicrowatts))")
        }
        add(String(localized: "Unified Memory"), "\(memory(hardware?.ramUsedBytes)) / \(memory(hardware?.ramTotalBytes))")
        if detailed {
            add(String(localized: "GPU Unified Memory Usage"), memory(hardware?.gpuMemoryBytes))
        }
        if level == .full {
            add(String(localized: "Swap"), "\(memory(hardware?.swapUsedBytes)) / \(memory(hardware?.swapTotalBytes))")
            let pressure: String
            switch hardware?.memoryPressure {
            case .normal: pressure = String(localized: "Normal")
            case .warning: pressure = String(localized: "Elevated")
            case .critical: pressure = String(localized: "High")
            case nil: pressure = "—"
            }
            add(String(localized: "Memory Pressure"), pressure)
            add(String(localized: "System Power (Estimated)"), power(hardware?.systemMicrowatts))
            if hardware?.aneMicrowatts != nil { add(String(localized: "Neural Engine"), power(hardware?.aneMicrowatts)) }
            if let fans = hardware?.fans, !fans.isEmpty {
                for (index, fan) in fans.enumerated() {
                    add(String(localized: "Fan \(index + 1)"), "\(fan.rpm) RPM" + (fan.maxRPM.map { " / \($0)" } ?? ""))
                }
            }
            add(String(localized: "Virtual Display"), refreshRate.map { String(format: "%.0f Hz", $0) } ?? "—")
        }
        if let battery = hardware?.battery {
            let state = battery.isCharging ? String(localized: "Charging") : battery.isOnBattery ? String(localized: "On Battery") : String(localized: "Plugged In")
            var value = "\(battery.chargePercent)% · \(state)"
            if detailed, battery.isOnBattery, let minutes = battery.minutesToEmpty {
                value += String(localized: " · about \(minutes / 60)h \(minutes % 60)m remaining")
            } else if detailed, battery.isCharging, let minutes = battery.minutesToFullCharge {
                value += String(localized: " · about \(minutes / 60)h \(minutes % 60)m to full")
            }
            add(String(localized: "Battery"), value)
        }
        let cores = level == .full ? (hardware?.cores ?? []).map { core in
            "\(core.tier)\(core.die).\(core.id)  \(utilization(core.busy))  " +
            "\(number(core.megahertz > 0 ? core.megahertz : nil, divisor: 1000, format: "%.2f", unit: "G"))  \(temperature(core.millidegrees))"
        } : []
        return Self(level: level, headline: headline, rows: rows,
                    frameTimes: detailed ? (game?.history?.frameTimesMS ?? []) : [], cores: cores)
    }
}

@MainActor
final class GamePerformanceHUD: NSView {
    private(set) var presentation = GameHUDPresentation(level: .fps, headline: String(localized: "No frames yet"), rows: [], frameTimes: [], cores: [])
    private let padding: CGFloat = 12
    private let rowHeight: CGFloat = 19
    private var aspectRatioConstraint: NSLayoutConstraint?
    private var hasGraph: Bool { presentation.level.rawValue >= 3 }
    private var coreColumns: Int { presentation.cores.count > 24 ? 3 : 2 }
    private var coreRows: Int { (presentation.cores.count + coreColumns - 1) / coreColumns }

    override var isFlipped: Bool { true }
    override var intrinsicContentSize: NSSize {
        let width: CGFloat = presentation.level == .fps ? 155 : presentation.level == .full ? (coreColumns == 3 ? 590 : 430) : 360
        let rows = CGFloat(presentation.rows.count) * rowHeight
        let cores = presentation.cores.isEmpty ? 0 : 23 + CGFloat(coreRows) * 17
        return NSSize(width: width, height: 49 + rows + (hasGraph ? 89 : 0) + cores)
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        identifier = NSUserInterfaceItemIdentifier("game-performance-hud")
        translatesAutoresizingMaskIntoConstraints = false
        wantsLayer = true
        setAccessibilityElement(true)
        setAccessibilityRole(.staticText)
        updateAspectRatio()
    }

    required init?(coder: NSCoder) { nil }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func update(_ value: GameHUDPresentation) {
        guard value != presentation else { return }
        let oldSize = intrinsicContentSize
        presentation = value
        if intrinsicContentSize != oldSize {
            updateAspectRatio()
            invalidateIntrinsicContentSize()
        }
        setAccessibilityLabel(([String(localized: "Game Performance"), value.headline] + value.rows.map { "\($0.title) \($0.value)" } + value.cores).joined(separator: ", "))
        needsDisplay = true
    }

    private func updateAspectRatio() {
        let size = intrinsicContentSize
        let ratio = size.width / size.height
        aspectRatioConstraint?.isActive = false
        let constraint = widthAnchor.constraint(equalTo: heightAnchor, multiplier: ratio)
        constraint.isActive = true
        aspectRatioConstraint = constraint
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let context = NSGraphicsContext.current?.cgContext else { return }
        let size = intrinsicContentSize
        let scale = min(1, bounds.width / size.width, bounds.height / size.height)
        context.saveGState()
        defer { context.restoreGState() }
        context.scaleBy(x: scale, y: scale)
        NSColor(calibratedWhite: 0.035, alpha: 0.86).setFill()
        NSBezierPath(roundedRect: NSRect(origin: .zero, size: size), xRadius: 9, yRadius: 9).fill()
        drawText(presentation.headline, at: NSRect(x: padding, y: 11, width: size.width - 24, height: 25),
                 size: 19, color: NSColor(calibratedRed: 0.52, green: 0.96, blue: 0.76, alpha: 1), weight: .semibold)
        var y: CGFloat = 42
        for row in presentation.rows {
            if row.section {
                NSColor.white.withAlphaComponent(0.12).setFill()
                NSRect(x: padding, y: y, width: size.width - padding * 2, height: 0.5).fill()
            }
            drawText(row.title, at: NSRect(x: padding, y: y + 2, width: size.width - 24, height: rowHeight),
                     size: row.section ? 10 : 11, color: .white.withAlphaComponent(row.section ? 0.55 : 0.72))
            drawText(row.value, at: NSRect(x: padding, y: y + 2, width: size.width - 24, height: rowHeight),
                     size: 11, color: .white, alignment: .right)
            y += rowHeight
        }
        if hasGraph {
            drawText(String(localized: "Frame Submission Interval · ms"), at: NSRect(x: padding, y: y + 2, width: size.width - 24, height: 18),
                     size: 10, color: .white.withAlphaComponent(0.55))
            let graph = NSRect(x: padding, y: y + 22, width: size.width - 24, height: 56)
            drawGraph(in: graph)
            y += 89
        }
        if !presentation.cores.isEmpty {
            drawText(String(localized: "CPU by Core · Utilization / GHz / Temperature"), at: NSRect(x: padding, y: y, width: size.width - 24, height: 20),
                     size: 10, color: .white.withAlphaComponent(0.55))
            y += 23
            let columnWidth = (size.width - padding * 2) / CGFloat(coreColumns)
            for (index, core) in presentation.cores.enumerated() {
                drawText(core, at: NSRect(x: padding + CGFloat(index % coreColumns) * columnWidth,
                                         y: y + CGFloat(index / coreColumns) * 17,
                                         width: columnWidth - 4, height: 17), size: 10, color: .white.withAlphaComponent(0.88))
            }
        }
    }

    private func drawText(_ text: String, at rect: NSRect, size: CGFloat, color: NSColor,
                          weight: NSFont.Weight = .regular, alignment: NSTextAlignment = .left) {
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = alignment
        paragraph.lineBreakMode = .byTruncatingTail
        (text as NSString).draw(in: rect, withAttributes: [
            .font: NSFont.monospacedDigitSystemFont(ofSize: size, weight: weight),
            .foregroundColor: color, .paragraphStyle: paragraph,
        ])
    }

    private func drawGraph(in rect: NSRect) {
        NSColor.white.withAlphaComponent(0.035).setFill()
        NSBezierPath(rect: rect).fill()
        guard !presentation.frameTimes.isEmpty else {
            drawText(String(localized: "Waiting for full frame data"), at: rect.insetBy(dx: 8, dy: 16), size: 10, color: .white.withAlphaComponent(0.5))
            return
        }
        let maximum = max(20, (presentation.frameTimes.max() ?? 20) * 1.12)
        let reference = 1000.0 / 60
        let referenceY = rect.maxY - CGFloat(reference / maximum) * rect.height
        NSColor.white.withAlphaComponent(0.18).setStroke()
        let guide = NSBezierPath()
        guide.move(to: NSPoint(x: rect.minX, y: referenceY))
        guide.line(to: NSPoint(x: rect.maxX, y: referenceY))
        guide.lineWidth = 0.5
        guide.stroke()
        let path = NSBezierPath()
        let count = presentation.frameTimes.count
        for (index, value) in presentation.frameTimes.enumerated() {
            let point = NSPoint(x: rect.minX + CGFloat(index) / CGFloat(max(1, count - 1)) * rect.width,
                               y: rect.maxY - CGFloat(value / maximum) * rect.height)
            if index == 0 { path.move(to: point) } else { path.line(to: point) }
        }
        NSColor(calibratedRed: 0.52, green: 0.96, blue: 0.76, alpha: 1).setStroke()
        path.lineWidth = 1
        path.stroke()
        drawText(String(format: "%.1f", maximum), at: NSRect(x: rect.minX + 3, y: rect.minY, width: 65, height: 14),
                 size: 9, color: .white.withAlphaComponent(0.6))
    }
}
