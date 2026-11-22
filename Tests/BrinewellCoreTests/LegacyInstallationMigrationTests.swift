import Foundation
import XCTest
@testable import BrinewellCore

final class LegacyInstallationMigrationTests: XCTestCase {
    private var suiteNames: [String] = []

    private func withTemporaryRoots(_ body: (URL, URL) throws -> Void) throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let support = root.appendingPathComponent("Application Support", isDirectory: true)
        let caches = root.appendingPathComponent("Caches", isDirectory: true)
        try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: caches, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try body(support, caches)
    }

    private func suite(_ prefix: String) -> (name: String, defaults: UserDefaults) {
        let name = "\(prefix).\(UUID().uuidString)"
        suiteNames.append(name)
        return (name, UserDefaults(suiteName: name)!)
    }

    override func tearDown() {
        for name in suiteNames {
            UserDefaults().removePersistentDomain(forName: name)
        }
        suiteNames = []
        super.tearDown()
    }

    private func migrate(support: URL, caches: URL,
                         defaults: (name: String, defaults: UserDefaults)? = nil,
                         legacyDefaults: (name: String, defaults: UserDefaults)? = nil)
        -> LegacyInstallationMigration.Report {
        let new = defaults ?? suite("new")
        let legacy = legacyDefaults ?? suite("legacy")
        return LegacyInstallationMigration.migrateIfNeeded(
            applicationSupport: support, caches: caches,
            defaults: new.defaults, legacyDefaults: legacy.defaults,
            legacyDefaultsDomain: legacy.name,
            newDefaultsDomain: new.name)
    }

    private func writeConfiguration(_ object: [String: Any], to url: URL) throws {
        let data = try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url)
    }

    private func configurationDictionary(at url: URL) -> [String: Any]? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    func testNothingToMigrate() throws {
        try withTemporaryRoots { support, caches in
            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .nothingToDo)
            XCTAssertFalse(report.needsAttention)
            XCTAssertFalse(FileManager.default.fileExists(
                atPath: support.appendingPathComponent("Brinewell").path))
        }
    }

    func testMigratesDirectoryAndRelocatesOnlyRootedPaths() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            let disk = old.appendingPathComponent("VirtualMachines/SteamOS.img")
            try files.createDirectory(at: disk.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data([1, 2, 3]).write(to: disk)
            let external = support.deletingLastPathComponent().appendingPathComponent("AppleDeck Share")
            try files.createDirectory(at: external, withIntermediateDirectories: true)
            let framework = support.deletingLastPathComponent().appendingPathComponent("Frameworks/AppleDeckD3DMetal.framework")
            try writeConfiguration([
                "cpuCount": 4,
                "memoryGiB": 8,
                "diskImagePath": disk.path,
                "d3dMetalFrameworkPath": framework.path,
                "sharedFolder": ["path": external.path, "readOnly": true],
                "pendingDiskShrink": [
                    "diskImagePath": disk.path, "capacityGiB": 64, "bootStarted": false,
                    "identity": ["diskGUID": "0000", "homeUUID": "1111"],
                ],
                "futureUnknownKey": ["nested": ["keep", 42]],
            ], to: old.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .migrated)
            XCTAssertTrue(report.rewroteConfiguration)
            XCTAssertFalse(files.fileExists(atPath: old.path))
            let new = support.appendingPathComponent("Brinewell", isDirectory: true)
            let movedDisk = new.appendingPathComponent("VirtualMachines/SteamOS.img")
            XCTAssertEqual(try Data(contentsOf: movedDisk), Data([1, 2, 3]))

            let stored = try XCTUnwrap(configurationDictionary(
                at: new.appendingPathComponent("configuration.json")))
            let canonicalNew = new.resolvingSymlinksInPath().path
            XCTAssertEqual(stored["diskImagePath"] as? String, "\(canonicalNew)/VirtualMachines/SteamOS.img")
            // Paths outside the legacy root never change, even when their name
            // happens to contain "AppleDeck".
            XCTAssertEqual(stored["d3dMetalFrameworkPath"] as? String, framework.path)
            XCTAssertEqual((stored["sharedFolder"] as? [String: Any])?["path"] as? String, external.path)
            XCTAssertEqual(
                (stored["pendingDiskShrink"] as? [String: Any])?["diskImagePath"] as? String,
                "\(canonicalNew)/VirtualMachines/SteamOS.img")
            // Unknown keys survive untouched.
            XCTAssertEqual(
                (stored["futureUnknownKey"] as? [String: Any])?["nested"] as? [AnyHashable],
                ["keep", 42])
            XCTAssertEqual(stored["cpuCount"] as? Int, 4)
        }
    }

    func testConfigurationStaysPrivate() throws {
        try withTemporaryRoots { support, caches in
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            try FileManager.default.createDirectory(at: old, withIntermediateDirectories: true)
            try writeConfiguration(["cpuCount": 4, "memoryGiB": 8,
                                    "diskImagePath": old.appendingPathComponent("disk.img").path],
                                   to: old.appendingPathComponent("configuration.json"))
            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .migrated)
            let attributes = try FileManager.default.attributesOfItem(
                atPath: support.appendingPathComponent("Brinewell/configuration.json").path)
            XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.int16Value, 0o600)
        }
    }

    func testSecondRunIsNoOp() throws {
        try withTemporaryRoots { support, caches in
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            try FileManager.default.createDirectory(at: old, withIntermediateDirectories: true)
            try writeConfiguration(["cpuCount": 4, "memoryGiB": 8,
                                    "diskImagePath": old.appendingPathComponent("disk.img").path],
                                   to: old.appendingPathComponent("configuration.json"))
            _ = migrate(support: support, caches: caches)
            let configurationURL = support.appendingPathComponent("Brinewell/configuration.json")
            let afterFirst = try Data(contentsOf: configurationURL)

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .nothingToDo)
            XCTAssertFalse(report.rewroteConfiguration)
            XCTAssertEqual(try Data(contentsOf: configurationURL), afterFirst)
        }
    }

    func testInterruptedMoveResumesRewrite() throws {
        try withTemporaryRoots { support, caches in
            // Simulate a crash after the rename but before the path rewrite:
            // the new directory exists with old-root paths still inside.
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            let new = support.appendingPathComponent("Brinewell", isDirectory: true)
            try FileManager.default.createDirectory(at: new, withIntermediateDirectories: true)
            let disk = old.appendingPathComponent("VirtualMachines/SteamOS.img").path
            try writeConfiguration(["cpuCount": 4, "memoryGiB": 8, "diskImagePath": disk],
                                   to: new.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .migrated)
            XCTAssertTrue(report.rewroteConfiguration)
            let stored = try XCTUnwrap(configurationDictionary(
                at: new.appendingPathComponent("configuration.json")))
            XCTAssertEqual(stored["diskImagePath"] as? String,
                           "\(new.resolvingSymlinksInPath().path)/VirtualMachines/SteamOS.img")
        }
    }

    func testConflictLeavesEverythingAlone() throws {
        try withTemporaryRoots { support, caches in
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            let new = support.appendingPathComponent("Brinewell", isDirectory: true)
            try FileManager.default.createDirectory(at: old, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: new, withIntermediateDirectories: true)
            try writeConfiguration(["cpuCount": 2], to: old.appendingPathComponent("configuration.json"))
            try writeConfiguration(["cpuCount": 8], to: new.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .conflict)
            XCTAssertTrue(report.needsAttention)
            XCTAssertTrue(FileManager.default.fileExists(
                atPath: old.appendingPathComponent("configuration.json").path))
            XCTAssertEqual(configurationDictionary(at: old.appendingPathComponent("configuration.json"))?["cpuCount"] as? Int, 2)
            XCTAssertEqual(configurationDictionary(at: new.appendingPathComponent("configuration.json"))?["cpuCount"] as? Int, 8)
        }
    }

    func testCorruptConfigurationMovesButIsNotRewritten() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            try files.createDirectory(at: old, withIntermediateDirectories: true)
            let corrupt = Data("not json {".utf8)
            try corrupt.write(to: old.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            // The data is at the new path, but the launch flow must not treat
            // an unreadable configuration as a clean migration.
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.needsAttention)
            XCTAssertTrue(report.invalidConfiguration)
            XCTAssertFalse(report.unreadableConfiguration)
            XCTAssertTrue(report.movedData)
            XCTAssertFalse(report.rewroteConfiguration)
            XCTAssertFalse(files.fileExists(atPath: old.path))
            XCTAssertEqual(try Data(contentsOf: support.appendingPathComponent("Brinewell/configuration.json")),
                           corrupt)
        }
    }

    func testFailedRewriteReportsNewLocationAndLeavesFileUntouched() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            try files.createDirectory(at: old, withIntermediateDirectories: true)
            let diskPath = old.appendingPathComponent("disk.img").path
            try writeConfiguration(["diskImagePath": diskPath],
                                   to: old.appendingPathComponent("configuration.json"))
            // A read-only directory makes the staged rewrite fail after the
            // rename; the original configuration must stay byte-identical.
            try files.setAttributes([.posixPermissions: 0o500], ofItemAtPath: old.path)
            defer {
                let new = support.appendingPathComponent("Brinewell")
                try? files.setAttributes([.posixPermissions: 0o700], ofItemAtPath: new.path)
            }

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.needsAttention)
            XCTAssertFalse(report.invalidConfiguration)
            XCTAssertFalse(report.unreadableConfiguration)
            XCTAssertFalse(files.fileExists(atPath: old.path))
            let stored = try XCTUnwrap(configurationDictionary(
                at: support.appendingPathComponent("Brinewell/configuration.json")))
            XCTAssertEqual(stored["diskImagePath"] as? String, diskPath)
        }
    }

    func testCachesMoveBestEffort() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let oldCache = caches.appendingPathComponent("AppleDeck/Installers", isDirectory: true)
            try files.createDirectory(at: oldCache, withIntermediateDirectories: true)
            try Data([9]).write(to: oldCache.appendingPathComponent("blob"))

            let report = migrate(support: support, caches: caches)
            XCTAssertTrue(report.cachesMoved)
            XCTAssertFalse(files.fileExists(atPath: caches.appendingPathComponent("AppleDeck").path))
            XCTAssertEqual(try Data(contentsOf: caches.appendingPathComponent("Brinewell/Installers/blob")), Data([9]))
        }
    }

    func testLanguageImportRunsOnce() throws {
        try withTemporaryRoots { support, caches in
            let new = suite("new")
            let legacy = suite("legacy")
            legacy.defaults.set(["zh-Hans"], forKey: "AppleLanguages")
            legacy.defaults.set("secret", forKey: "UnrelatedKey")

            _ = migrate(support: support, caches: caches, defaults: new, legacyDefaults: legacy)
            var domain = new.defaults.persistentDomain(forName: new.name)
            XCTAssertEqual(domain?["AppleLanguages"] as? [String], ["zh-Hans"])
            XCTAssertEqual(domain?[LegacyInstallationMigration.defaultsImportedKey] as? Bool, true)
            XCTAssertNil(domain?["UnrelatedKey"])

            // A changed legacy preference must not overwrite the imported one.
            new.defaults.set(["en"], forKey: "AppleLanguages")
            legacy.defaults.set(["zh-Hans"], forKey: "AppleLanguages")
            _ = migrate(support: support, caches: caches, defaults: new, legacyDefaults: legacy)
            domain = new.defaults.persistentDomain(forName: new.name)
            XCTAssertEqual(domain?["AppleLanguages"] as? [String], ["en"])
        }
    }

    func testLanguageImportSkippedWhenAlreadySet() throws {
        try withTemporaryRoots { support, caches in
            let new = suite("new")
            new.defaults.set(["en"], forKey: "AppleLanguages")
            let legacy = suite("legacy")
            legacy.defaults.set(["zh-Hans"], forKey: "AppleLanguages")

            _ = migrate(support: support, caches: caches, defaults: new, legacyDefaults: legacy)
            let domain = new.defaults.persistentDomain(forName: new.name)
            XCTAssertEqual(domain?["AppleLanguages"] as? [String], ["en"])
            XCTAssertEqual(domain?[LegacyInstallationMigration.defaultsImportedKey] as? Bool, true)
        }
    }

    func testLanguageImportWritesNothingWhenLegacyFollowedSystem() throws {
        try withTemporaryRoots { support, caches in
            let new = suite("new")
            let legacy = suite("legacy")
            // No AppleLanguages in the legacy persistent domain: the old app
            // followed the system. The global domain's AppleLanguages must not
            // leak in, and the one-time flag is still set so a later explicit
            // legacy value is never imported afterwards.
            _ = migrate(support: support, caches: caches, defaults: new, legacyDefaults: legacy)
            var domain = new.defaults.persistentDomain(forName: new.name)
            XCTAssertNil(domain?["AppleLanguages"])
            XCTAssertEqual(domain?[LegacyInstallationMigration.defaultsImportedKey] as? Bool, true)

            legacy.defaults.set(["zh-Hans"], forKey: "AppleLanguages")
            _ = migrate(support: support, caches: caches, defaults: new, legacyDefaults: legacy)
            domain = new.defaults.persistentDomain(forName: new.name)
            XCTAssertNil(domain?["AppleLanguages"])
        }
    }

    /// `{}` is valid JSON but not a loadable configuration (missing
    /// cpuCount/memoryGiB); the file must not count as a clean migration.
    func testUndecodableConfigurationNeedsRepair() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            try files.createDirectory(at: old, withIntermediateDirectories: true)
            let original = Data("{}".utf8)
            try original.write(to: old.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.needsAttention)
            XCTAssertTrue(report.invalidConfiguration)
            XCTAssertFalse(report.unreadableConfiguration)
            XCTAssertTrue(report.movedData)
            XCTAssertFalse(files.fileExists(atPath: old.path))
            XCTAssertEqual(try Data(contentsOf: support.appendingPathComponent("Brinewell/configuration.json")),
                           original)
        }
    }

    /// A symlink (or file) at the legacy path is an occupied path, not a
    /// conflict between two data folders.
    func testLegacyPathOccupiedBySymlink() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let target = support.appendingPathComponent("Elsewhere", isDirectory: true)
            try files.createDirectory(at: target, withIntermediateDirectories: true)
            let old = support.appendingPathComponent("AppleDeck")
            try files.createSymbolicLink(at: old, withDestinationURL: target)

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .pathOccupied)
            XCTAssertTrue(report.needsAttention)
            XCTAssertFalse(report.movedData)
            XCTAssertNotNil(try? files.destinationOfSymbolicLink(atPath: old.path))
            XCTAssertFalse(files.fileExists(
                atPath: support.appendingPathComponent("Brinewell").path))
        }
    }

    /// A native Brinewell install (no legacy directory ever existed) with a
    /// broken configuration is a repair case, but no data moved — the alert
    /// must not claim AppleDeck data was migrated.
    func testNativeInstallWithBrokenConfigDidNotMoveData() throws {
        try withTemporaryRoots { support, caches in
            let new = support.appendingPathComponent("Brinewell", isDirectory: true)
            try FileManager.default.createDirectory(at: new, withIntermediateDirectories: true)
            try Data("not json {".utf8).write(to: new.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.needsAttention)
            XCTAssertTrue(report.invalidConfiguration)
            XCTAssertFalse(report.movedData)
        }
    }

    /// Same as above, but with JSON that parses yet cannot decode into a
    /// configuration.
    func testNativeInstallWithUndecodableConfigDidNotMoveData() throws {
        try withTemporaryRoots { support, caches in
            let new = support.appendingPathComponent("Brinewell", isDirectory: true)
            try FileManager.default.createDirectory(at: new, withIntermediateDirectories: true)
            try writeConfiguration(["unexpected": true],
                                   to: new.appendingPathComponent("configuration.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.invalidConfiguration)
            XCTAssertFalse(report.movedData)
        }
    }

    /// A path under the legacy root makes the file a rewrite candidate, but
    /// the result must still decode. When it cannot (missing memoryGiB), the
    /// rewritten bytes must never replace the user's file.
    func testUndecodableRewriteCandidateLeavesFileUntouched() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let old = support.appendingPathComponent("AppleDeck", isDirectory: true)
            try files.createDirectory(at: old, withIntermediateDirectories: true)
            let configurationURL = old.appendingPathComponent("configuration.json")
            try writeConfiguration(["cpuCount": 4,
                                    "diskImagePath": old.appendingPathComponent("disk.img").path],
                                   to: configurationURL)
            let original = try Data(contentsOf: configurationURL)

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.needsAttention)
            XCTAssertTrue(report.invalidConfiguration)
            XCTAssertFalse(report.unreadableConfiguration)
            XCTAssertFalse(report.rewroteConfiguration)
            XCTAssertFalse(files.fileExists(atPath: old.path))
            XCTAssertEqual(try Data(contentsOf: support.appendingPathComponent("Brinewell/configuration.json")),
                           original)
        }
    }

    /// A dangling symlink (for example an unmounted volume) is invisible to
    /// fileExists but is still an occupied path that must block migration.
    func testLegacyPathOccupiedByDanglingSymlink() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let old = support.appendingPathComponent("AppleDeck")
            try files.createSymbolicLink(
                at: old, withDestinationURL: support.appendingPathComponent("Unmounted Volume"))
            XCTAssertFalse(files.fileExists(atPath: old.path))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .pathOccupied)
            XCTAssertTrue(report.needsAttention)
            XCTAssertFalse(report.movedData)
            XCTAssertNotNil(try? files.destinationOfSymbolicLink(atPath: old.path))
            XCTAssertFalse(files.fileExists(
                atPath: support.appendingPathComponent("Brinewell").path))
        }
    }

    /// A dangling or unreadable configuration symlink inside the new support
    /// directory is a repair case, not a clean native install.
    func testDanglingConfigurationSymlinkNeedsRepair() throws {
        try withTemporaryRoots { support, caches in
            let files = FileManager.default
            let new = support.appendingPathComponent("Brinewell", isDirectory: true)
            try files.createDirectory(at: new, withIntermediateDirectories: true)
            try files.createSymbolicLink(
                at: new.appendingPathComponent("configuration.json"),
                withDestinationURL: new.appendingPathComponent("missing.json"))

            let report = migrate(support: support, caches: caches)
            XCTAssertEqual(report.status, .movedButConfigNeedsRepair)
            XCTAssertTrue(report.needsAttention)
            XCTAssertTrue(report.unreadableConfiguration)
            XCTAssertFalse(report.invalidConfiguration)
            XCTAssertFalse(report.movedData)
            XCTAssertNotNil(try? files.destinationOfSymbolicLink(
                atPath: new.appendingPathComponent("configuration.json").path))
        }
    }
}
