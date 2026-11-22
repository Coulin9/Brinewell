import AppKit
import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class RuntimeStartupTests: XCTestCase {
    @MainActor
    func testCancelledStartupReleasesDiskAndDoesNotLaunchServices() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? fm.removeItem(at: root) }
        let resources = root.appendingPathComponent("Contents/Resources/Runtime")
        let renderer = root.appendingPathComponent("Contents/Helpers/virgl_render_server")
        let driver = root.appendingPathComponent("Contents/Frameworks/libvulkan_kosmickrisp.dylib")
        let run = root.appendingPathComponent("run")
        for directory in [resources, renderer.deletingLastPathComponent(), driver.deletingLastPathComponent(), run] {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true,
                                   attributes: [.posixPermissions: 0o700])
        }
        for name in ["Image", "initramfs.cpio.gz", "brinewell-layer.img", "gvproxy"] {
            let file = resources.appendingPathComponent(name)
            try Data().write(to: file)
            try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: file.path)
        }
        try Data().write(to: renderer)
        try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: renderer.path)
        try Data().write(to: driver)
        let disk = root.appendingPathComponent("disk.img")
        try Data().write(to: disk)
        let arguments = ["--resources", resources.path, "--disk", disk.path, "--cpus", "1", "--memory-gib", "1",
                         "--run-directory", run.path, "--graphics-backend", "venus"]
        let windows = Set(NSApplication.shared.windows.map(ObjectIdentifier.init))
        for cancelTask in [false, true] {
            let options = try RuntimeOptions(arguments: arguments)
            let runtime: Runtime
            do { runtime = try Runtime(options: options) }
            catch { close(options.diskFD); throw error }
            let starting = Task { try await runtime.start() }
            if cancelTask { starting.cancel() } else { runtime.shutdown() }
            do {
                try await starting.value
                XCTFail("A cancelled startup must not launch the VM")
            } catch is CancellationError { }
            XCTAssertEqual(Set(NSApplication.shared.windows.map(ObjectIdentifier.init)), windows)
            XCTAssertTrue(try fm.contentsOfDirectory(atPath: run.path).isEmpty)
            let retry = try RuntimeOptions(arguments: arguments)
            close(retry.diskFD)
        }
    }
}
