import Foundation
import IOKit

@MainActor
final class HostHardwareTelemetry {
    enum MemoryPressure: Int, Equatable {
        case normal = 0, warning, critical
    }

    // Every value describes the host Mac, never the VM or the current game.
    struct Snapshot: Equatable {
        struct Core: Equatable {
            let tier: String
            let die: Int
            let id: Int
            let megahertz: Int64
            let busy: Int64?
            let millidegrees: Int64?

            var wireValues: String {
                "\(tier) \(die) \(id) \(megahertz) \(busy ?? -1) \(millidegrees ?? -1)"
            }
        }

        struct Fan: Equatable {
            let name: String
            let rpm: Int
            let maxRPM: Int?
        }

        struct Battery: Equatable {
            let chargePercent: Int
            let isCharging: Bool
            let isOnBattery: Bool
            let minutesToEmpty: Int?
            let minutesToFullCharge: Int?
        }

        let cpuMicrowatts: Int64?
        let cpuMillidegrees: Int64?
        let gpuMillidegrees: Int64?
        let gpuMicrowatts: Int64?
        let gpuHertz: Int64?
        let gpuBusyBasisPoints: Int64?
        let gpuMemoryBytes: Int64?
        let cpuBusyBasisPoints: Int64?
        let cpuMegahertz: Int64?
        let ramUsedBytes: Int64?
        let ramTotalBytes: Int64?
        let cores: [Core]
        let aneMicrowatts: Int64?
        // SMC PSTR estimate, including more than CPU/GPU/ANE; not wall-socket power.
        let systemMicrowatts: Int64?
        let swapUsedBytes: Int64?
        let swapTotalBytes: Int64?
        let memoryPressure: MemoryPressure?
        // macmon uses 100 - kern.memorystatus_level, not RAM used / total.
        let memoryPressurePercent: Int?
        let fans: [Fan]
        let battery: Battery?

        var wireValues: String {
            let totals = [cpuMicrowatts, cpuMillidegrees, gpuMillidegrees, gpuMicrowatts, gpuHertz,
             gpuBusyBasisPoints, gpuMemoryBytes, cpuBusyBasisPoints, cpuMegahertz, ramUsedBytes,
             ramTotalBytes]
                .map { String($0 ?? -1) }
            return (totals + [String(cores.count)] + cores.map(\.wireValues)).joined(separator: " ")
        }
    }

    private struct Reading: Decodable {
        struct Temperature: Decodable {
            let cpu_temp_avg: Double?
            let gpu_temp_avg: Double?
        }
        struct CPUTier: Decodable {
            struct Core: Decodable {
                let die_id: Int
                let core_id: Int
                let active_ratio: Double
                let freq_mhz: Int64
            }
            let label: String
            let active_ratio: Double
            let freq_mhz: Int64
            let cores: [Core]
        }
        struct Memory: Decodable {
            let ram_usage: Int64?
            let ram_total: Int64?
            let swap_usage: Int64?
            let swap_total: Int64?
            let pressure_level: Int?
            let pressure_percent: Int?
        }
        struct Fan: Decodable {
            let name: String
            let rpm: Int
            let max_rpm: Int?
        }
        let cpu_active_ratio: Double?
        let cpu_tiers: [CPUTier]?
        let cpu_power: Double?
        let gpu_power: Double?
        let ane_power: Double?
        let sys_power: Double?
        let gpu_freq_mhz: Double?
        let gpu_active_ratio: Double?
        let temp: Temperature?
        let memory: Memory?
        let fans: [Fan]?
    }

    struct TimedSnapshot {
        let snapshot: Snapshot
        let time: ContinuousClock.Instant

        func current(at now: ContinuousClock.Instant = .now) -> Snapshot? {
            let age = time.duration(to: now)
            return age >= .zero && age < .seconds(2) ? snapshot : nil
        }
    }

    private let process = Process()
    private let output = Pipe()
    private lazy var coreTemperatures = HostCoreTemperatures()
    private var line = Data()
    private var discardingLine = false
    private var latest: TimedSnapshot?

    func start(executable: URL) throws {
        process.executableURL = executable
        process.arguments = ["pipe", "--interval", "1000"]
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        try process.run()
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let bytes = handle.availableData
            if bytes.isEmpty { handle.readabilityHandler = nil }
            DispatchQueue.main.async { self?.consume(bytes) }
        }
    }

    func stop() {
        output.fileHandleForReading.readabilityHandler = nil
        if process.isRunning { process.terminate() }
        latest = nil
    }

    func sample() -> Snapshot? {
        latest?.current()
    }

    private func consume(_ bytes: Data) {
        if bytes.isEmpty { latest = nil; return }
        for byte in bytes {
            if byte == 10 {
                if !discardingLine { update(line) }
                line.removeAll(keepingCapacity: true)
                discardingLine = false
            } else if !discardingLine {
                if line.count < 32768 { line.append(byte) } else { discardingLine = true }
            }
        }
    }

    private func update(_ data: Data) {
        guard let snapshot = Self.decode(data, gpuMemoryBytes: Self.gpuMemoryInUse(),
                                        coreTemperature: { self.coreTemperatures?.read(tier: $0, die: $1, core: $2) },
                                        battery: HostPowerSources.battery()) else {
            latest = nil
            return
        }
        latest = TimedSnapshot(snapshot: snapshot, time: .now)
    }

    static func decode(_ data: Data, gpuMemoryBytes: Int64? = nil,
                       coreTemperature: (String, Int, Int) -> Int64? = { _, _, _ in nil },
                       battery: Snapshot.Battery? = nil) -> Snapshot? {
        guard data.count <= 32768, let reading = try? JSONDecoder().decode(Reading.self, from: data) else { return nil }
        func scaled(_ reading: Double?, maximum: Double, multiplier: Double) -> Int64? {
            // macmon can use zero when a frequency or power counter is unavailable.
            guard let reading, reading.isFinite, reading > 0, reading <= maximum else { return nil }
            return Int64((reading * multiplier).rounded())
        }
        func basisPoints(_ reading: Double?) -> Int64? {
            guard let reading, (0...1).contains(reading) else { return nil }
            return Int64((reading * 10_000).rounded())
        }
        let cpuFrequency = reading.cpu_tiers?
            .filter { $0.active_ratio > 0 && $0.active_ratio <= 1 && (1...20_000).contains($0.freq_mhz) }
            .map(\.freq_mhz).max()
        func memoryPair(used: Int64?, total: Int64?, allowEmpty: Bool) -> (used: Int64, total: Int64)? {
            guard let used, let total, total >= (allowEmpty ? 0 : 1), (0...total).contains(used) else { return nil }
            return (used, total)
        }
        let memory = memoryPair(used: reading.memory?.ram_usage, total: reading.memory?.ram_total, allowEmpty: false)
        let swap = memoryPair(used: reading.memory?.swap_usage, total: reading.memory?.swap_total, allowEmpty: true)
        var cores: [Snapshot.Core] = []
        for tier in reading.cpu_tiers ?? [] {
            let label = ["E", "P", "S"].contains(tier.label) ? tier.label : "_"
            for core in tier.cores {
                guard (0...63).contains(core.die_id), (0...255).contains(core.core_id) else { continue }
                cores.append(.init(tier: label, die: core.die_id, id: core.core_id,
                                   megahertz: (0...20_000).contains(core.freq_mhz) ? core.freq_mhz : -1,
                                   busy: basisPoints(core.active_ratio),
                                   millidegrees: coreTemperature(label, core.die_id, core.core_id)))
            }
        }
        let fans = (reading.fans ?? []).prefix(16).compactMap { fan -> Snapshot.Fan? in
            guard !fan.name.isEmpty, fan.name.utf8.count <= 64, (0...100_000).contains(fan.rpm) else { return nil }
            let maximum = fan.max_rpm.flatMap { (1...100_000).contains($0) ? $0 : nil }
            return Snapshot.Fan(name: fan.name, rpm: fan.rpm, maxRPM: maximum)
        }
        return Snapshot(
            cpuMicrowatts: scaled(reading.cpu_power, maximum: 1000, multiplier: 1_000_000),
            cpuMillidegrees: scaled(reading.temp?.cpu_temp_avg, maximum: 200, multiplier: 1000),
            gpuMillidegrees: scaled(reading.temp?.gpu_temp_avg, maximum: 200, multiplier: 1000),
            gpuMicrowatts: scaled(reading.gpu_power, maximum: 1000, multiplier: 1_000_000),
            gpuHertz: scaled(reading.gpu_freq_mhz, maximum: 20_000, multiplier: 1_000_000),
            gpuBusyBasisPoints: basisPoints(reading.gpu_active_ratio),
            gpuMemoryBytes: gpuMemoryBytes.flatMap { $0 >= 0 ? $0 : nil },
            cpuBusyBasisPoints: basisPoints(reading.cpu_active_ratio),
            cpuMegahertz: cpuFrequency,
            ramUsedBytes: memory?.used,
            ramTotalBytes: memory?.total,
            cores: Array(cores.prefix(64)),
            aneMicrowatts: scaled(reading.ane_power, maximum: 1000, multiplier: 1_000_000),
            systemMicrowatts: scaled(reading.sys_power, maximum: 2000, multiplier: 1_000_000),
            swapUsedBytes: swap?.used,
            swapTotalBytes: swap?.total,
            memoryPressure: reading.memory?.pressure_level.flatMap(MemoryPressure.init(rawValue:)),
            memoryPressurePercent: reading.memory?.pressure_percent.flatMap { (0...100).contains($0) ? $0 : nil },
            fans: fans,
            battery: battery
        )
    }

    private static func gpuMemoryInUse() -> Int64? {
        var services: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"), &services)
                == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(services) }
        while case let service = IOIteratorNext(services), service != 0 {
            defer { IOObjectRelease(service) }
            let properties = IORegistryEntryCreateCFProperty(
                service, "PerformanceStatistics" as CFString, kCFAllocatorDefault, 0
            )?.takeRetainedValue() as? [String: Any]
            if let bytes = (properties?["In use system memory"] as? NSNumber)?.int64Value, bytes >= 0 {
                return bytes
            }
        }
        return nil
    }
}
