import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class RuntimeDiskLockTests: XCTestCase {
    func testHardLinkSharesDiskLockAndCanReacquireAfterClose() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let contents = root.appendingPathComponent("Contents", isDirectory: true)
        let resources = contents.appendingPathComponent("Resources/Runtime", isDirectory: true)
        let helpers = contents.appendingPathComponent("Helpers", isDirectory: true)
        let frameworks = contents.appendingPathComponent("Frameworks", isDirectory: true)
        let runDirectory = root.appendingPathComponent("run", isDirectory: true)
        try fm.createDirectory(at: resources, withIntermediateDirectories: true)
        try fm.createDirectory(at: helpers, withIntermediateDirectories: true)
        try fm.createDirectory(at: frameworks, withIntermediateDirectories: true)
        let renderer = helpers.appendingPathComponent("virgl_render_server")
        try Data().write(to: renderer)
        try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: renderer.path)
        try Data().write(to: frameworks.appendingPathComponent("libvulkan_kosmickrisp.dylib"))
        try fm.createDirectory(at: runDirectory, withIntermediateDirectories: false,
                               attributes: [.posixPermissions: 0o700])
        for name in ["Image", "initramfs.cpio.gz", "brinewell-layer.img", "gvproxy"] {
            let path = resources.appendingPathComponent(name)
            try Data().write(to: path)
            if name == "gvproxy" {
                try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: path.path)
            }
        }
        let disk = root.appendingPathComponent("disk.img")
        let alias = root.appendingPathComponent("disk-alias.img")
        try Data().write(to: disk)
        try fm.linkItem(at: disk, to: alias)

        func arguments(disk: URL) -> [String] {
            ["--resources", resources.path, "--disk", disk.path, "--cpus", "1",
             "--memory-gib", "1", "--run-directory", runDirectory.path, "--graphics-backend", "venus"]
        }

        // A failed default D3DMetal preflight must not leave the disk locked.
        XCTAssertThrowsError(try RuntimeOptions(arguments: Array(arguments(disk: disk).dropLast(2))))
        let first = try RuntimeOptions(arguments: arguments(disk: disk))
        var firstFDOpen = true
        defer { if firstFDOpen { close(first.diskFD) } }
        do {
            let unexpected = try RuntimeOptions(arguments: arguments(disk: alias))
            close(unexpected.diskFD)
            XCTFail("A hard link acquired the already locked disk")
        } catch {
            // Lock exclusion is the assertion; the error wording is not part of the contract.
        }
        close(first.diskFD)
        firstFDOpen = false
        let reacquired = try RuntimeOptions(arguments: arguments(disk: alias))
        close(reacquired.diskFD)
    }

    func testRenamedOverDiskPathFailsIdentityCheckUntilReopened() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let disk = root.appendingPathComponent("disk.img")
        let replacement = root.appendingPathComponent("replacement.img")
        try Data("a".utf8).write(to: disk)
        try Data("b".utf8).write(to: replacement)
        let fd = open(disk.path, O_RDWR | O_CLOEXEC)
        XCTAssertGreaterThanOrEqual(fd, 0)
        defer { close(fd) }
        XCTAssertEqual(flock(fd, LOCK_EX | LOCK_NB), 0)
        XCTAssertTrue(diskFDMatchesPath(fd, path: disk.path))
        XCTAssertEqual(rename(replacement.path, disk.path), 0)
        XCTAssertFalse(diskFDMatchesPath(fd, path: disk.path))
        let fresh = open(disk.path, O_RDWR | O_CLOEXEC)
        XCTAssertGreaterThanOrEqual(fresh, 0)
        defer { close(fresh) }
        XCTAssertEqual(flock(fresh, LOCK_EX | LOCK_NB), 0)
        XCTAssertTrue(diskFDMatchesPath(fresh, path: disk.path))
    }
}
