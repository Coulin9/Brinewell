import AppKit
import BrinewellCore
import Foundation
import XCTest
@testable import BrinewellVMRuntime

@MainActor
final class GameHUDPresentationTests: XCTestCase {
    private func hardware() throws -> HostHardwareTelemetry.Snapshot {
        var json: [String: Any] = [
            "cpu_power": 12.5, "gpu_power": 22.25, "sys_power": 49.5,
            "cpu_active_ratio": 0.125, "gpu_active_ratio": 0.75, "gpu_freq_mhz": 1240,
            "temp": ["cpu_temp_avg": 62.5, "gpu_temp_avg": 60],
            "memory": ["ram_usage": 16_106_127_360, "ram_total": 51_539_607_552,
                       "swap_usage": 0, "swap_total": 0, "pressure_level": 0],
            "fans": [["name": "Left", "rpm": 2100, "max_rpm": 5300],
                     ["name": "Right", "rpm": 2000, "max_rpm": 5700]],
        ]
        json["cpu_tiers"] = [["label": "P", "active_ratio": 0.125, "freq_mhz": 4100,
                               "cores": (0..<18).map { ["die_id": 0, "core_id": $0, "active_ratio": 0.25, "freq_mhz": 4100] }]]
        return try XCTUnwrap(HostHardwareTelemetry.decode(JSONSerialization.data(withJSONObject: json),
            gpuMemoryBytes: 3_221_225_472, coreTemperature: { _, _, _ in 62500 },
            battery: .init(chargePercent: 76, isCharging: false, isOnBattery: true,
                           minutesToEmpty: 142, minutesToFullCharge: nil)))
    }

    private func game() throws -> GamePerformanceSnapshot {
        var state = GamePerformanceState()
        state.setEnabled(true)
        for batch in 1...8 {
            let intervals = (0..<30).map { $0 == 14 ? UInt64(33_333_334) : UInt64(16_666_667) }
            let csv = intervals.map(String.init).joined(separator: ",")
            let line = "game-frame 2 neptune 123 42 2560 1600 1 30 \(intervals.reduce(0,+)) \(batch) 1 \(csv)"
            state.receive(try XCTUnwrap(GameFrameSample(line: line)), at: UInt64(batch) * 500_000_000)
        }
        return try XCTUnwrap(state.snapshot(at: 4_000_000_000))
    }

    func testLevelsKeepMetricScopesAndUnitsHonest() throws {
        let game = try game(), hardware = try hardware()
        let fps = GameHUDPresentation.make(level: .fps, game: game, hardware: hardware, refreshRate: 120)
        XCTAssertTrue(fps.rows.isEmpty)
        XCTAssertTrue(fps.frameTimes.isEmpty)
        let compact = GameHUDPresentation.make(level: .compact, game: game, hardware: hardware, refreshRate: 120)
        XCTAssertTrue(compact.rows.contains { $0.title == "Host Hardware · Whole Mac" })
        XCTAssertTrue(compact.rows.contains { $0.title == "CPU" && $0.value.contains("12.5 W") })
        XCTAssertTrue(compact.rows.contains { $0.title == "Unified Memory" && $0.value == "15.0 GiB / 48.0 GiB" })
        XCTAssertTrue(compact.cores.isEmpty)
        let full = GameHUDPresentation.make(level: .full, game: game, hardware: hardware, refreshRate: 120)
        XCTAssertEqual(full.cores.count, 18)
        XCTAssertEqual(full.frameTimes.count, 240)
        XCTAssertTrue(full.rows.contains { $0.title == "GPU Unified Memory Usage" && $0.value == "3.0 GiB" })
        XCTAssertTrue(full.rows.contains { $0.title == "Swap" && $0.value == "0.0 GiB / 0.0 GiB" })
        XCTAssertTrue(full.rows.contains { $0.title == "System Power (Estimated)" && $0.value == "49.5 W" })
        XCTAssertTrue(full.rows.contains { $0.title == "1% Low" && $0.value == "30.0 FPS" })
        XCTAssertTrue(full.rows.contains { $0.title == "Battery" && $0.value.contains("2h 22m") })
        XCTAssertFalse(full.rows.contains { $0.title == "VRAM" || $0.title.contains("GPU Time") })
    }

    func testUnavailableHardwareIsNotZeroAndNoBatteryIsNotInvented() {
        let presentation = GameHUDPresentation.make(level: .full, game: nil, hardware: nil, refreshRate: nil)
        XCTAssertTrue(presentation.rows.contains { $0.title == "GPU Clock" && $0.value == "—" })
        XCTAssertTrue(presentation.rows.contains { $0.title == "Host Hardware · Whole Mac" && $0.value == "Data unavailable" })
        XCTAssertFalse(presentation.rows.contains { $0.title == "Battery" || $0.title.hasPrefix("Fan") })
        XCTAssertTrue(presentation.cores.isEmpty)
        XCTAssertTrue(presentation.frameTimes.isEmpty)
    }

    func testIdleCoreKeepsZeroUtilizationWithoutClaimingZeroFrequency() throws {
        let json = #"{"cpu_tiers":[{"label":"P","active_ratio":0,"freq_mhz":0,"cores":[{"die_id":0,"core_id":0,"active_ratio":0,"freq_mhz":0}]}]}"#
        let hardware = try XCTUnwrap(HostHardwareTelemetry.decode(Data(json.utf8)))
        let presentation = GameHUDPresentation.make(level: .full, game: nil, hardware: hardware, refreshRate: nil)
        XCTAssertEqual(presentation.cores, ["P0.0  0%  —  —"])
    }

    func testAllLevelsRenderAndRemainNonInteractiveWhenCompressed() throws {
        let hardware = try hardware(), game = try game()
        for level in GameHUDLevel.allCases where level != .off {
            let view = GamePerformanceHUD(frame: .zero)
            view.update(.make(level: level, game: game, hardware: hardware, refreshRate: 120))
            let size = view.intrinsicContentSize
            XCTAssertGreaterThan(size.width, 0)
            XCTAssertGreaterThan(size.height, 0)
            view.frame = NSRect(origin: .zero, size: size)
            let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
            view.cacheDisplay(in: view.bounds, to: bitmap)
            if let directory = ProcessInfo.processInfo.environment["BRINEWELL_HUD_RENDER_DIR"] {
                try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
                try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
                    .write(to: URL(fileURLWithPath: directory).appendingPathComponent("level-\(level.rawValue).png"))
            }
            view.frame = NSRect(x: 0, y: 0, width: 280, height: 220)
            let small = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
            view.cacheDisplay(in: view.bounds, to: small)
            XCTAssertNil(view.hitTest(NSPoint(x: 10, y: 10)))
            XCTAssertNotNil(view.accessibilityLabel())
        }
    }
}
