import Foundation
import IOKit.ps

@MainActor
enum HostPowerSources {
    static func battery() -> HostHardwareTelemetry.Snapshot.Battery? {
        guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(info)?.takeRetainedValue() as? [CFTypeRef] else { return nil }
        for source in sources {
            guard let description = IOPSGetPowerSourceDescription(info, source)?.takeUnretainedValue() as? [String: Any]
            else { continue }
            if let battery = battery(from: description) { return battery }
        }
        return nil
    }

    static func battery(from description: [String: Any]) -> HostHardwareTelemetry.Snapshot.Battery? {
        func integer(_ key: String) -> Int? {
            guard let number = description[key] as? NSNumber,
                  CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
            return Int(exactly: number.doubleValue)
        }
        func boolean(_ key: String) -> Bool? {
            guard let number = description[key] as? NSNumber,
                  CFGetTypeID(number) == CFBooleanGetTypeID() else { return nil }
            return number.boolValue
        }
        func minutes(_ key: String) -> Int? {
            // IOPS uses -1 while estimating; minutes are only meaningful in the matching power state.
            integer(key).flatMap { $0 >= 0 ? $0 : nil }
        }
        guard description[kIOPSTypeKey] as? String == kIOPSInternalBatteryType,
              boolean(kIOPSIsPresentKey) == true,
              let current = integer(kIOPSCurrentCapacityKey),
              let maximum = integer(kIOPSMaxCapacityKey), maximum > 0, (0...maximum).contains(current),
              let charging = boolean(kIOPSIsChargingKey),
              let state = description[kIOPSPowerSourceStateKey] as? String,
              state == kIOPSBatteryPowerValue || state == kIOPSACPowerValue else { return nil }
        let onBattery = state == kIOPSBatteryPowerValue
        return .init(chargePercent: Int((Double(current) / Double(maximum) * 100).rounded()),
                     isCharging: charging, isOnBattery: onBattery,
                     minutesToEmpty: onBattery && !charging ? minutes(kIOPSTimeToEmptyKey) : nil,
                     minutesToFullCharge: charging ? minutes(kIOPSTimeToFullChargeKey) : nil)
    }
}
