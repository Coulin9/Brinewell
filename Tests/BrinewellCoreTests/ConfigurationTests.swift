import CryptoKit
import Foundation
import XCTest
@testable import BrinewellCore

final class ConfigurationTests: XCTestCase {
    private func host(cpus: Int = 8, memoryGiB: UInt64 = 16, fastestCoreCount: Int? = nil) -> HostCapabilities {
        HostCapabilities(logicalCPUCount: cpus, physicalMemoryBytes: memoryGiB * 1_073_741_824,
                         architecture: "test", metalDeviceName: nil, fastestCoreCount: fastestCoreCount)
    }

    private func withTemporaryDirectory(_ body: (URL) throws -> Void) throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        try body(directory)
    }

    func testLowResourceDefaultsRemainValid() throws {
        for capabilities in [host(cpus: 1, memoryGiB: 0), host(cpus: 2, memoryGiB: 4)] {
            let configuration = VMConfiguration.defaults(for: capabilities)
            XCTAssertEqual(configuration.cpuCount, 1)
            XCTAssertEqual(configuration.memoryGiB, 1)
            XCTAssertNoThrow(try configuration.validate(for: capabilities))
        }
    }

    func testCPUAndMemoryBounds() throws {
        let capabilities = host()
        for cpu in [0, capabilities.maximumCPUs + 1] {
            XCTAssertThrowsError(try VMConfiguration(cpuCount: cpu, memoryGiB: 1).validate(for: capabilities))
        }
        for memory in [0, capabilities.maximumMemoryGiB + 1] {
            XCTAssertThrowsError(try VMConfiguration(cpuCount: 1, memoryGiB: memory).validate(for: capabilities))
        }
        XCTAssertNoThrow(try VMConfiguration(cpuCount: capabilities.maximumCPUs, memoryGiB: capabilities.maximumMemoryGiB).validate(for: capabilities))
    }

    func testDiskPathValidation() throws {
        try withTemporaryDirectory { directory in
            let disk = directory.appendingPathComponent("disk.img")
            try Data("disk".utf8).write(to: disk)
            let capabilities = host()
            XCTAssertNoThrow(try VMConfiguration(cpuCount: 1, memoryGiB: 1).validate(for: capabilities))
            XCTAssertNoThrow(try VMConfiguration(diskImagePath: disk.path, cpuCount: 1, memoryGiB: 1).validate(for: capabilities))
            for path in [directory.path, directory.appendingPathComponent("missing").path, "relative.img"] {
                XCTAssertThrowsError(try VMConfiguration(diskImagePath: path, cpuCount: 1, memoryGiB: 1).validate(for: capabilities))
            }
        }
    }

    func testConfigurationRoundTrip() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("nested/configuration.json"))
            let capabilities = host(fastestCoreCount: 4)
            let configuration = VMConfiguration(cpuCount: 6, memoryGiB: 5)
            try store.save(configuration, for: capabilities)
            XCTAssertEqual(try store.load(for: capabilities), configuration)
        }
    }


    func testLegacyConfigurationDecodesWithGraphicsDefaults() throws {
        let data = Data(#"{"cpuCount":2,"memoryGiB":4}"#.utf8)
        let configuration = try JSONDecoder().decode(VMConfiguration.self, from: data)
        XCTAssertEqual(configuration.cpuCount, 2)
        XCTAssertEqual(configuration.memoryGiB, 4)
        XCTAssertEqual(configuration.graphicsBackend, .automatic)
        XCTAssertNil(configuration.d3dMetalFrameworkPath)
        XCTAssertEqual(configuration.neptuneApertureMiB, 32768)
        XCTAssertFalse(configuration.neptuneMultiRing)
        XCTAssertEqual(configuration.gameHUDLevel, .off)
        XCTAssertEqual(configuration.gameHUDPosition, .topRight)
        XCTAssertNil(configuration.sharedFolder)
        XCTAssertNil(configuration.pendingDiskShrink)
    }

    func testPendingDiskShrinkRoundTrip() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            let capabilities = host()
            let identity = SystemDiskIdentity(diskGUID: UUID(), homePartitionGUID: UUID(), capacityBytes: 128 * 1_073_741_824)
            var configuration = VMConfiguration(cpuCount: 4, memoryGiB: 8)
            configuration.pendingDiskShrink = PendingDiskShrink(diskImagePath: "/tmp/disk.img", capacityGiB: 64, bootStarted: true, identity: identity)
            try store.save(configuration, for: capabilities)
            XCTAssertEqual(try store.load(for: capabilities), configuration)

            configuration.pendingDiskShrink = nil
            try store.save(configuration, for: capabilities)
            XCTAssertNil(try store.load(for: capabilities).pendingDiskShrink)
        }
    }

    func testPendingDiskShrinkWithoutIdentityIsDropped() throws {
        let data = Data(#"{"cpuCount":2,"memoryGiB":4,"pendingDiskShrink":{"diskImagePath":"/tmp/disk.img","capacityGiB":64,"bootStarted":true}}"#.utf8)
        let configuration = try JSONDecoder().decode(VMConfiguration.self, from: data)
        XCTAssertNil(configuration.pendingDiskShrink)
    }

    func testSharedFolderPersistsWhenDisconnectedAndCanBeRemoved() throws {
        try withTemporaryDirectory { directory in
            let share = directory.appendingPathComponent("Mac 分享")
            try FileManager.default.createDirectory(at: share, withIntermediateDirectories: false)
            let folder = VMSharedFolder(path: share.path)
            XCTAssertTrue(folder.readOnly)
            XCTAssertEqual(try folder.resolvedPath(), share.resolvingSymlinksInPath().path)

            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            var configuration = VMConfiguration(cpuCount: 2, memoryGiB: 4, sharedFolder: folder)
            configuration.sharedFolder?.readOnly = false
            try store.save(configuration, for: host())
            try FileManager.default.removeItem(at: share)
            XCTAssertEqual(try store.load(for: host()), configuration)
            XCTAssertThrowsError(try folder.resolvedPath())

            configuration.sharedFolder = nil
            try store.save(configuration, for: host())
            XCTAssertNil(try store.load(for: host()).sharedFolder)
        }
    }

    func testSharedFolderIdentityAndFallback() throws {
        try withTemporaryDirectory { root in
            let fm = FileManager.default
            let first = root.appendingPathComponent("one/name")
            let second = root.appendingPathComponent("two/name")
            try fm.createDirectory(at: first, withIntermediateDirectories: true)
            try fm.createDirectory(at: second, withIntermediateDirectories: true)
            let path = try VMSharedFolder(path: first.path).resolvedPath()
            let id = VMSharedFolder.shareIdentifier(forResolvedPath: path)
            XCTAssertEqual(id.count, 32)
            XCTAssertNotNil(id.range(of: "^[0-9a-f]{32}$", options: .regularExpression))
            XCTAssertEqual(id, VMSharedFolder.shareIdentifier(forResolvedPath: path))
            XCTAssertNotEqual(id, VMSharedFolder.shareIdentifier(forResolvedPath: second.resolvingSymlinksInPath().path))
            let link = root.appendingPathComponent("link")
            try fm.createSymbolicLink(at: link, withDestinationURL: first)
            XCTAssertEqual(id, VMSharedFolder.shareIdentifier(forResolvedPath: try VMSharedFolder(path: link.path).resolvedPath()))
            let volume = try XCTUnwrap(try first.resourceValues(forKeys: [.volumeUUIDStringKey]).volumeUUIDString)
            XCTAssertFalse(volume.isEmpty)
            let renamed = first.deletingLastPathComponent().appendingPathComponent("renamed")
            try fm.moveItem(at: first, to: renamed)
            XCTAssertEqual(id, VMSharedFolder.shareIdentifier(forResolvedPath: renamed.resolvingSymlinksInPath().path))

            let missing = root.appendingPathComponent("missing").path
            let expected = SHA256.hash(data: Data("brinewell-share-v1\0path\0\(missing)".utf8))
                .prefix(16).map { String(format: "%02x", $0) }.joined()
            XCTAssertEqual(VMSharedFolder.shareIdentifier(forResolvedPath: missing), expected)
        }
    }

    func testSharedFolderLabels() {
        let cases = [("/tmp/Hello", "Hello"), ("/tmp/中文", "中文"),
                     ("/tmp/  a\"b\n\t c  ", "a\"b c"), ("/", "Shared Folder"),
                     ("/tmp/\n\t", "Shared Folder"), ("/tmp/e\u{301}", "é"),
                     ("/tmp/" + String(repeating: "界", count: 17), String(repeating: "界", count: 16)),
                     ("/tmp/" + String(repeating: "a", count: 47) + "👨‍👩‍👧", String(repeating: "a", count: 47))]
        for (path, expected) in cases {
            XCTAssertEqual(VMSharedFolder.displayLabel(forResolvedPath: path), expected)
            let hex = VMSharedFolder.displayLabelHex(forResolvedPath: path)
            XCTAssertEqual(hex, expected.utf8.map { String(format: "%02x", $0) }.joined())
            XCTAssertLessThanOrEqual(hex.count, 96)
            XCTAssertNotNil(hex.range(of: "^[0-9a-f]+$", options: .regularExpression))
        }
    }

    func testGraphicsConfigurationRoundTrip() throws {
        let configuration = VMConfiguration(
            cpuCount: 2,
            memoryGiB: 4,
            graphicsBackend: .neptune,
            d3dMetalFrameworkPath: "/tmp/D3DMetal.framework",
            neptuneApertureMiB: 16384,
            neptuneMultiRing: true,
            gameHUDLevel: .compact,
            gameHUDPosition: .bottomLeft
        )
        let data = try JSONEncoder().encode(configuration)
        XCTAssertEqual(try JSONDecoder().decode(VMConfiguration.self, from: data), configuration)
    }

    func testNewConfigurationsDefaultToAutomatic() {
        XCTAssertEqual(VMConfiguration(cpuCount: 2, memoryGiB: 4).graphicsBackend, .automatic)
        XCTAssertEqual(VMConfiguration.defaults(for: host()).graphicsBackend, .automatic)
        XCTAssertEqual(GraphicsBackend.allCases, [.automatic, .neptune, .mullion, .venus])
    }

    func testGameHUDCanPersistWithoutChangingStoredVMSettings() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            var stored = VMConfiguration(
                diskImagePath: directory.appendingPathComponent("disconnected.img").path,
                cpuCount: 3, memoryGiB: 5, graphicsBackend: .venus,
                d3dMetalFrameworkPath: "/tmp/D3DMetal.framework", neptuneApertureMiB: 16384,
                neptuneMultiRing: true,
                display: VMDisplayConfiguration(width: 1920, height: 1080, refreshRate: 90),
                sharedFolder: VMSharedFolder(path: "/tmp/Mac 分享", readOnly: false),
                network: VMNetworkConfiguration(proxyEnabled: true, proxyURL: "https://localhost:8443")
            )
            try store.save(stored, for: host())
            for level in GameHUDLevel.allCases {
                for position in GameHUDPosition.allCases {
                    try store.setGameHUD(level: level, position: position, for: host())
                    stored.gameHUDLevel = level
                    stored.gameHUDPosition = position
                    XCTAssertEqual(try store.load(for: host()), stored)
                }
            }
            stored.cpuCount = 4
            stored.memoryGiB = 6
            try store.save(stored, for: host())
            try store.setGameHUD(level: .off, position: .topRight, for: host())
            stored.gameHUDLevel = .off
            stored.gameHUDPosition = .topRight
            XCTAssertEqual(try store.load(for: host()), stored)
        }
    }

    func testGraphicsBackendPersistsWithoutChangingStoredVMSettings() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            var stored = VMConfiguration(
                cpuCount: 3, memoryGiB: 5, graphicsBackend: .venus,
                display: VMDisplayConfiguration(width: 1920, height: 1080, refreshRate: 90),
                sharedFolder: VMSharedFolder(path: "/tmp/share", readOnly: true),
                network: VMNetworkConfiguration(proxyEnabled: true, proxyURL: "https://localhost:8443")
            )
            try store.save(stored, for: host())
            for backend in [GraphicsBackend.mullion, .neptune, .venus] {
                try store.setGraphicsBackend(backend, for: host())
                stored.graphicsBackend = backend
                XCTAssertEqual(try store.load(for: host()), stored)
            }
        }
    }

    func testGameHUDLevelsAndPositionsRoundTripWithStableValues() throws {
        let levels: [(GameHUDLevel, Int)] = [(.off, 0), (.fps, 1), (.compact, 2), (.detailed, 3), (.full, 4)]
        let positions: [(GameHUDPosition, String)] = [
            (.topLeft, "top-left"), (.topRight, "top-right"),
            (.bottomLeft, "bottom-left"), (.bottomRight, "bottom-right"),
        ]
        XCTAssertEqual(GameHUDLevel.allCases, levels.map(\.0))
        XCTAssertEqual(GameHUDPosition.allCases, positions.map(\.0))
        for (level, number) in levels {
            for (position, name) in positions {
                let configuration = VMConfiguration(cpuCount: 2, memoryGiB: 4,
                                                    gameHUDLevel: level, gameHUDPosition: position)
                let data = try JSONEncoder().encode(configuration)
                let json = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
                XCTAssertEqual(json["gameHUDLevel"] as? Int, number)
                XCTAssertEqual(json["gameHUDPosition"] as? String, name)
                XCTAssertNil(json["gameHUDEnabled"])
                XCTAssertEqual(try JSONDecoder().decode(VMConfiguration.self, from: data), configuration)
            }
        }
    }

    func testLegacyGameHUDStatesMigrateAndSaveOnlyNewFields() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            let legacyStates: [(Bool, GameHUDLevel)] = [(true, .compact), (false, .off)]
            for (enabled, expected) in legacyStates {
                let data = Data("{\"cpuCount\":3,\"memoryGiB\":5,\"gameHUDEnabled\":\(enabled)}".utf8)
                try data.write(to: store.url)
                let configuration = try store.load(for: host())
                XCTAssertEqual(configuration.gameHUDLevel, expected)
                XCTAssertEqual(configuration.gameHUDPosition, .topRight)
                XCTAssertEqual(configuration.cpuCount, 3)
                XCTAssertEqual(configuration.memoryGiB, 5)
                try store.save(configuration, for: host())
                let saved = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: store.url)) as? [String: Any])
                XCTAssertNil(saved["gameHUDEnabled"])
                XCTAssertEqual(saved["gameHUDLevel"] as? Int, expected.rawValue)
                XCTAssertEqual(try store.load(for: host()), configuration)
            }
        }
    }

    func testExplicitGameHUDLevelTakesPrecedenceOverLegacyState() throws {
        for legacy in ["true", "false", "\"obsolete\""] {
            for level in GameHUDLevel.allCases {
                let data = Data("{\"cpuCount\":2,\"memoryGiB\":4,\"gameHUDEnabled\":\(legacy),\"gameHUDLevel\":\(level.rawValue),\"gameHUDPosition\":\"bottom-left\"}".utf8)
                let configuration = try JSONDecoder().decode(VMConfiguration.self, from: data)
                XCTAssertEqual(configuration.gameHUDLevel, level)
                XCTAssertEqual(configuration.gameHUDPosition, .bottomLeft)
            }
        }
    }

    func testInvalidGameHUDConfigurationIsRejectedWithoutOverwritingData() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            for fields in [
                #""gameHUDLevel":-1"#, #""gameHUDLevel":5"#, #""gameHUDLevel":"full""#,
                #""gameHUDLevel":1.5"#, #""gameHUDLevel":true"#,
                #""gameHUDLevel":9,"gameHUDEnabled":true"#,
                #""gameHUDPosition":"center""#, #""gameHUDPosition":0"#,
                #""gameHUDEnabled":"true""#,
            ] {
                let data = Data("{\"cpuCount\":2,\"memoryGiB\":4,\(fields)}".utf8)
                _ = try JSONSerialization.jsonObject(with: data)
                try data.write(to: store.url)
                XCTAssertThrowsError(try store.load(for: host()), fields)
                XCTAssertThrowsError(try store.setGameHUD(level: .compact, position: .topLeft, for: host()), fields)
                XCTAssertEqual(try Data(contentsOf: store.url), data)
            }
        }
    }

    func testAutomaticConfigurationPreservesSettings() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            let data = Data(#"{"cpuCount":2,"memoryGiB":4,"graphicsBackend":"automatic","d3dMetalFrameworkPath":"/tmp/D3DMetal.framework","neptuneApertureMiB":16384,"neptuneMultiRing":true}"#.utf8)
            try data.write(to: store.url)
            let configuration = try store.load(for: host())
            XCTAssertEqual(configuration.graphicsBackend, .automatic)
            XCTAssertEqual(configuration.cpuCount, 2)
            XCTAssertEqual(configuration.memoryGiB, 4)
            XCTAssertEqual(configuration.d3dMetalFrameworkPath, "/tmp/D3DMetal.framework")
            XCTAssertEqual(configuration.neptuneApertureMiB, 16384)
            XCTAssertTrue(configuration.neptuneMultiRing)
            try store.save(configuration, for: host())
            let saved = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: store.url)) as? [String: Any])
            XCTAssertEqual(saved["graphicsBackend"] as? String, "automatic")
            XCTAssertEqual(try store.load(for: host()), configuration)
        }
    }

    func testExplicitGraphicsBackendsSurviveRoundTrip() throws {
        for backend in GraphicsBackend.allCases {
            let configuration = VMConfiguration(cpuCount: 2, memoryGiB: 4, graphicsBackend: backend)
            let data = try JSONEncoder().encode(configuration)
            XCTAssertEqual(try JSONDecoder().decode(VMConfiguration.self, from: data), configuration)
        }
    }

    func testUnknownGraphicsBackendIsRejectedWithoutOverwritingConfiguration() throws {
        try withTemporaryDirectory { directory in
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            let data = Data(#"{"cpuCount":2,"memoryGiB":4,"graphicsBackend":"unknown"}"#.utf8)
            try data.write(to: store.url)
            XCTAssertThrowsError(try store.load(for: host()))
            XCTAssertEqual(try Data(contentsOf: store.url), data)
        }
    }

    func testNeptuneApertureBounds() throws {
        let capabilities = host()
        XCTAssertThrowsError(try VMConfiguration(cpuCount: 1, memoryGiB: 1, neptuneApertureMiB: 4096)
            .validate(for: capabilities))
        XCTAssertThrowsError(try VMConfiguration(cpuCount: 1, memoryGiB: 1, neptuneApertureMiB: 65536 + 8192)
            .validate(for: capabilities))
        XCTAssertNoThrow(try VMConfiguration(cpuCount: 1, memoryGiB: 1, neptuneApertureMiB: 8192)
            .validate(for: capabilities))
    }

    func testMissingConfigurationReturnsDefaultsWithoutCreatingFile() throws {
        try withTemporaryDirectory { directory in
            let url = directory.appendingPathComponent("missing/configuration.json")
            let capabilities = host(cpus: 18, fastestCoreCount: 6)
            let configuration = try ConfigurationStore(url: url).load(for: capabilities)
            XCTAssertEqual(configuration, .defaults(for: capabilities))
            XCTAssertEqual(configuration.cpuCount, 6)
            XCTAssertFalse(FileManager.default.fileExists(atPath: url.path))
            XCTAssertFalse(FileManager.default.fileExists(atPath: url.deletingLastPathComponent().path))
        }
    }

    func testCorruptConfigurationIsNotOverwritten() throws {
        try withTemporaryDirectory { directory in
            let url = directory.appendingPathComponent("configuration.json")
            let original = Data("not JSON".utf8)
            try original.write(to: url)
            XCTAssertThrowsError(try ConfigurationStore(url: url).load(for: host()))
            XCTAssertEqual(try Data(contentsOf: url), original)
        }
    }

    func testInvalidConfigurationDoesNotReplaceExistingData() throws {
        try withTemporaryDirectory { directory in
            let url = directory.appendingPathComponent("configuration.json")
            let store = ConfigurationStore(url: url)
            let valid = VMConfiguration(cpuCount: 2, memoryGiB: 4)
            try store.save(valid, for: host())
            let original = try Data(contentsOf: url)
            let invalid = VMConfiguration(cpuCount: 0, memoryGiB: 4)
            XCTAssertThrowsError(try store.save(invalid, for: host()))
            XCTAssertEqual(try Data(contentsOf: url), original)

            let invalidData = try JSONEncoder().encode(invalid)
            try invalidData.write(to: url)
            XCTAssertThrowsError(try store.load(for: host()))
            XCTAssertEqual(try Data(contentsOf: url), invalidData)
        }
    }

    func testDecodeReturnsStoredValuesWhenHostValidationFails() throws {
        try withTemporaryDirectory { directory in
            let url = directory.appendingPathComponent("configuration.json")
            let stored = VMConfiguration(
                cpuCount: host().maximumCPUs + 1,
                memoryGiB: 5
            )
            try JSONEncoder().encode(stored).write(to: url)
            let store = ConfigurationStore(url: url)
            XCTAssertThrowsError(try store.load(for: host()))
            XCTAssertEqual(try store.decode(), stored)
        }
    }

    func testRemovedDiskKeepsSettingsLoadableAndEditable() throws {
        try withTemporaryDirectory { directory in
            let disk = directory.appendingPathComponent("disk.img")
            try Data("disk".utf8).write(to: disk)
            let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
            var configuration = VMConfiguration(diskImagePath: disk.path, cpuCount: 3, memoryGiB: 5)
            try store.save(configuration, for: host())
            try FileManager.default.removeItem(at: disk)

            XCTAssertEqual(try store.load(for: host()), configuration)
            XCTAssertThrowsError(try configuration.validate(for: host()))
            configuration.memoryGiB = 6
            try store.save(configuration, for: host())
            XCTAssertEqual(try store.load(for: host()), configuration)
            XCTAssertEqual(try store.load(for: host()).diskImagePath, disk.path)
        }
    }
}
