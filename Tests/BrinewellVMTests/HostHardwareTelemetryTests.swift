import Foundation
import IOKit.ps
import XCTest
@testable import BrinewellVMRuntime

final class HostHardwareTelemetryTests: XCTestCase {
    // Retained macmon sample: perf-lab/gamescope-gap-20261114/gamescope-on-1/power.jsonl:1.
    // Only consumed fields and the first core of each tier are included here.
    private let recorded = #"""
    {"cpu_active_ratio":0.18699008226394653,"cpu_power":10.156746864318848,
     "cpu_tiers":[
       {"active_ratio":0.09486301988363266,"freq_mhz":1659,"label":"P","cores":[
         {"active_ratio":0.2843344807624817,"core_id":0,"die_id":0,"freq_mhz":1753}]},
       {"active_ratio":0.3712442219257355,"freq_mhz":4450,"label":"S","cores":[
         {"active_ratio":0.45505231618881226,"core_id":0,"die_id":0,"freq_mhz":4429}]}],
     "gpu_active_ratio":0.4834590554237366,"gpu_freq_mhz":338,"gpu_power":1.2556430101394653,
     "ane_power":0.0,"sys_power":23.85986328125,
     "temp":{"cpu_temp_avg":51.538238525390625,"gpu_temp_avg":50.046688079833984},
     "memory":{"pressure_level":0,"pressure_percent":30,"ram_total":51539607552,
       "ram_usage":36266328064,"swap_total":9663676416,"swap_usage":8441757696},
     "fans":[{"max_rpm":5349,"name":"fan0","rpm":1348},{"max_rpm":5777,"name":"fan1","rpm":1456}]}
    """#

    private func fields() throws -> [String: Any] {
        try XCTUnwrap(JSONSerialization.jsonObject(with: Data(recorded.utf8)) as? [String: Any])
    }

    @MainActor
    private func decode(_ fields: [String: Any]) throws -> HostHardwareTelemetry.Snapshot {
        try XCTUnwrap(HostHardwareTelemetry.decode(JSONSerialization.data(withJSONObject: fields)))
    }

    @MainActor
    func testRecordedHostSnapshotIncludesSwapFansAndMemoryPressure() throws {
        let sample = try decode(fields())
        XCTAssertEqual(sample.cpuBusyBasisPoints, 1870)
        XCTAssertEqual(sample.cpuMegahertz, 4450)
        XCTAssertEqual(sample.cpuMicrowatts, 10_156_747)
        XCTAssertEqual(sample.cpuMillidegrees, 51_538)
        XCTAssertEqual(sample.gpuBusyBasisPoints, 4835)
        XCTAssertEqual(sample.gpuHertz, 338_000_000)
        XCTAssertEqual(sample.gpuMicrowatts, 1_255_643)
        XCTAssertEqual(sample.gpuMillidegrees, 50_047)
        XCTAssertEqual(sample.systemMicrowatts, 23_859_863)
        XCTAssertEqual(sample.ramUsedBytes, 36_266_328_064)
        XCTAssertEqual(sample.ramTotalBytes, 51_539_607_552)
        XCTAssertEqual(sample.swapUsedBytes, 8_441_757_696)
        XCTAssertEqual(sample.swapTotalBytes, 9_663_676_416)
        XCTAssertEqual(sample.memoryPressure, .normal)
        XCTAssertEqual(sample.memoryPressurePercent, 30)
        XCTAssertEqual(sample.fans, [.init(name: "fan0", rpm: 1348, maxRPM: 5349),
                                    .init(name: "fan1", rpm: 1456, maxRPM: 5777)])
        XCTAssertEqual(sample.cores.map(\.tier), ["P", "S"])
        XCTAssertEqual(sample.cores.map(\.megahertz), [1753, 4429])
        XCTAssertEqual(sample.cores.map(\.busy), [2843, 4551])
        XCTAssertNil(sample.gpuMemoryBytes, "AGX memory is independently sampled, not inferred from RAM")
        XCTAssertNil(sample.battery, "a macmon sample must not invent a battery")
        XCTAssertEqual(sample, try decode(fields()))
    }

    @MainActor
    func testOlderSamplesAndGuestWireProtocolRemainCompatible() throws {
        var old = try fields()
        for key in ["fans", "ane_power", "sys_power"] { old.removeValue(forKey: key) }
        old["memory"] = ["ram_total": 51_539_607_552, "ram_usage": 36_266_328_064]
        let sample = try decode(old)
        XCTAssertNil(sample.swapUsedBytes)
        XCTAssertNil(sample.swapTotalBytes)
        XCTAssertNil(sample.memoryPressure)
        XCTAssertNil(sample.memoryPressurePercent)
        XCTAssertNil(sample.aneMicrowatts)
        XCTAssertNil(sample.systemMicrowatts)
        XCTAssertEqual(sample.fans, [])
        XCTAssertEqual(sample.wireValues, try decode(fields()).wireValues,
                       "host-only additions must not change the existing guest protocol")
        XCTAssertEqual(sample.wireValues.split(separator: " ").count, 12 + 6 * sample.cores.count)
    }

    @MainActor
    func testMissingInvalidAndZeroValuesStayDistinct() throws {
        let missing = try decode([:])
        XCTAssertNil(missing.cpuBusyBasisPoints)
        XCTAssertNil(missing.swapUsedBytes)
        XCTAssertNil(missing.systemMicrowatts)
        var zero = try fields()
        zero["cpu_active_ratio"] = 0
        zero["gpu_active_ratio"] = 0
        zero["memory"] = ["ram_total": 1024, "ram_usage": 0, "swap_total": 0, "swap_usage": 0,
                          "pressure_level": 0, "pressure_percent": 0]
        zero["fans"] = [["name": "fan0", "rpm": 0]]
        let idle = try decode(zero)
        XCTAssertEqual(idle.cpuBusyBasisPoints, 0)
        XCTAssertEqual(idle.gpuBusyBasisPoints, 0)
        XCTAssertEqual(idle.ramUsedBytes, 0)
        XCTAssertEqual(idle.swapUsedBytes, 0)
        XCTAssertEqual(idle.swapTotalBytes, 0)
        XCTAssertEqual(idle.memoryPressurePercent, 0)
        XCTAssertEqual(idle.fans, [.init(name: "fan0", rpm: 0, maxRPM: nil)])

        var invalid = zero
        invalid["cpu_active_ratio"] = -1
        invalid["gpu_active_ratio"] = 1.01
        invalid["cpu_power"] = -10
        invalid["sys_power"] = 0 // macmon's unavailable sentinel
        invalid["ane_power"] = 1001
        invalid["gpu_freq_mhz"] = 20_001
        invalid["memory"] = ["ram_total": 100, "ram_usage": 101, "swap_total": -1, "swap_usage": 0,
                             "pressure_level": 3, "pressure_percent": 101]
        invalid["fans"] = [["name": "bad", "rpm": -1], ["name": "fan1", "rpm": 1000, "max_rpm": -1]]
        let rejected = try decode(invalid)
        XCTAssertNil(rejected.cpuBusyBasisPoints)
        XCTAssertNil(rejected.gpuBusyBasisPoints)
        XCTAssertNil(rejected.cpuMicrowatts)
        XCTAssertNil(rejected.systemMicrowatts)
        XCTAssertNil(rejected.aneMicrowatts)
        XCTAssertNil(rejected.gpuHertz)
        XCTAssertNil(rejected.ramUsedBytes)
        XCTAssertNil(rejected.swapUsedBytes)
        XCTAssertNil(rejected.memoryPressure)
        XCTAssertNil(rejected.memoryPressurePercent)
        XCTAssertEqual(rejected.fans, [.init(name: "fan1", rpm: 1000, maxRPM: nil)])
    }

    @MainActor
    func testMalformedAndUnboundedInputIsRejected() {
        for input in ["not json", #"{"cpu_power":NaN}"#, #"{"cpu_power":1e400}"#,
                      #"{"memory":{"swap_usage":9223372036854775808}}"#,
                      #"{"fans":[{"name":"fan0","rpm":"unknown"}]}"#,
                      String(repeating: " ", count: 32769)] {
            XCTAssertNil(HostHardwareTelemetry.decode(Data(input.utf8)), input)
        }
    }

    @MainActor
    func testSnapshotExpiresInsteadOfRetainingOldHardwareNumbers() throws {
        let now = ContinuousClock.now
        let timed = HostHardwareTelemetry.TimedSnapshot(snapshot: try decode(fields()), time: now)
        XCTAssertNotNil(timed.current(at: now))
        XCTAssertNotNil(timed.current(at: now.advanced(by: .milliseconds(1999))))
        XCTAssertNil(timed.current(at: now.advanced(by: .seconds(2))))
        XCTAssertNil(timed.current(at: now.advanced(by: .seconds(-1))))
    }

    private func batteryDescription() -> [String: Any] {
        [kIOPSTypeKey: kIOPSInternalBatteryType, kIOPSIsPresentKey: true,
         kIOPSCurrentCapacityKey: 3000, kIOPSMaxCapacityKey: 5000,
         kIOPSIsChargingKey: false, kIOPSPowerSourceStateKey: kIOPSBatteryPowerValue,
         kIOPSTimeToEmptyKey: 125, kIOPSTimeToFullChargeKey: 15]
    }

    @MainActor
    func testOfficialBatteryStateUsesOnlyApplicableTimeEstimate() throws {
        var description = batteryDescription()
        let battery = try XCTUnwrap(HostPowerSources.battery(from: description))
        XCTAssertEqual(battery.chargePercent, 60)
        XCTAssertTrue(battery.isOnBattery)
        XCTAssertFalse(battery.isCharging)
        XCTAssertEqual(battery.minutesToEmpty, 125)
        XCTAssertNil(battery.minutesToFullCharge)
        description[kIOPSIsChargingKey] = true
        description[kIOPSPowerSourceStateKey] = kIOPSACPowerValue
        let charging = try XCTUnwrap(HostPowerSources.battery(from: description))
        XCTAssertFalse(charging.isOnBattery)
        XCTAssertTrue(charging.isCharging)
        XCTAssertNil(charging.minutesToEmpty)
        XCTAssertEqual(charging.minutesToFullCharge, 15)
        description[kIOPSTimeToFullChargeKey] = -1
        XCTAssertNil(HostPowerSources.battery(from: description)?.minutesToFullCharge)
        description[kIOPSCurrentCapacityKey] = 0
        XCTAssertEqual(HostPowerSources.battery(from: description)?.chargePercent, 0)
    }

    @MainActor
    func testExternalMissingOrInvalidBatteriesAreNotDisplayed() {
        for (key, value) in [(kIOPSTypeKey, "UPS" as Any), (kIOPSIsPresentKey, false),
                             (kIOPSCurrentCapacityKey, 6000), (kIOPSCurrentCapacityKey, -1),
                             (kIOPSCurrentCapacityKey, true), (kIOPSCurrentCapacityKey, Double.infinity),
                             (kIOPSMaxCapacityKey, 0), (kIOPSIsChargingKey, 1),
                             (kIOPSPowerSourceStateKey, kIOPSOffLineValue)] {
            var description = batteryDescription()
            description[key] = value
            XCTAssertNil(HostPowerSources.battery(from: description), "accepted \(key)=\(value)")
        }
        XCTAssertNil(HostPowerSources.battery(from: [:]))
    }
}
