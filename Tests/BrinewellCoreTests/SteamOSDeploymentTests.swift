import Darwin
import Foundation
import XCTest
@testable import BrinewellCore

final class SteamOSDeploymentTests: XCTestCase {
    private func withDirectory(_ body: (URL) throws -> Void) throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        try body(directory)
    }

    func testBlankDiskIsSparseAndNotYetAUsableSystem() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            var attributes = stat()
            XCTAssertEqual(stat(disk.path, &attributes), 0)
            XCTAssertEqual(attributes.st_size, 64 * 1_073_741_824)
            XCTAssertLessThan(attributes.st_blocks * 512, 1024 * 1024)
            XCTAssertThrowsError(try SteamOSDisk.validateSystem(at: disk))
            try writeSystemSignatures(disk)
            XCTAssertNoThrow(try SteamOSDisk.validateSystem(at: disk))
            XCTAssertThrowsError(try SteamOSDisk.validateInstaller(at: disk)) { error in
                XCTAssertTrue(error.localizedDescription.contains("Choose Existing System Disk"))
            }
        }
    }

    func testCreationNeverOverwritesExistingFilesOrSymlinks() throws {
        try withDirectory { directory in
            let existing = directory.appendingPathComponent("existing.img")
            let original = Data("user disk".utf8)
            try original.write(to: existing)
            XCTAssertThrowsError(try SteamOSDisk.createBlank(at: existing, capacityGiB: 128))
            XCTAssertEqual(try Data(contentsOf: existing), original)
            let link = directory.appendingPathComponent("link.img")
            try FileManager.default.createSymbolicLink(at: link, withDestinationURL: existing)
            XCTAssertThrowsError(try SteamOSDisk.createBlank(at: link, capacityGiB: 128))
            XCTAssertEqual(try Data(contentsOf: existing), original)
        }
    }

    func testGPTCorruptionAndTruncationAreRejected() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("disk.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let file = try FileHandle(forUpdating: disk)
            defer { try? file.close() }
            try file.seek(toOffset: 512 + 16)
            let checksumByte = try XCTUnwrap(file.read(upToCount: 1)?.first)
            try file.seek(toOffset: 512 + 16)
            try file.write(contentsOf: Data([checksumByte ^ 0xff]))
            XCTAssertThrowsError(try SteamOSDisk.validateSystem(at: disk))
            try file.truncate(atOffset: 2048)
            XCTAssertThrowsError(try SteamOSDisk.validateSystem(at: disk))
        }
    }

    func testRecoveryImageIsAcceptedWithoutBeingWritten() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("recovery.img")
            try makeRecovery(disk)
            let before = try Data(contentsOf: disk)
            XCTAssertNoThrow(try SteamOSDisk.validateInstaller(at: disk))
            XCTAssertThrowsError(try SteamOSDisk.validateSystem(at: disk)) { error in
                XCTAssertTrue(error.localizedDescription.contains("Fresh Install"))
            }
            XCTAssertEqual(try Data(contentsOf: disk), before)
        }
    }

    func testTruncatedCompressedInstallerLeavesSourceAndNoOutput() throws {
        try withDirectory { directory in
            let archive = directory.appendingPathComponent("broken.img.bz2")
            let bytes = Data("BZh9incomplete archive".utf8)
            try bytes.write(to: archive)
            let output = directory.appendingPathComponent("installer.img")
            XCTAssertThrowsError(try SteamOSPreparation().extract(archive: archive, to: output))
            XCTAssertEqual(try Data(contentsOf: archive), bytes)
            XCTAssertFalse(FileManager.default.fileExists(atPath: output.path))
        }
    }

    func testCompressedInstallerRoundTripKeepsArchiveUnchanged() throws {
        try withDirectory { directory in
            let original = directory.appendingPathComponent("original.img")
            try makeRecovery(original)
            let archive = directory.appendingPathComponent("recovery.img.bz2")
            XCTAssertTrue(FileManager.default.createFile(atPath: archive.path, contents: nil))
            let output = try FileHandle(forWritingTo: archive)
            let compressor = Process()
            compressor.executableURL = URL(fileURLWithPath: "/usr/bin/bzip2")
            compressor.arguments = ["-c", original.path]
            compressor.standardOutput = output
            try compressor.run()
            compressor.waitUntilExit()
            try output.close()
            XCTAssertEqual(compressor.terminationStatus, 0)
            let before = try Data(contentsOf: archive)
            let extracted = directory.appendingPathComponent("extracted.img")
            try SteamOSPreparation().extract(archive: archive, to: extracted)
            XCTAssertEqual(try Data(contentsOf: extracted), try Data(contentsOf: original))
            XCTAssertEqual(try Data(contentsOf: archive), before)
        }
    }

    func testHelperSuccessRequiresStdoutAcknowledgementAndZeroExit() throws {
        try withDirectory { directory in
            let target = directory.appendingPathComponent("target.img")
            let source = directory.appendingPathComponent("source.img")
            try makeRecovery(source)
            let guid = try SteamOSDisk.createBlank(at: target, capacityGiB: 64)
            try writeSystemSignatures(target)
            let resources = directory.appendingPathComponent("Runtime")
            try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: false)
            try Data().write(to: resources.appendingPathComponent("initramfs.cpio.gz"))
            let helper = directory.appendingPathComponent("helper")
            let cases = [
                ("exit 0", false),
                ("echo 'BRINEWELL prepare done' >&2\nexit 0", false),
                ("echo 'BRINEWELL prepare done'\nexit 1", false),
                ("echo 'BRINEWELL prepare done'\nexit 0", true),
            ]
            for (script, succeeds) in cases {
                try Data("#!/bin/sh\n\(script)\n".utf8).write(to: helper)
                try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: helper.path)
                let prepare = {
                    try SteamOSPreparation().prepare(source: source, target: target, diskGUID: guid,
                                                     helper: helper, resources: resources,
                                                     runDirectory: directory) { _ in }
                }
                if succeeds { XCTAssertNoThrow(try prepare()) }
                else { XCTAssertThrowsError(try prepare()) }
            }
        }
    }

    func testDownloadResumeRequiresSameStrongIdentityAndBoundedLength() {
        let url = SteamOSRelease.current.url
        let first = SteamOSDownload.Identity(url: url, bytes: 100, etag: "\"one\"", modified: "today")
        XCTAssertTrue(first.canResume(first, received: 30))
        XCTAssertTrue(first.canResume(first, received: 100))
        XCTAssertFalse(first.canResume(first, received: 101))
        XCTAssertFalse(first.canResume(first, received: 0))
        XCTAssertFalse(first.canResume(.init(url: url, bytes: 100, etag: "\"two\"", modified: "today"), received: 30))
        XCTAssertFalse(first.canResume(.init(url: url, bytes: 200, etag: "\"one\"", modified: "today"), received: 30))
        let noValidator = SteamOSDownload.Identity(url: url, bytes: 100, etag: "W/\"one\"", modified: nil)
        XCTAssertFalse(noValidator.canResume(noValidator, received: 30))
        let modified = SteamOSDownload.Identity(url: url, bytes: 100, etag: nil, modified: "today")
        XCTAssertTrue(modified.canResume(modified, received: 30))
    }

    func testGrowSystemExpandsImageAndKeepsHomeData() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let oldSize = UInt64(64) * 1_073_741_824
            let oldLast = oldSize / 512 - 1
            let file = try FileHandle(forUpdating: disk)
            try file.seek(toOffset: 2 * 512)
            let oldTable = try XCTUnwrap(file.read(upToCount: 128 * 128))
            let sentinel = Data((0..<512).map { UInt8(truncatingIfNeeded: $0 * 31) })
            try file.seek(toOffset: (oldLast - 33) * 512)
            try file.write(contentsOf: sentinel)
            try file.synchronize()
            try file.close()

            try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 100)

            var attributes = stat()
            XCTAssertEqual(stat(disk.path, &attributes), 0)
            let newSize = UInt64(100) * 1_073_741_824
            let newLast = newSize / 512 - 1
            XCTAssertEqual(attributes.st_size, off_t(newSize))
            XCTAssertLessThan(attributes.st_blocks * 512, 64 * 1024 * 1024)
            XCTAssertEqual(try SteamOSDisk.systemCapacity(at: disk), newSize)
            XCTAssertNoThrow(try SteamOSDisk.validateSystem(at: disk))

            let grown = try FileHandle(forReadingFrom: disk)
            defer { try? grown.close() }
            func sector(_ number: UInt64) throws -> Data {
                try grown.seek(toOffset: number * 512)
                return try XCTUnwrap(grown.read(upToCount: 512))
            }
            let mbr = try sector(0)
            XCTAssertEqual(mbr[510], 0x55)
            XCTAssertEqual(mbr[511], 0xaa)
            for offset in stride(from: 446, to: 510, by: 16) {
                if offset == 446 {
                    XCTAssertEqual(mbr[offset + 4], 0xee)
                    XCTAssertEqual(integer(mbr, at: offset + 8, as: UInt32.self), 1)
                    XCTAssertEqual(integer(mbr, at: offset + 12, as: UInt32.self),
                                   UInt32(min(newLast, UInt64(UInt32.max))))
                } else {
                    XCTAssertEqual(mbr[offset + 4], 0)
                }
            }
            let primary = try sector(1)
            XCTAssertEqual(integer(primary, at: 24, as: UInt64.self), 1)
            XCTAssertEqual(integer(primary, at: 32, as: UInt64.self), newLast)
            XCTAssertEqual(integer(primary, at: 48, as: UInt64.self), newLast - 33)
            var primaryCRC = Data(primary.prefix(92))
            put(UInt32(0), into: &primaryCRC, at: 16)
            XCTAssertEqual(SteamOSDisk.crc32(primaryCRC), integer(primary, at: 16, as: UInt32.self))
            let backup = try sector(newLast)
            XCTAssertEqual(backup.prefix(8), Data("EFI PART".utf8))
            XCTAssertEqual(integer(backup, at: 24, as: UInt64.self), newLast)
            XCTAssertEqual(integer(backup, at: 32, as: UInt64.self), 1)
            XCTAssertEqual(integer(backup, at: 72, as: UInt64.self), newLast - 32)
            var backupCRC = Data(backup.prefix(92))
            put(UInt32(0), into: &backupCRC, at: 16)
            XCTAssertEqual(SteamOSDisk.crc32(backupCRC), integer(backup, at: 16, as: UInt32.self))
            try grown.seek(toOffset: 2 * 512)
            let table = try XCTUnwrap(grown.read(upToCount: 128 * 128))
            XCTAssertEqual(table.prefix(7 * 128), oldTable.prefix(7 * 128))
            XCTAssertEqual(table.subdata(in: (7 * 128)..<(7 * 128 + 40)),
                           oldTable.subdata(in: (7 * 128)..<(7 * 128 + 40)))
            XCTAssertEqual(table.subdata(in: (7 * 128 + 48)..<(8 * 128)),
                           oldTable.subdata(in: (7 * 128 + 48)..<(8 * 128)))
            XCTAssertEqual(integer(table, at: 7 * 128 + 40, as: UInt64.self), newLast - 33)
            XCTAssertEqual(SteamOSDisk.crc32(table), integer(primary, at: 88, as: UInt32.self))
            try grown.seek(toOffset: (newLast - 32) * 512)
            XCTAssertEqual(try XCTUnwrap(grown.read(upToCount: 128 * 128)), table)
            XCTAssertEqual(try sector(oldLast - 32), Data(repeating: 0, count: 512))
            XCTAssertEqual(try sector(oldLast), Data(repeating: 0, count: 512))
            try grown.seek(toOffset: (oldLast - 33) * 512)
            XCTAssertEqual(try XCTUnwrap(grown.read(upToCount: 512)), sentinel)
            try assertNoStagingFiles(in: directory)
        }
    }

    func testGrowSystemRejectsShrinkEqualLockedAndLinked() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let before = try metadata(disk)
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 64))
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 32))
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 1_000_000))
            let alias = directory.appendingPathComponent("alias.img")
            try FileManager.default.linkItem(at: disk, to: alias)
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 128))
            try FileManager.default.removeItem(at: alias)
            let fd = open(disk.path, O_RDWR | O_CLOEXEC)
            XCTAssertGreaterThanOrEqual(fd, 0)
            XCTAssertEqual(flock(fd, LOCK_EX | LOCK_NB), 0)
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 128))
            close(fd)
            var attributes = stat()
            XCTAssertEqual(stat(disk.path, &attributes), 0)
            XCTAssertEqual(attributes.st_size, 64 * 1_073_741_824)
            XCTAssertEqual(try metadata(disk), before)
            XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path), ["SteamOS.img"])
            XCTAssertNoThrow(try SteamOSDisk.validateSystem(at: disk))
        }
    }

    func testGrowSystemRejectsNonStandardGPTGeometry() throws {
        try withDirectory { directory in
            // A partition table holding fewer than 128 entries.
            let shortTable = directory.appendingPathComponent("short.img")
            try SteamOSDisk.createBlank(at: shortTable, capacityGiB: 64)
            try writeSystemSignatures(shortTable)
            var attributes = stat()
            XCTAssertEqual(stat(shortTable.path, &attributes), 0)
            let last = UInt64(attributes.st_size) / 512 - 1
            let reader = try FileHandle(forReadingFrom: shortTable)
            try reader.seek(toOffset: 2 * 512)
            let shortEntries = Data(try XCTUnwrap(reader.read(upToCount: 4 * 128)))
            try reader.close()
            for sector in [UInt64(1), last] {
                try patchHeader(shortTable, sector: sector) { header in
                    put(UInt32(4), into: &header, at: 80)
                    put(SteamOSDisk.crc32(shortEntries), into: &header, at: 88)
                }
            }
            var before = try metadata(shortTable)
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: shortTable, toCapacityGiB: 128))
            XCTAssertEqual(try metadata(shortTable), before)
            try assertNoStagingFiles(in: directory, ignoring: ["short.img"])

            // A backup header whose partition table points at the primary table.
            let misdirected = directory.appendingPathComponent("misdirected.img")
            try SteamOSDisk.createBlank(at: misdirected, capacityGiB: 64)
            try writeSystemSignatures(misdirected)
            try patchHeader(misdirected, sector: last) { header in
                put(UInt64(2), into: &header, at: 72)
            }
            before = try metadata(misdirected)
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: misdirected, toCapacityGiB: 128))
            XCTAssertEqual(try metadata(misdirected), before)
            try assertNoStagingFiles(in: directory, ignoring: ["short.img", "misdirected.img"])
        }
    }

    func testGrowSystemPreservesFileACLs() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let chmod = Process()
            chmod.executableURL = URL(fileURLWithPath: "/bin/chmod")
            chmod.arguments = ["+a", "nobody allow read", disk.path]
            try chmod.run()
            chmod.waitUntilExit()
            guard chmod.terminationStatus == 0, aclEntries(at: disk).contains("nobody allow read") else {
                throw XCTSkip("ACLs are not supported here")
            }
            try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 128)
            XCTAssertTrue(aclEntries(at: disk).contains("nobody allow read"))
        }
    }

    private func aclEntries(at url: URL) -> String {
        let list = Process()
        let pipe = Pipe()
        list.executableURL = URL(fileURLWithPath: "/bin/ls")
        list.arguments = ["-le", url.path]
        list.standardOutput = pipe
        guard let _ = try? list.run() else { return "" }
        list.waitUntilExit()
        return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    }

    // Head (0..<34) and tail sectors: enough to cover both GPTs and the MBR.
    private func metadata(_ url: URL) throws -> Data {
        var attributes = stat()
        XCTAssertEqual(stat(url.path, &attributes), 0)
        let last = UInt64(attributes.st_size) / 512
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        let head = try XCTUnwrap(file.read(upToCount: 34 * 512))
        try file.seek(toOffset: (last - 34) * 512)
        let tail = try XCTUnwrap(file.read(upToCount: 34 * 512))
        return head + tail
    }

    private func patchHeader(_ disk: URL, sector: UInt64, _ patch: (inout Data) -> Void) throws {
        let file = try FileHandle(forUpdating: disk)
        defer { try? file.close() }
        try file.seek(toOffset: sector * 512)
        var header = try XCTUnwrap(file.read(upToCount: 512))
        patch(&header)
        put(UInt32(0), into: &header, at: 16)
        put(SteamOSDisk.crc32(header.prefix(92)), into: &header, at: 16)
        try file.seek(toOffset: sector * 512)
        try file.write(contentsOf: header)
    }

    private func assertNoStagingFiles(in directory: URL, ignoring kept: [String] = ["SteamOS.img"]) throws {
        let contents = try FileManager.default.contentsOfDirectory(atPath: directory.path)
        XCTAssertEqual(contents.filter { !kept.contains($0) }, [])
        XCTAssertFalse(contents.contains { $0.contains(".resize-") })
    }

    func testGrowSystemPreservesDedicatedHomeTypeGUID() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let homeType = gptGUIDBytes("933AC7E1-2EB4-4F13-B844-0E14E2AEF915")
            try rewriteEntry8TypeGUID(disk, type: homeType)
            try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 128)
            let file = try FileHandle(forReadingFrom: disk)
            defer { try? file.close() }
            try file.seek(toOffset: 2 * 512 + 7 * 128)
            let entry = try XCTUnwrap(file.read(upToCount: 128))
            XCTAssertEqual(entry.prefix(16), homeType)
        }
    }

    func testSystemDiskUsageReportsCapacityAllocationAndBounds() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let gib = UInt64(1) << 30
            var usage = try SteamOSDisk.systemDiskUsage(at: disk)
            XCTAssertEqual(usage.capacityBytes, 64 * gib)
            XCTAssertLessThan(usage.allocatedBytes, 16 * 1024 * 1024)
            // Layout floor: partitions before home (~22.4 GiB) + 1 GiB home + GPT tail.
            XCTAssertEqual(usage.minimumCapacityGiB, 24)
            var volume = statfs()
            XCTAssertEqual(statfs(disk.path, &volume), 0)
            XCTAssertEqual(usage.maximumCapacityGiB,
                           Int(UInt64(volume.f_blocks) * UInt64(volume.f_bsize) / gib))
            // A few MiB of real data raises allocatedBytes but stays under the
            // layout floor, so the minimum is unchanged.
            let file = try FileHandle(forUpdating: disk)
            try file.seek(toOffset: 46_925_824 * 512)
            try file.write(contentsOf: Data(repeating: 0x5a, count: 3 * 1024 * 1024))
            try file.synchronize()
            try file.close()
            usage = try SteamOSDisk.systemDiskUsage(at: disk)
            XCTAssertGreaterThanOrEqual(usage.allocatedBytes, 3 * 1024 * 1024)
            XCTAssertEqual(usage.minimumCapacityGiB, 24)
        }
    }

    func testHomeShrinkTargetKiBMathAndBounds() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let identity = try SteamOSDisk.systemDiskUsage(at: disk).identity
            // lastUsable = 40 * 2^21 - 34 = 83886046; home start = 46925824.
            // (83886046 - 46925824 + 1) * 512 / 1024 rounded down to a multiple of 4.
            XCTAssertEqual(try SteamOSDisk.homeShrinkTargetKiB(at: disk, toCapacityGiB: 40, expected: identity), 18_480_108)
            XCTAssertThrowsError(try SteamOSDisk.homeShrinkTargetKiB(at: disk, toCapacityGiB: 20, expected: identity))
            XCTAssertThrowsError(try SteamOSDisk.homeShrinkTargetKiB(at: disk, toCapacityGiB: 64, expected: identity))
            XCTAssertThrowsError(try SteamOSDisk.homeShrinkTargetKiB(at: disk, toCapacityGiB: 128, expected: identity))
            let stale = SystemDiskIdentity(diskGUID: UUID(), homePartitionGUID: identity.homePartitionGUID,
                                           capacityBytes: identity.capacityBytes)
            XCTAssertThrowsError(try SteamOSDisk.homeShrinkTargetKiB(at: disk, toCapacityGiB: 40, expected: stale)) { error in
                XCTAssertTrue(error.localizedDescription.contains("changed"))
            }
        }
    }

    func testHomeFilesystemBytesReadsExt4Superblock() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            try writeFakeHomeSuperblock(disk, blocksLo: 1000)
            XCTAssertEqual(try SteamOSDisk.homeFilesystemBytes(at: disk), 1000 * 1024)
            // 64-bit feature: high dword counts; 4 KiB blocks (log_block_size = 2).
            try writeFakeHomeSuperblock(disk, blocksLo: 1, blocksHi: 2, freeHi: 2, logBlockSize: 2, incompat: 0x80)
            XCTAssertEqual(try SteamOSDisk.homeFilesystemBytes(at: disk), UInt64(8_589_934_593) * 4096)
            // Without the 64-bit feature the high dword is ignored.
            try writeFakeHomeSuperblock(disk, blocksLo: 1, blocksHi: 2, incompat: 0)
            XCTAssertEqual(try SteamOSDisk.homeFilesystemBytes(at: disk), 1024)
            // Without metadata_csum no superblock checksum is required.
            try writeFakeHomeSuperblock(disk, blocksLo: 1000, roCompat: 0)
            XCTAssertEqual(try SteamOSDisk.homeFilesystemBytes(at: disk), 1000 * 1024)
        }
    }

    func testHomeFilesystemBytesRejectsUntrustworthySuperblocks() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let probe = { try SteamOSDisk.homeFilesystemBytes(at: disk) }
            try writeFakeHomeSuperblock(disk, blocksLo: 0, freeLo: 0)
            XCTAssertThrowsError(try probe())
            try writeFakeHomeSuperblock(disk, blocksLo: 10, freeLo: 11)
            XCTAssertThrowsError(try probe())
            try writeFakeHomeSuperblock(disk, blocksLo: 1000, firstData: 0)
            XCTAssertThrowsError(try probe())
            try writeFakeHomeSuperblock(disk, blocksLo: 1000, state: 0)
            XCTAssertThrowsError(try probe())
            try writeFakeHomeSuperblock(disk, blocksLo: 1000, state: 3)
            XCTAssertThrowsError(try probe())
            try writeFakeHomeSuperblock(disk, blocksLo: 1000, incompat: 0x4)
            XCTAssertThrowsError(try probe())
            try writeFakeHomeSuperblock(disk, blocksLo: 1000, checksumType: 0)
            XCTAssertThrowsError(try probe())
            // A valid superblock whose contents no longer match its checksum.
            try writeFakeHomeSuperblock(disk, blocksLo: 1000)
            let file = try FileHandle(forUpdating: disk)
            try file.seek(toOffset: 46_925_824 * 512 + 1024 + 0x0c)
            try file.write(contentsOf: Data([7, 0, 0, 0])) // s_free_blocks_count, stale CRC
            try file.close()
            XCTAssertThrowsError(try probe())
        }
    }

    func testHomeFilesystemBytesMatchesRealExt4Image() throws {
        guard FileManager.default.isExecutableFile(atPath: "/opt/homebrew/bin/mke2fs") else {
            throw XCTSkip("mke2fs is not installed")
        }
        try withDirectory { directory in
            let image = directory.appendingPathComponent("home.ext4")
            let make = Process()
            make.executableURL = URL(fileURLWithPath: "/opt/homebrew/bin/mke2fs")
            make.arguments = ["-t", "ext4", "-b", "1024", image.path, "8M"]
            make.standardOutput = FileHandle.nullDevice
            make.standardError = FileHandle.nullDevice
            try make.run()
            make.waitUntilExit()
            guard make.terminationStatus == 0 else { throw XCTSkip("mke2fs failed") }
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            let filesystem = try FileHandle(forReadingFrom: image)
            let contents = filesystem.readDataToEndOfFile()
            try filesystem.close()
            let home = try FileHandle(forUpdating: disk)
            try home.seek(toOffset: 46_925_824 * 512)
            try home.write(contentsOf: contents)
            try home.close()
            XCTAssertEqual(try SteamOSDisk.homeFilesystemBytes(at: disk), 8 * 1024 * 1024)
        }
    }

    func testShrinkSystemTrimsImageWhenFilesystemFits() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 128)
            try writeSystemSignatures(disk)
            try writeFakeHomeSuperblock(disk, blocksLo: 1_000_000) // ~1 GiB ext4
            let identity = try SteamOSDisk.systemDiskUsage(at: disk).identity
            // Re-running a committed shrink is a no-op (same size, same GUIDs).
            XCTAssertNoThrow(try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 128, expected: identity))
            let file = try FileHandle(forUpdating: disk)
            try file.seek(toOffset: 2 * 512)
            let oldTable = try XCTUnwrap(file.read(upToCount: 128 * 128))
            let sentinel = Data((0..<512).map { UInt8(truncatingIfNeeded: $0 * 17) })
            let newSize = UInt64(100) * 1_073_741_824
            let newLast = newSize / 512 - 1
            try file.seek(toOffset: (newLast - 33) * 512) // last sector of the new home
            try file.write(contentsOf: sentinel)
            try file.synchronize()
            try file.close()

            try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 100, expected: identity)

            var attributes = stat()
            XCTAssertEqual(stat(disk.path, &attributes), 0)
            XCTAssertEqual(attributes.st_size, off_t(newSize))
            XCTAssertNoThrow(try SteamOSDisk.validateSystem(at: disk))
            let shrunk = try FileHandle(forReadingFrom: disk)
            defer { try? shrunk.close() }
            func sector(_ number: UInt64) throws -> Data {
                try shrunk.seek(toOffset: number * 512)
                return try XCTUnwrap(shrunk.read(upToCount: 512))
            }
            let primary = try sector(1)
            XCTAssertEqual(integer(primary, at: 24, as: UInt64.self), 1)
            XCTAssertEqual(integer(primary, at: 32, as: UInt64.self), newLast)
            XCTAssertEqual(integer(primary, at: 48, as: UInt64.self), newLast - 33)
            var primaryCRC = Data(primary.prefix(92))
            put(UInt32(0), into: &primaryCRC, at: 16)
            XCTAssertEqual(SteamOSDisk.crc32(primaryCRC), integer(primary, at: 16, as: UInt32.self))
            let backup = try sector(newLast)
            XCTAssertEqual(backup.prefix(8), Data("EFI PART".utf8))
            XCTAssertEqual(integer(backup, at: 24, as: UInt64.self), newLast)
            XCTAssertEqual(integer(backup, at: 32, as: UInt64.self), 1)
            XCTAssertEqual(integer(backup, at: 72, as: UInt64.self), newLast - 32)
            var backupCRC = Data(backup.prefix(92))
            put(UInt32(0), into: &backupCRC, at: 16)
            XCTAssertEqual(SteamOSDisk.crc32(backupCRC), integer(backup, at: 16, as: UInt32.self))
            try shrunk.seek(toOffset: 2 * 512)
            let table = try XCTUnwrap(shrunk.read(upToCount: 128 * 128))
            XCTAssertEqual(table.prefix(7 * 128), oldTable.prefix(7 * 128))
            XCTAssertEqual(table.subdata(in: (7 * 128)..<(7 * 128 + 40)),
                           oldTable.subdata(in: (7 * 128)..<(7 * 128 + 40)))
            XCTAssertEqual(table.subdata(in: (7 * 128 + 48)..<(8 * 128)),
                           oldTable.subdata(in: (7 * 128 + 48)..<(8 * 128)))
            XCTAssertEqual(integer(table, at: 7 * 128 + 40, as: UInt64.self), newLast - 33)
            XCTAssertEqual(SteamOSDisk.crc32(table), integer(primary, at: 88, as: UInt32.self))
            try shrunk.seek(toOffset: (newLast - 32) * 512)
            XCTAssertEqual(try XCTUnwrap(shrunk.read(upToCount: 128 * 128)), table)
            XCTAssertEqual(try sector(newLast - 33), sentinel)
            try assertNoStagingFiles(in: directory)
        }
    }

    func testShrinkSystemRejectsWhenFilesystemDoesNotFit() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 128)
            try writeSystemSignatures(disk)
            // ~100 GiB filesystem cannot fit the ~77.6 GiB home of a 100 GiB disk.
            try writeFakeHomeSuperblock(disk, blocksLo: 100 * 1_048_576)
            let identity = try SteamOSDisk.systemDiskUsage(at: disk).identity
            let before = try metadata(disk)
            XCTAssertThrowsError(try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 100, expected: identity)) { error in
                XCTAssertTrue(error.localizedDescription.contains("has not been reduced"))
            }
            XCTAssertThrowsError(try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 256, expected: identity))
            // A disk whose GUIDs differ is rejected before any fit check.
            let stale = SystemDiskIdentity(diskGUID: identity.diskGUID, homePartitionGUID: UUID(),
                                           capacityBytes: identity.capacityBytes)
            XCTAssertThrowsError(try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 100, expected: stale)) { error in
                XCTAssertTrue(error.localizedDescription.contains("changed"))
            }
            XCTAssertEqual(try metadata(disk), before)
            try assertNoStagingFiles(in: directory)
        }
    }

    func testShrinkSystemFitBoundaryUsesFilesystemBlocks() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 128)
            try writeSystemSignatures(disk)
            let identity = try SteamOSDisk.systemDiskUsage(at: disk).identity
            // 100 GiB home span: (209715166 - 46925824 + 1) * 512 = 83348143616 bytes.
            // One block too many (1024 B) overflows it; exactly fitting passes.
            try writeFakeHomeSuperblock(disk, blocksLo: 81_394_672)
            let before = try metadata(disk)
            XCTAssertThrowsError(try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 100, expected: identity))
            XCTAssertEqual(try metadata(disk), before)
            try writeFakeHomeSuperblock(disk, blocksLo: 81_394_671)
            XCTAssertNoThrow(try SteamOSDisk.shrinkSystem(at: disk, toCapacityGiB: 100, expected: identity))
            var attributes = stat()
            XCTAssertEqual(stat(disk.path, &attributes), 0)
            XCTAssertEqual(attributes.st_size, 100 * 1_073_741_824)
            try assertNoStagingFiles(in: directory)
        }
    }

    func testResizeRejectsPartitionSlotGaps() throws {
        try withDirectory { directory in
            let disk = directory.appendingPathComponent("SteamOS.img")
            try SteamOSDisk.createBlank(at: disk, capacityGiB: 64)
            try writeSystemSignatures(disk)
            // Move the home entry from slot 8 to slot 9, leaving slot 8 empty.
            try patchTables(disk) { table in
                table.replaceSubrange((8 * 128)..<(9 * 128), with: table[(7 * 128)..<(8 * 128)])
                table.replaceSubrange((7 * 128)..<(8 * 128), with: Data(repeating: 0, count: 128))
            }
            let before = try metadata(disk)
            XCTAssertThrowsError(try SteamOSDisk.systemDiskUsage(at: disk))
            XCTAssertThrowsError(try SteamOSDisk.growSystem(at: disk, toCapacityGiB: 100))
            XCTAssertEqual(try metadata(disk), before)
            try assertNoStagingFiles(in: directory)
        }
    }

    // Minimal valid ext4 superblock inside the home partition (home starts at
    // sector 46925824). Defaults satisfy every check in homeFilesystemBytes,
    // including the metadata_csum crc32c when ro_compat enables it.
    private func writeFakeHomeSuperblock(_ disk: URL, blocksLo: UInt32, blocksHi: UInt32 = 0,
                                         freeLo: UInt32? = nil, freeHi: UInt32 = 0,
                                         logBlockSize: UInt32 = 0, firstData: UInt32? = nil,
                                         incompat: UInt32 = 0, roCompat: UInt32 = 0x400,
                                         state: UInt16 = 1, checksumType: UInt8 = 1) throws {
        var superblock = Data(repeating: 0, count: 1024)
        put(blocksLo, into: &superblock, at: 0x04)
        put(freeLo ?? blocksLo, into: &superblock, at: 0x0c)
        put(firstData ?? (logBlockSize == 0 ? 1 : 0), into: &superblock, at: 0x14)
        put(logBlockSize, into: &superblock, at: 0x18)
        put(state, into: &superblock, at: 0x3a)
        put(UInt16(0xef53), into: &superblock, at: 0x38)
        put(incompat, into: &superblock, at: 0x60)
        put(roCompat, into: &superblock, at: 0x64)
        put(blocksHi, into: &superblock, at: 0x150)
        put(freeHi, into: &superblock, at: 0x158)
        if roCompat & 0x400 != 0 {
            superblock[0x175] = checksumType
            if checksumType == 1 {
                put(SteamOSDisk.crc32c(0xffff_ffff, Data(superblock.prefix(0x3fc))),
                    into: &superblock, at: 0x3fc)
            }
        }
        let file = try FileHandle(forUpdating: disk)
        defer { try? file.close() }
        try file.seek(toOffset: 46_925_824 * 512 + 1024)
        try file.write(contentsOf: superblock)
    }

    // Repoints entry 8's type GUID in both GPTs and fixes every affected CRC.
    private func rewriteEntry8TypeGUID(_ disk: URL, type: Data) throws {
        try patchTables(disk) { table in
            table.replaceSubrange((7 * 128)..<(7 * 128 + 16), with: type)
        }
    }

    // Rewrites both copies of the partition table and fixes header CRCs.
    private func patchTables(_ disk: URL, _ mutate: (inout Data) -> Void) throws {
        var attributes = stat()
        XCTAssertEqual(stat(disk.path, &attributes), 0)
        let last = UInt64(attributes.st_size) / 512 - 1
        let file = try FileHandle(forUpdating: disk)
        defer { try? file.close() }
        try file.seek(toOffset: 2 * 512)
        var table = try XCTUnwrap(file.read(upToCount: 128 * 128))
        mutate(&table)
        for tableSector in [UInt64(2), last - 32] {
            try file.seek(toOffset: tableSector * 512)
            try file.write(contentsOf: table)
        }
        for headerSector in [UInt64(1), last] {
            try file.seek(toOffset: headerSector * 512)
            var header = try XCTUnwrap(file.read(upToCount: 512))
            put(SteamOSDisk.crc32(table), into: &header, at: 88)
            put(UInt32(0), into: &header, at: 16)
            put(SteamOSDisk.crc32(header.prefix(92)), into: &header, at: 16)
            try file.seek(toOffset: headerSector * 512)
            try file.write(contentsOf: header)
        }
    }

    private func gptGUIDBytes(_ string: String) -> Data {
        var value = UUID(uuidString: string)!.uuid
        var data = withUnsafeBytes(of: &value) { Data($0) }
        data.replaceSubrange(0..<4, with: data[0..<4].reversed())
        data.replaceSubrange(4..<6, with: data[4..<6].reversed())
        data.replaceSubrange(6..<8, with: data[6..<8].reversed())
        return data
    }

    private func integer<T: FixedWidthInteger>(_ data: Data, at offset: Int, as: T.Type = T.self) -> T {
        var value: T = 0
        for index in 0..<MemoryLayout<T>.size { value |= T(data[offset + index]) << (index * 8) }
        return value
    }

    private func writeSystemSignatures(_ url: URL) throws {
        let file = try FileHandle(forWritingTo: url)
        defer { try? file.close() }
        let starts: [UInt64] = [2048, 526336, 657408, 788480, 21760000, 42731520, 44828672, 46925824]
        for index in 0..<3 {
            var fat = Data(repeating: 0, count: 512)
            fat.replaceSubrange(82..<85, with: Data("FAT".utf8))
            fat[510] = 0x55
            fat[511] = 0xaa
            try file.seek(toOffset: starts[index] * 512)
            try file.write(contentsOf: fat)
        }
        for index in 3..<5 {
            try file.seek(toOffset: starts[index] * 512 + 0x10040)
            try file.write(contentsOf: Data("_BHRfS_M".utf8))
        }
        for index in 5..<8 {
            try file.seek(toOffset: starts[index] * 512 + 1080)
            try file.write(contentsOf: Data([0x53, 0xef]))
        }
    }

    // Independent small GPT fixture: mirrors the UEFI byte layout, not production disk creation.
    private func makeRecovery(_ url: URL) throws {
        var image = Data(repeating: 0, count: 16 * 1024 * 1024)
        var entries = Data(repeating: 0, count: 128 * 128)
        let names = ["esp", "efi-A", "rootfs-A", "var-A", "home"]
        let starts: [UInt64] = [2048, 4096, 8192, 16384, 20480]
        for index in names.indices {
            let offset = index * 128
            entries[offset] = 1 // A non-empty partition type.
            entries[offset + 16] = UInt8(index + 1)
            put(starts[index], into: &entries, at: offset + 32)
            put(index == 4 ? UInt64(28671) : starts[index + 1] - 1, into: &entries, at: offset + 40)
            let name = names[index].data(using: .utf16LittleEndian)!
            entries.replaceSubrange((offset + 56)..<(offset + 56 + name.count), with: name)
        }
        func header(current: UInt64, other: UInt64, table: UInt64) -> Data {
            var result = Data(repeating: 0, count: 512)
            result.replaceSubrange(0..<8, with: Data("EFI PART".utf8))
            put(UInt32(0x10000), into: &result, at: 8)
            put(UInt32(92), into: &result, at: 12)
            put(current, into: &result, at: 24)
            put(other, into: &result, at: 32)
            put(UInt64(34), into: &result, at: 40)
            put(UInt64(32734), into: &result, at: 48)
            put(table, into: &result, at: 72)
            put(UInt32(128), into: &result, at: 80)
            put(UInt32(128), into: &result, at: 84)
            put(SteamOSDisk.crc32(entries), into: &result, at: 88)
            put(SteamOSDisk.crc32(result.prefix(92)), into: &result, at: 16)
            return result
        }
        image.replaceSubrange(512..<1024, with: header(current: 1, other: 32767, table: 2))
        image.replaceSubrange(1024..<(1024 + entries.count), with: entries)
        image.replaceSubrange((32735 * 512)..<(32767 * 512), with: entries)
        image.replaceSubrange((32767 * 512)..<(32768 * 512), with: header(current: 32767, other: 1, table: 32735))
        image[2048 * 512 + 510] = 0x55
        image[2048 * 512 + 511] = 0xaa
        image.replaceSubrange((2048 * 512 + 82)..<(2048 * 512 + 85), with: Data("FAT".utf8))
        image.replaceSubrange((8192 * 512 + 0x10040)..<(8192 * 512 + 0x10048), with: Data("_BHRfS_M".utf8))
        try image.write(to: url)
    }

    private func put<T: FixedWidthInteger>(_ value: T, into data: inout Data, at offset: Int) {
        for index in 0..<MemoryLayout<T>.size { data[offset + index] = UInt8(truncatingIfNeeded: value >> (index * 8)) }
    }
}
