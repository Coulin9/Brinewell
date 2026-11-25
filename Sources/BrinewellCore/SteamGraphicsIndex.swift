import Foundation

public struct SteamGraphicsIndex: Sendable, Equatable {
    private let entries: [UInt32: GraphicsBackend]
    private static let maximumSize = 25 * 1024 * 1024
    private enum IndexError: Error { case tooLarge, unsupportedSchema }
    private struct Document: Decodable {
        let schemaVersion: Int
        let generatedAt: String
        let source: String
        let neptune: [UInt32]
        let mullion: [UInt32]
        let venus: [UInt32]
    }

    public init(entries: [UInt32: GraphicsBackend]) {
        self.entries = entries.filter { $0.key != 0 && $0.value.isConcrete }
    }

    public static func load(from url: URL) throws -> SteamGraphicsIndex {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let data = try handle.read(upToCount: maximumSize + 1) ?? Data()
        guard data.count <= maximumSize else { throw IndexError.tooLarge }
        let document = try JSONDecoder().decode(Document.self, from: data)
        guard document.schemaVersion == 1 else { throw IndexError.unsupportedSchema }
        var entries: [UInt32: GraphicsBackend] = [:]
        for (ids, backend) in [(document.venus, GraphicsBackend.venus), (document.mullion, .mullion), (document.neptune, .neptune)] {
            for id in ids where id != 0 { entries[id] = backend }
        }
        return SteamGraphicsIndex(entries: entries)
    }

    public static func loadIfPresent(from url: URL?) -> SteamGraphicsIndex? {
        guard let url else { return nil }
        return try? load(from: url)
    }

    public func backend(for appID: UInt32) -> GraphicsBackend? { entries[appID] }
    public var count: Int { entries.count }
}
