import Darwin
import Foundation

public struct SteamOSDeploymentError: LocalizedError, Sendable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

/// Stable identity of a system disk and its home partition, from the GPT GUIDs.
public struct SystemDiskIdentity: Codable, Equatable, Sendable {
    public let diskGUID: UUID
    public let homePartitionGUID: UUID
    public let capacityBytes: UInt64
    public init(diskGUID: UUID, homePartitionGUID: UUID, capacityBytes: UInt64) {
        self.diskGUID = diskGUID
        self.homePartitionGUID = homePartitionGUID
        self.capacityBytes = capacityBytes
    }
}

/// Usage and resize bounds of a system disk, in bytes and GiB (2^30).
public struct SystemDiskUsage: Equatable, Sendable {
    public let capacityBytes: UInt64
    public let allocatedBytes: UInt64
    public let minimumCapacityGiB: Int
    public let maximumCapacityGiB: Int
    public let identity: SystemDiskIdentity
    public init(capacityBytes: UInt64, allocatedBytes: UInt64, minimumCapacityGiB: Int,
                maximumCapacityGiB: Int, identity: SystemDiskIdentity) {
        self.capacityBytes = capacityBytes
        self.allocatedBytes = allocatedBytes
        self.minimumCapacityGiB = minimumCapacityGiB
        self.maximumCapacityGiB = maximumCapacityGiB
        self.identity = identity
    }
}

/// The raw GPT layout consumed by Brinewell's SteamOS initramfs.
public enum SteamOSDisk {
    public static let capacitiesGiB = [64, 128, 256, 512, 1024]
    private static let systemNames = ["esp", "efi-A", "efi-B", "rootfs-A", "rootfs-B", "var-A", "var-B", "home"]
    private static let recoveryNames = ["esp", "efi-A", "rootfs-A", "var-A", "home"]
    private static let starts: [UInt64] = [2048, 526336, 657408, 788480, 21760000, 42731520, 44828672, 46925824]

    public static func validateSystem(at url: URL) throws {
        let disk = try Reader(url)
        let partitions = try disk.partitions()
        guard partitions.map(\.name) == systemNames else {
            if partitions.map(\.name) == recoveryNames {
                throw SteamOSDeploymentError(String(localized: "This is a SteamOS installation image. Choose Fresh Install."))
            }
            throw SteamOSDeploymentError(String(localized: "This file is not a usable SteamOS system disk. Choose a different disk or use Fresh Install."))
        }
        for partition in partitions {
            if partition.name == "esp" || partition.name.hasPrefix("efi-") {
                try disk.validateFAT(partition)
            } else if partition.name.hasPrefix("rootfs-") {
                try disk.validateBtrfs(partition)
            } else {
                let magic = try disk.read(at: partition.start * 512 + 1080, count: 2)
                guard magic == Data([0x53, 0xef]) else {
                    throw SteamOSDeploymentError(String(localized: "The system disk was not fully formatted (\(partition.name)). Reinstall SteamOS."))
                }
            }
        }
    }

    public static func validateInstaller(at url: URL) throws {
        let disk = try Reader(url)
        let partitions = try disk.partitions()
        if partitions.map(\.name) == systemNames {
            throw SteamOSDeploymentError(String(localized: "This disk already contains SteamOS. Use Choose Existing System Disk."))
        }
        guard partitions.map(\.name) == recoveryNames else {
            throw SteamOSDeploymentError(String(localized: "Choose a SteamOS ARM64 recovery image in .img, .raw, or .img.bz2 format."))
        }
        try disk.validateFAT(partitions[0])
        try disk.validateBtrfs(partitions[2])
    }

    /// Creates only a new sparse file. Existing files and symlinks are never opened for writing.
    @discardableResult
    public static func createBlank(at url: URL, capacityGiB: Int) throws -> UUID {
        guard capacitiesGiB.contains(capacityGiB) else {
            throw SteamOSDeploymentError(String(localized: "Choose a disk capacity of 64, 128, 256, 512, or 1024 GB."))
        }
        let parent = url.deletingLastPathComponent()
        guard try parent.resourceValues(forKeys: [.volumeSupportsSparseFilesKey]).volumeSupportsSparseFiles == true else {
            throw SteamOSDeploymentError(String(localized: "This location does not support sparse disk images. Choose a folder on an APFS volume."))
        }
        let descriptor = open(url.path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard descriptor >= 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not create the system disk. Choose a writable folder and make sure the destination file does not already exist."))
        }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        var success = false
        defer {
            try? handle.close()
            if !success { try? FileManager.default.removeItem(at: url) }
        }
        let size = UInt64(capacityGiB) * 1_073_741_824
        guard ftruncate(descriptor, off_t(size)) == 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not allocate the system disk. Check the destination and available space."))
        }
        let lastSector = size / 512 - 1
        let diskGUID = UUID()
        var entries = Data(repeating: 0, count: 128 * 128)
        for index in systemNames.indices {
            let offset = index * 128
            let type = index < 3 ? "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" : "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
            entries.replaceSubrange(offset..<(offset + 16), with: guidBytes(UUID(uuidString: type)!))
            entries.replaceSubrange((offset + 16)..<(offset + 32), with: guidBytes(UUID()))
            entries.put(starts[index], at: offset + 32)
            entries.put(index + 1 < starts.count ? starts[index + 1] - 1 : lastSector - 33, at: offset + 40)
            let name = systemNames[index].data(using: .utf16LittleEndian)!
            entries.replaceSubrange((offset + 56)..<(offset + 56 + name.count), with: name)
        }
        var mbr = Data(repeating: 0, count: 512)
        mbr[450] = 0xee
        mbr.put(UInt32(1), at: 454)
        mbr.put(UInt32(min(lastSector, UInt64(UInt32.max))), at: 458)
        mbr[510] = 0x55
        mbr[511] = 0xaa
        func write(_ data: Data, sector: UInt64) throws {
            try handle.seek(toOffset: sector * 512)
            try handle.write(contentsOf: data)
        }
        try write(mbr, sector: 0)
        try write(header(current: 1, backup: lastSector, entries: 2, lastUsable: lastSector - 33, guid: diskGUID, table: entries), sector: 1)
        try write(entries, sector: 2)
        try write(entries, sector: lastSector - 32)
        try write(header(current: lastSector, backup: 1, entries: lastSector - 32, lastUsable: lastSector - 33, guid: diskGUID, table: entries), sector: lastSector)
        try handle.synchronize()
        success = true
        return diskGUID
    }

    /// Logical byte size of a GPT-valid system disk; never reads the sparse data extents.
    public static func systemCapacity(at url: URL) throws -> UInt64 {
        let disk = try Reader(url)
        let partitions = try disk.partitions()
        guard partitions.map(\.name) == systemNames else {
            throw SteamOSDeploymentError(String(localized: "This file is not a usable SteamOS system disk. Choose a different disk or use Fresh Install."))
        }
        return disk.size
    }

    /// Usage and resize bounds for a GPT-valid system disk.
    public static func systemDiskUsage(at url: URL) throws -> SystemDiskUsage {
        let disk = try Reader(url)
        let partitions = try systemPartitions(disk: disk)
        return try usage(disk: disk, partitions: partitions, layout: disk.gpt(), at: url)
    }

    /// Partitions of a disk that matches the exact SteamOS system layout:
    /// raw GPT slots 1...8 hold systemNames in order and no other slot is used.
    private static func systemPartitions(disk: Reader) throws -> [Partition] {
        let partitions = try disk.partitions()
        guard partitions.map(\.name) == systemNames else {
            throw SteamOSDeploymentError(String(localized: "This file is not a usable SteamOS system disk. Choose a different disk or use Fresh Install."))
        }
        guard partitions.count == systemNames.count,
              partitions.enumerated().allSatisfy({ $0.element.index == $0.offset }) else {
            throw SteamOSDeploymentError(String(localized: "The system disk's partition layout is not supported. The home partition must be the last partition."))
        }
        return partitions
    }

    private static func layoutMinimumGiB(homeStart: UInt64) -> Int {
        // Room for every partition before home plus a 1 GiB home and the GPT tail.
        Int((homeStart * 512 + 34 * 512 + 2 * (1 << 30) - 1) / (1 << 30))
    }

    private static func identity(disk: Reader, layout: Reader.GPTLayout, homeIndex: Int) -> SystemDiskIdentity {
        let offset = homeIndex * 128
        return SystemDiskIdentity(
            diskGUID: uuid(fromGPT: Data(layout.header[56..<72])),
            homePartitionGUID: uuid(fromGPT: Data(layout.table[(offset + 16)..<(offset + 32)])),
            capacityBytes: disk.size)
    }

    private static func usage(disk: Reader, partitions: [Partition],
                              layout: Reader.GPTLayout, at url: URL) throws -> SystemDiskUsage {
        var info = stat()
        var volume = statfs()
        guard stat(url.path, &info) == 0, statfs(url.path, &volume) == 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not read the selected image. Check that it exists and that you have permission to read it."))
        }
        let gib = UInt64(1) << 30
        let allocated = UInt64(max(0, info.st_blocks)) * 512
        let (totalBytes, overflow) = UInt64(volume.f_blocks).multipliedReportingOverflow(by: UInt64(volume.f_bsize))
        let maximum = Int(overflow ? UInt64.max / gib : totalBytes / gib)
        let allocatedMinimum = (allocated + 10 * gib + gib - 1) / gib
        return SystemDiskUsage(capacityBytes: disk.size, allocatedBytes: allocated,
                               minimumCapacityGiB: Int(max(allocatedMinimum, UInt64(layoutMinimumGiB(homeStart: partitions[7].start)))),
                               maximumCapacityGiB: maximum,
                               identity: identity(disk: disk, layout: layout, homeIndex: partitions[7].index))
    }

    private static func capacityBoundsError(minimum: Int, maximum: Int) -> SteamOSDeploymentError {
        SteamOSDeploymentError(String(localized: "Choose a capacity between \(minimum) and \(maximum) GB."))
    }

    /// Grows the image and its GPT so the home partition reaches the new end.
    /// The ext4 filesystem itself is grown by the guest's x-systemd.growfs on
    /// the next boot.
    public static func growSystem(at url: URL, toCapacityGiB: Int) throws {
        try resizeSystem(at: url, toCapacityGiB: toCapacityGiB, growing: true)
    }

    /// Shrinks the image and its GPT so the home partition ends at the new end.
    /// Only valid once the guest has already shrunk the ext4 filesystem with
    /// the brinewell.shrink_home target from homeShrinkTargetKiB. `expected`
    /// must be the identity captured when the shrink was scheduled.
    public static func shrinkSystem(at url: URL, toCapacityGiB: Int, expected: SystemDiskIdentity) throws {
        try resizeSystem(at: url, toCapacityGiB: toCapacityGiB, growing: false, expected: expected)
    }

    /// KiB (multiple of 4) the guest must pass to resize2fs before a shrink.
    public static func homeShrinkTargetKiB(at url: URL, toCapacityGiB: Int, expected: SystemDiskIdentity) throws -> UInt64 {
        let changed = SteamOSDeploymentError(String(localized: "The system disk changed after the shrink was scheduled. The shrink was cancelled."))
        let disk = try Reader(url)
        let partitions = try systemPartitions(disk: disk)
        let layout = try disk.gpt()
        guard identity(disk: disk, layout: layout, homeIndex: partitions[7].index) == expected else {
            throw changed
        }
        let usage = try usage(disk: disk, partitions: partitions, layout: layout, at: url)
        guard toCapacityGiB >= usage.minimumCapacityGiB else {
            throw capacityBoundsError(minimum: usage.minimumCapacityGiB, maximum: Int(disk.size / (1 << 30)) - 1)
        }
        let (newSize, overflow) = UInt64(max(0, toCapacityGiB)).multipliedReportingOverflow(by: 1_073_741_824)
        guard toCapacityGiB >= 1, !overflow, newSize < disk.size else {
            throw SteamOSDeploymentError(String(localized: "The new capacity must be smaller than the current disk size."))
        }
        let lastUsable = newSize / 512 - 34
        let homeStart = partitions[7].start
        let homeSectors = lastUsable > homeStart ? lastUsable - homeStart + 1 : 0
        return (homeSectors * 512 / 1024) & ~UInt64(3)
    }

    /// Byte size of the ext4 filesystem in the home partition, from its superblock.
    public static func homeFilesystemBytes(at url: URL) throws -> UInt64 {
        let disk = try Reader(url)
        let partitions = try systemPartitions(disk: disk)
        return try homeFilesystemBytes(disk: disk, homeStart: partitions[7].start)
    }

    /// ext4 superblock sanity gate: only a clean, consistent, self-checksummed
    /// filesystem counts as trustworthy evidence before truncation.
    private static func homeFilesystemBytes(disk: Reader, homeStart: UInt64) throws -> UInt64 {
        let readError = SteamOSDeploymentError(String(localized: "Could not read the size of the home file system."))
        let superblock = try disk.read(at: homeStart * 512 + 1024, count: 1024)
        guard superblock.integer(at: 0x38, as: UInt16.self) == 0xef53 else { throw readError }
        let logBlockSize: UInt32 = superblock.integer(at: 0x18)
        guard logBlockSize <= 6 else { throw readError }
        let blockSize = UInt64(1024) << logBlockSize
        let incompat: UInt32 = superblock.integer(at: 0x60)
        var blocks = UInt64(superblock.integer(at: 0x04, as: UInt32.self))
        var freeBlocks = UInt64(superblock.integer(at: 0x0c, as: UInt32.self))
        if incompat & 0x80 != 0 {
            blocks |= UInt64(superblock.integer(at: 0x150, as: UInt32.self)) << 32
            freeBlocks |= UInt64(superblock.integer(at: 0x158, as: UInt32.self)) << 32
        }
        let firstData: UInt32 = superblock.integer(at: 0x14)
        let state: UInt16 = superblock.integer(at: 0x3a)
        guard blocks > 0, freeBlocks <= blocks,
              firstData == (blockSize == 1024 ? 1 : 0),
              state & 0x1 != 0, state & 0x2 == 0,
              incompat & 0x4 == 0 else { throw readError }
        if superblock.integer(at: 0x64, as: UInt32.self) & 0x400 != 0 {
            guard superblock[0x175] == 1,
                  crc32c(0xffff_ffff, Data(superblock.prefix(0x3fc))) == superblock.integer(at: 0x3fc, as: UInt32.self) else {
                throw readError
            }
        }
        let (bytes, overflow) = blocks.multipliedReportingOverflow(by: blockSize)
        guard !overflow else { throw readError }
        return bytes
    }

    /// Shared resize transaction: lock the live image, stage a same-directory
    /// APFS clone, rewrite the GPT so entry 8 (home) ends at the new last
    /// usable sector, then swap it in atomically.
    private static func resizeSystem(at url: URL, toCapacityGiB: Int, growing: Bool, expected: SystemDiskIdentity? = nil) throws {
        let fd = open(url.path, O_RDWR | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not open the system disk for resizing."))
        }
        defer { close(fd) }
        var info = stat()
        guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_size >= 34 * 512 else {
            throw SteamOSDeploymentError(String(localized: "Choose a complete raw disk image."))
        }
        guard info.st_nlink == 1 else {
            throw SteamOSDeploymentError(String(localized: "The system disk has more than one file name (hard link). Remove the extra links before resizing."))
        }
        guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
            let error = errno
            if error == EWOULDBLOCK || error == EAGAIN {
                throw SteamOSDeploymentError(String(localized: "The SteamOS disk is in use. Shut down the virtual machine first."))
            }
            throw SteamOSDeploymentError(String(localized: "Could not lock the system disk for resizing."))
        }
        let replaced = SteamOSDeploymentError(String(localized: "The system disk was replaced while it was being resized. Try again."))
        func pathMatchesLockedFD() -> Bool {
            var current = stat()
            return stat(url.path, &current) == 0 && current.st_dev == info.st_dev
                && current.st_ino == info.st_ino && current.st_nlink == 1
        }
        guard pathMatchesLockedFD() else { throw replaced }
        let oldSize = UInt64(info.st_size)
        let (newSize, sizeOverflow) = UInt64(max(0, toCapacityGiB)).multipliedReportingOverflow(by: 1_073_741_824)
        let unsupported = SteamOSDeploymentError(String(localized: "The system disk's partition layout is not supported. The home partition must be the last partition."))
        guard oldSize % 512 == 0 else { throw unsupported }
        let oldLastSector = oldSize / 512 - 1
        let disk = try Reader(descriptor: fd)
        let partitions = try systemPartitions(disk: disk)
        let layout = try disk.gpt()
        let primary = layout.header
        let backup = layout.backup
        guard primary.integer(at: 8, as: UInt32.self) == 0x0001_0000,
              primary.integer(at: 12, as: UInt32.self) == 92,
              primary.integer(at: 24, as: UInt64.self) == 1,
              primary.integer(at: 32, as: UInt64.self) == oldLastSector,
              layout.backupSector == oldLastSector,
              primary.integer(at: 40, as: UInt64.self) >= 34,
              primary.integer(at: 48, as: UInt64.self) == oldLastSector - 33,
              primary.integer(at: 72, as: UInt64.self) == 2,
              primary.integer(at: 80, as: UInt32.self) == 128,
              primary.integer(at: 84, as: UInt32.self) == 128,
              backup.integer(at: 24, as: UInt64.self) == oldLastSector,
              backup.integer(at: 32, as: UInt64.self) == 1,
              backup.integer(at: 72, as: UInt64.self) == oldLastSector - 32 else {
            throw unsupported
        }
        let lastUsable: UInt64 = primary.integer(at: 48)
        guard let home = partitions.first(where: { $0.name == "home" }),
              home.index == 7, home.end == lastUsable,
              partitions.allSatisfy({ $0.end <= home.end }) else { throw unsupported }
        let homeOffset = home.index * 128
        let homeStart = home.start
        if let expected {
            let actual = identity(disk: disk, layout: layout, homeIndex: home.index)
            guard expected.diskGUID == actual.diskGUID,
                  expected.homePartitionGUID == actual.homePartitionGUID else {
                throw SteamOSDeploymentError(String(localized: "The system disk changed after the shrink was scheduled. The shrink was cancelled."))
            }
        }
        // A previously committed shrink is a no-op: the GUIDs still match and
        // the GPT already ends home at the last usable sector.
        if !growing, !sizeOverflow, newSize == oldSize { return }
        let mbr = try disk.read(at: 0, count: 512)
        let hybrid = SteamOSDeploymentError(String(localized: "The system disk uses a hybrid MBR and cannot be grown."))
        guard mbr[510] == 0x55, mbr[511] == 0xaa else { throw unsupported }
        var protective: Int?
        for offset in stride(from: 446, to: 510, by: 16) {
            let type = mbr[offset + 4]
            if type == 0xee {
                guard protective == nil, mbr.integer(at: offset + 8, as: UInt32.self) == 1 else { throw hybrid }
                protective = offset
            } else if type != 0 { throw hybrid }
        }
        guard let protective else { throw unsupported }
        var volume = statfs()
        if growing {
            guard !sizeOverflow, newSize > oldSize else {
                throw SteamOSDeploymentError(String(localized: "The new capacity must be larger than the current disk size."))
            }
            guard statfs(url.path, &volume) == 0 else {
                throw SteamOSDeploymentError(String(localized: "Not enough free space on this volume to grow the disk safely."))
            }
            let (totalBytes, overflow) = UInt64(volume.f_blocks).multipliedReportingOverflow(by: UInt64(volume.f_bsize))
            let maximumGiB = Int(overflow ? UInt64.max / (1 << 30) : totalBytes / (1 << 30))
            guard toCapacityGiB <= maximumGiB else {
                throw capacityBoundsError(minimum: Int(oldSize / (1 << 30)) + 1, maximum: maximumGiB)
            }
            guard UInt64(volume.f_bavail) * UInt64(volume.f_bsize) >= 64 * 1_048_576 else {
                throw SteamOSDeploymentError(String(localized: "Not enough free space on this volume to grow the disk safely."))
            }
        } else {
            guard toCapacityGiB >= 1, !sizeOverflow, newSize < oldSize else {
                throw SteamOSDeploymentError(String(localized: "The new capacity must be smaller than the current disk size."))
            }
            let layoutMinimum = layoutMinimumGiB(homeStart: homeStart)
            guard toCapacityGiB >= layoutMinimum else {
                throw capacityBoundsError(minimum: layoutMinimum, maximum: Int(oldSize / (1 << 30)) - 1)
            }
            let newLastUsable = newSize / 512 - 34
            let newHomeBytes = newLastUsable > homeStart ? (newLastUsable - homeStart + 1) * 512 : 0
            guard try homeFilesystemBytes(disk: disk, homeStart: homeStart) <= newHomeBytes else {
                throw SteamOSDeploymentError(String(localized: "The SteamOS home file system has not been reduced yet, so the disk was not shrunk."))
            }
        }
        let staging = url.deletingLastPathComponent()
            .appendingPathComponent(".\(url.lastPathComponent).resize-\(UUID().uuidString)")
        guard fclonefileat(fd, AT_FDCWD, staging.path, UInt32(CLONE_ACL)) == 0 else {
            let error = errno
            if error == ENOTSUP || error == EOPNOTSUPP || error == EXDEV {
                throw SteamOSDeploymentError(String(localized: "This volume cannot clone the disk image. Growing the disk requires an APFS volume."))
            }
            throw SteamOSDeploymentError(String(localized: "Could not stage the resized disk image (\(String(cString: strerror(error))))."))
        }
        var committed = false
        defer { if !committed { unlink(staging.path) } }
        let cloneFD = open(staging.path, O_RDWR | O_NOFOLLOW | O_CLOEXEC)
        guard cloneFD >= 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not open the staged disk image."))
        }
        defer { close(cloneFD) }
        guard flock(cloneFD, LOCK_EX | LOCK_NB) == 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not lock the staged disk image."))
        }
        let writeError = SteamOSDeploymentError(String(localized: "Could not write the resized disk image."))
        if growing {
            guard ftruncate(cloneFD, off_t(newSize)) == 0 else { throw writeError }
        }
        let newLastSector = newSize / 512 - 1
        let newLastUsable = newLastSector - 33
        var newTable = layout.table
        newTable.put(newLastUsable, at: homeOffset + 40)
        var newMBR = mbr
        newMBR.put(UInt32(min(newLastSector, UInt64(UInt32.max))), at: protective + 12)
        let diskGUID = uuid(fromGPT: Data(primary[56..<72]))
        func write(_ data: Data, sector: UInt64) throws {
            try data.withUnsafeBytes { buffer in
                var done = 0
                while done < data.count {
                    let written = Darwin.pwrite(cloneFD, buffer.baseAddress! + done,
                                                data.count - done, off_t(sector * 512) + off_t(done))
                    if written < 0, errno == EINTR { continue }
                    guard written > 0 else { throw writeError }
                    done += written
                }
            }
        }
        try write(newMBR, sector: 0)
        try write(newTable, sector: newLastSector - 32)
        try write(header(current: newLastSector, backup: 1, entries: newLastSector - 32,
                         lastUsable: newLastUsable, guid: diskGUID, table: newTable), sector: newLastSector)
        if growing {
            try write(Data(repeating: 0, count: 33 * 512), sector: oldLastSector - 32)
        }
        try write(newTable, sector: 2)
        try write(header(current: 1, backup: newLastSector, entries: 2,
                         lastUsable: newLastUsable, guid: diskGUID, table: newTable), sector: 1)
        if !growing {
            guard ftruncate(cloneFD, off_t(newSize)) == 0 else { throw writeError }
        }
        if fcntl(cloneFD, F_FULLFSYNC) != 0 {
            let error = errno
            guard error == ENOTSUP || error == EINVAL, fsync(cloneFD) == 0 else {
                throw SteamOSDeploymentError(String(localized: "Could not save the resized disk image to storage."))
            }
        }
        let check = try Reader(descriptor: cloneFD)
        guard try check.partitions().contains(where: { $0.start == homeStart && $0.end == newLastUsable }) else {
            throw SteamOSDeploymentError(String(localized: "The resized disk image did not validate. The original disk was left unchanged."))
        }
        guard pathMatchesLockedFD() else { throw replaced }
        guard rename(staging.path, url.path) == 0 else {
            throw SteamOSDeploymentError(String(localized: "Could not replace the system disk with the resized image."))
        }
        committed = true
    }

    private static func uuid(fromGPT bytes: Data) -> UUID {
        var data = Data(bytes.prefix(16))
        data.replaceSubrange(0..<4, with: data[0..<4].reversed())
        data.replaceSubrange(4..<6, with: data[4..<6].reversed())
        data.replaceSubrange(6..<8, with: data[6..<8].reversed())
        return data.withUnsafeBytes { UUID(uuid: $0.load(as: uuid_t.self)) }
    }

    private static func header(current: UInt64, backup: UInt64, entries: UInt64, lastUsable: UInt64, guid: UUID, table: Data) -> Data {
        var data = Data(repeating: 0, count: 512)
        data.replaceSubrange(0..<8, with: Data("EFI PART".utf8))
        data.put(UInt32(0x10000), at: 8)
        data.put(UInt32(92), at: 12)
        data.put(current, at: 24)
        data.put(backup, at: 32)
        data.put(UInt64(34), at: 40)
        data.put(lastUsable, at: 48)
        data.replaceSubrange(56..<72, with: guidBytes(guid))
        data.put(entries, at: 72)
        data.put(UInt32(128), at: 80)
        data.put(UInt32(128), at: 84)
        data.put(crc32(table), at: 88)
        data.put(crc32(data.prefix(92)), at: 16)
        return data
    }

    private static func guidBytes(_ uuid: UUID) -> Data {
        var value = uuid.uuid
        var data = withUnsafeBytes(of: &value) { Data($0) }
        data.replaceSubrange(0..<4, with: data[0..<4].reversed())
        data.replaceSubrange(4..<6, with: data[4..<6].reversed())
        data.replaceSubrange(6..<8, with: data[6..<8].reversed())
        return data
    }

    struct Partition {
        let index: Int
        let name: String
        let start: UInt64
        let end: UInt64
    }

    private final class Reader {
        let handle: FileHandle
        let size: UInt64

        init(_ url: URL) throws {
            let fd = open(url.path, O_RDONLY | O_NOFOLLOW)
            guard fd >= 0 else { throw SteamOSDeploymentError(String(localized: "Could not read the selected image. Check that it exists and that you have permission to read it.")) }
            do {
                size = try Self.regularFileSize(fd)
            } catch {
                close(fd)
                throw error
            }
            handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        }

        /// Reads through an already open descriptor owned by the caller.
        init(descriptor: Int32) throws {
            size = try Self.regularFileSize(descriptor)
            handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: false)
        }

        private static func regularFileSize(_ fd: Int32) throws -> UInt64 {
            var info = stat()
            guard fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_size >= 34 * 512 else {
                throw SteamOSDeploymentError(String(localized: "Choose a complete raw disk image."))
            }
            return UInt64(info.st_size)
        }

        func read(at offset: UInt64, count: Int) throws -> Data {
            guard offset <= size, UInt64(count) <= size - offset else { throw invalidGPT() }
            try handle.seek(toOffset: offset)
            let data = try handle.read(upToCount: count) ?? Data()
            guard data.count == count else { throw invalidGPT() }
            return data
        }

        struct GPTLayout {
            let header: Data
            let backup: Data
            let table: Data
            let backupSector: UInt64
        }

        func gpt() throws -> GPTLayout {
            let header = try read(at: 512, count: 512)
            let table = try validateHeader(header, current: 1)
            let backupSector: UInt64 = header.integer(at: 32)
            let backup = try read(at: backupSector * 512, count: 512)
            let backupTable = try validateHeader(backup, current: backupSector)
            guard backup.integer(at: 32, as: UInt64.self) == 1,
                  header[40..<72] == backup[40..<72], table == backupTable else { throw invalidGPT() }
            return GPTLayout(header: header, backup: backup, table: table, backupSector: backupSector)
        }

        func partitions() throws -> [Partition] {
            let layout = try gpt()
            let header = layout.header
            let table = layout.table
            let first: UInt64 = header.integer(at: 40)
            let last: UInt64 = header.integer(at: 48)
            var result: [Partition] = []
            for offset in stride(from: 0, to: table.count, by: 128) {
                if table[offset..<(offset + 16)].allSatisfy({ $0 == 0 }) { continue }
                let start: UInt64 = table.integer(at: offset + 32)
                let end: UInt64 = table.integer(at: offset + 40)
                guard start >= first, end >= start, end <= last else { throw invalidGPT() }
                let nameData = table.subdata(in: (offset + 56)..<(offset + 128))
                let name = (String(data: nameData, encoding: .utf16LittleEndian) ?? "").prefix { $0 != "\0" }
                result.append(Partition(index: offset / 128, name: String(name), start: start, end: end))
            }
            let sorted = result.sorted { $0.start < $1.start }
            for (left, right) in zip(sorted, sorted.dropFirst()) where left.end >= right.start { throw invalidGPT() }
            return result
        }

        private func validateHeader(_ data: Data, current: UInt64) throws -> Data {
            guard data.prefix(8) == Data("EFI PART".utf8) else { throw invalidGPT() }
            let length = Int(data.integer(at: 12, as: UInt32.self))
            guard (92...512).contains(length), data.integer(at: 24, as: UInt64.self) == current else { throw invalidGPT() }
            var checksumData = Data(data.prefix(length))
            checksumData.put(UInt32(0), at: 16)
            guard crc32(checksumData) == data.integer(at: 16, as: UInt32.self) else { throw invalidGPT() }
            let count = Int(data.integer(at: 80, as: UInt32.self))
            let entrySize = Int(data.integer(at: 84, as: UInt32.self))
            let tableSector: UInt64 = data.integer(at: 72)
            let alternate: UInt64 = data.integer(at: 32)
            let first: UInt64 = data.integer(at: 40)
            let last: UInt64 = data.integer(at: 48)
            guard (1...128).contains(count), entrySize == 128,
                  alternate < size / 512, alternate != current, first <= last, last < size / 512,
                  tableSector >= 2, tableSector < size / 512 else { throw invalidGPT() }
            let table = try read(at: tableSector * 512, count: count * entrySize)
            guard crc32(table) == data.integer(at: 88, as: UInt32.self) else { throw invalidGPT() }
            return table
        }

        func validateFAT(_ partition: Partition) throws {
            let boot = try read(at: partition.start * 512, count: 512)
            let fat = Data("FAT".utf8)
            guard boot[510] == 0x55, boot[511] == 0xaa,
                  boot[54..<57] == fat || boot[82..<85] == fat else {
                throw SteamOSDeploymentError(String(localized: "The image’s boot partition is incomplete (\(partition.name)). Choose a complete image."))
            }
        }

        func validateBtrfs(_ partition: Partition) throws {
            guard partition.end - partition.start >= 129,
                  try read(at: partition.start * 512 + 0x10040, count: 8) == Data("_BHRfS_M".utf8) else {
                throw SteamOSDeploymentError(String(localized: "The image’s system partition is incomplete (\(partition.name)). Choose a complete image."))
            }
        }

        private func invalidGPT() -> SteamOSDeploymentError {
            SteamOSDeploymentError(String(localized: "The disk image’s GPT partition table is incomplete or damaged. Select another image or download it again."))
        }
    }

    static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xffff_ffff
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) == 1 ? 0xedb8_8320 : 0) }
        }
        return ~crc
    }

    /// CRC-32C (Castagnoli) without final inversion, the ext4 superblock convention.
    static func crc32c(_ seed: UInt32, _ data: Data) -> UInt32 {
        var crc = seed
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 { crc = (crc >> 1) ^ ((crc & 1) == 1 ? 0x82f6_3b78 : 0) }
        }
        return crc
    }
}

private extension Data {
    mutating func put<T: FixedWidthInteger>(_ value: T, at offset: Int) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { replaceSubrange(offset..<(offset + MemoryLayout<T>.size), with: $0) }
    }

    func integer<T: FixedWidthInteger>(at offset: Int, as: T.Type = T.self) -> T {
        var value: T = 0
        for index in 0..<MemoryLayout<T>.size { value |= T(self[offset + index]) << (index * 8) }
        return value
    }
}
