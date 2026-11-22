import Foundation
import IOKit

// AppleSMC key reads use the 80-byte SMC payload described by macmon's KeyData.
final class HostCoreTemperatures {
    private static let payloadSize = 80
    private static let keyInfoOffset = 28
    private static let resultOffset = 40
    private static let commandOffset = 42
    private static let valueOffset = 48

    private let connection: io_connect_t
    private var requests: [String: [UInt8]] = [:]

    init?() {
        guard HostCPUInfo.modelName() == "Apple M5 Max" else { return nil }

        var services: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleSMC"), &services)
                == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(services) }
        while case let service = IOIteratorNext(services), service != 0 {
            defer { IOObjectRelease(service) }
            var name = [UInt8](repeating: 0, count: 128)
            guard name.withUnsafeMutableBytes({ IORegistryEntryGetName(service, $0.baseAddress!) }) == KERN_SUCCESS,
                  String(decoding: name.prefix { $0 != 0 }, as: UTF8.self) == "AppleSMCKeysEndpoint" else { continue }
            var port: io_connect_t = 0
            guard IOServiceOpen(service, mach_task_self_, 0, &port) == KERN_SUCCESS else { return nil }
            connection = port
            return
        }
        return nil
    }

    deinit { IOServiceClose(connection) }

    func read(tier: String, die: Int, core: Int) -> Int64? {
        guard die == 0 else { return nil }
        // Stats' M5 sensor map. IOReport reports P-core IDs 0...5 and 10...15.
        let key: String
        switch (tier, core) {
        case ("S", 0...5): key = Self.superKeys[core]
        case ("P", 0...5): key = Self.performanceKeys[core]
        case ("P", 10...15): key = Self.performanceKeys[core - 4]
        default: return nil
        }
        guard let request = requests[key] ?? makeReadRequest(key),
              let response = call(request) else { return nil }
        let bits = response.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: Self.valueOffset, as: UInt32.self) }
        let degrees = Float(bitPattern: UInt32(littleEndian: bits))
        guard degrees > 0, degrees <= 150 else { return nil }
        return Int64((degrees * 1000).rounded())
    }

    private static let superKeys = ["Tp00", "Tp04", "Tp08", "Tp0C", "Tp0G", "Tp0K"]
    private static let performanceKeys = [
        "Tp0O", "Tp0R", "Tp0U", "Tp0X", "Tp0a", "Tp0d",
        "Tp0g", "Tp0j", "Tp0m", "Tp0p", "Tp0u", "Tp0y",
    ]

    private func makeReadRequest(_ key: String) -> [UInt8]? {
        var input = [UInt8](repeating: 0, count: Self.payloadSize)
        input.replaceSubrange(0..<4, with: key.utf8.reversed())
        input[Self.commandOffset] = 9 // Read key metadata.
        guard let response = call(input) else { return nil }
        let (length, type) = response.withUnsafeBytes { data in
            (data.loadUnaligned(fromByteOffset: Self.keyInfoOffset, as: UInt32.self),
             data.loadUnaligned(fromByteOffset: Self.keyInfoOffset + 4, as: UInt32.self))
        }
        guard length == 4, type == 0x666c7420 else { return nil } // FourCC "flt ".
        input.replaceSubrange(Self.keyInfoOffset..<(Self.keyInfoOffset + 12),
                              with: response[Self.keyInfoOffset..<(Self.keyInfoOffset + 12)])
        input[Self.commandOffset] = 5 // Read key value.
        requests[key] = input
        return input
    }

    private func call(_ input: [UInt8]) -> [UInt8]? {
        var output = [UInt8](repeating: 0, count: Self.payloadSize)
        var size = output.count
        let result = input.withUnsafeBytes { request in
            output.withUnsafeMutableBytes { response in
                IOConnectCallStructMethod(connection, 2, request.baseAddress, request.count, response.baseAddress, &size)
            }
        }
        guard result == KERN_SUCCESS, size == Self.payloadSize, output[Self.resultOffset] == 0 else { return nil }
        return output
    }
}
