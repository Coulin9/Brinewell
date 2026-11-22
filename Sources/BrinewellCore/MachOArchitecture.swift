import Foundation

/// Reads the architecture slices of a Mach-O file without loading it.
public enum MachOArchitecture {
    public enum Error: Swift.Error, LocalizedError {
        case unreadable(String)
        case unsupported(String)

        public var errorDescription: String? {
            switch self {
            case .unreadable(let path): return "cannot read Mach-O file: \(path)"
            case .unsupported(let path): return "not a Mach-O file: \(path)"
            }
        }
    }

    public static func architectures(at url: URL) throws -> Set<String> {
        let data: Data
        do {
            data = try Data(contentsOf: url, options: [.mappedIfSafe])
        } catch {
            throw Error.unreadable(url.path)
        }
        return try architectures(in: data, path: url.path)
    }

    public static func architectures(in data: Data, path: String = "<memory>") throws -> Set<String> {
        guard data.count >= 8 else { throw Error.unsupported(path) }
        let bytes = [UInt8](data.prefix(8))
        switch (bytes[0], bytes[1], bytes[2], bytes[3]) {
        case (0xCF, 0xFA, 0xED, 0xFE), (0xCE, 0xFA, 0xED, 0xFE):
            guard let cpu = readUInt32(data, 4, littleEndian: true) else { throw Error.unsupported(path) }
            return [name(for: cpu)]
        case (0xCA, 0xFE, 0xBA, 0xBE), (0xCA, 0xFE, 0xBA, 0xBF):
            return try fatArchitectures(data, path: path, is64: bytes[3] == 0xBF, littleEndian: false)
        case (0xBE, 0xBA, 0xFE, 0xCA), (0xBF, 0xBA, 0xFE, 0xCA):
            return try fatArchitectures(data, path: path, is64: bytes[0] == 0xBF, littleEndian: true)
        default:
            throw Error.unsupported(path)
        }
    }

    private static func fatArchitectures(_ data: Data, path: String, is64: Bool, littleEndian: Bool) throws -> Set<String> {
        guard let count = readUInt32(data, 4, littleEndian: littleEndian), count > 0, count <= 32 else {
            throw Error.unsupported(path)
        }
        let stride = is64 ? 32 : 20
        var result = Set<String>()
        for index in 0..<Int(count) {
            let offset = 8 + index * stride
            guard let cpu = readUInt32(data, offset, littleEndian: littleEndian) else { break }
            result.insert(name(for: cpu))
        }
        guard !result.isEmpty else { throw Error.unsupported(path) }
        return result
    }

    private static func readUInt32(_ data: Data, _ offset: Int, littleEndian: Bool) -> UInt32? {
        guard offset >= 0, offset + 4 <= data.count else { return nil }
        let value = data.withUnsafeBytes { raw -> UInt32 in
            let base = raw.bindMemory(to: UInt8.self)
            let b0 = UInt32(base[offset])
            let b1 = UInt32(base[offset + 1])
            let b2 = UInt32(base[offset + 2])
            let b3 = UInt32(base[offset + 3])
            return littleEndian
                ? b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
                : (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
        }
        return value
    }

    private static func name(for cpu: UInt32) -> String {
        switch cpu {
        case 0x0100_0007: return "x86_64"
        case 0x0100_000C: return "arm64"
        case 0x0000_0007: return "i386"
        case 0x0000_000C: return "arm"
        default: return String(format: "cpu-0x%08x", cpu)
        }
    }
}
