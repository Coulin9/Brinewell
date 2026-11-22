import Darwin
import Foundation

enum HostCPUInfo {
    private struct SamplerReport: Decodable {
        struct SoC: Decodable {
            struct Tier: Decodable {
                let freqs: [Int]
            }
            let cpu_tiers: [Tier]
        }
        let soc: SoC
    }

    static func modelName() -> String? {
        var size = 0
        guard sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0) == 0,
              (2...128).contains(size) else { return nil }
        var bytes = [UInt8](repeating: 0, count: size)
        guard sysctlbyname("machdep.cpu.brand_string", &bytes, &size, nil, 0) == 0 else { return nil }
        let name = String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        return name.isEmpty ? nil : name
    }

    static func kernelArguments(sampler: URL) -> String {
        var arguments = ""
        if let model = modelName() {
            // Keep whitespace out of the kernel argument without changing the brand string.
            let encoded = Data(model.utf8).base64EncodedString()
            arguments += " brinewell.cpu_model_b64=\(encoded)"
        }
        if let frequency = maximumFrequencyMHz(sampler: sampler) {
            arguments += " brinewell.cpu_max_mhz=\(frequency)"
        }
        return arguments
    }

    private static func maximumFrequencyMHz(sampler: URL) -> Int? {
        let process = Process()
        let output = Pipe()
        process.executableURL = sampler
        process.arguments = ["pipe", "--soc-info", "--samples", "1", "--interval", "100"]
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        do { try process.run() } catch { return nil }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else { return nil }
        return maximumFrequencyMHz(in: data)
    }

    static func maximumFrequencyMHz(in data: Data) -> Int? {
        guard let report = try? JSONDecoder().decode(SamplerReport.self, from: data) else { return nil }
        return report.soc.cpu_tiers.flatMap(\.freqs).filter { (1...20_000).contains($0) }.max()
    }
}
