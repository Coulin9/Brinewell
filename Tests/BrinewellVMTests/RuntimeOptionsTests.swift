import BrinewellCore
import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class RuntimeOptionsTests: XCTestCase {
    private func withRuntimeArguments(_ body: ([String], URL) throws -> Void) throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? fm.removeItem(at: root) }
        let contents = root.appendingPathComponent("Contents", isDirectory: true)
        let resources = contents.appendingPathComponent("Resources/Runtime", isDirectory: true)
        let helpers = contents.appendingPathComponent("Helpers", isDirectory: true)
        let frameworks = contents.appendingPathComponent("Frameworks", isDirectory: true)
        let run = root.appendingPathComponent("run", isDirectory: true)
        for directory in [resources, helpers, frameworks] {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        try fm.createDirectory(at: run, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        for name in ["Image", "initramfs.cpio.gz", "brinewell-layer.img", "gvproxy"] {
            let url = resources.appendingPathComponent(name)
            try Data().write(to: url)
            try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: url.path)
        }
        let renderer = helpers.appendingPathComponent("virgl_render_server")
        try Data().write(to: renderer)
        try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: renderer.path)
        try Data().write(to: frameworks.appendingPathComponent("libvulkan_kosmickrisp.dylib"))
        let disk = root.appendingPathComponent("disk.img")
        try Data().write(to: disk)
        try body(["--resources", resources.path, "--disk", disk.path, "--cpus", "1", "--memory-gib", "1",
                  "--run-directory", run.path, "--graphics-backend", "venus"], root)
    }

    func testOmittedDisplayOptionsKeepLegacyDefaults() throws {
        try withRuntimeArguments { arguments, _ in
            let options = try RuntimeOptions(arguments: arguments)
            defer { close(options.diskFD) }
            XCTAssertEqual(options.display, VMDisplayConfiguration())
            XCTAssertNil(options.hostDisplayID)
        }
    }

    func testGameHUDOptionsDefaultOffAndAcceptLegacyStates() throws {
        try withRuntimeArguments { arguments, _ in
            let cases: [([String], GameHUDLevel)] = [
                ([], .off), (["--game-hud", "0"], .off), (["--game-hud", "1"], .compact),
            ]
            for (flags, expected) in cases {
                let options = try RuntimeOptions(arguments: arguments + flags)
                defer { close(options.diskFD) }
                XCTAssertEqual(options.gameHUDLevel, expected)
                XCTAssertEqual(options.gameHUDPosition, .topRight)
            }
        }
    }

    func testEveryGameHUDLevelAndPositionReachesRuntimeOptions() throws {
        try withRuntimeArguments { arguments, _ in
            let levels: [(String, GameHUDLevel)] = [
                ("0", .off), ("1", .fps), ("2", .compact), ("3", .detailed), ("4", .full),
            ]
            let positions: [(String, GameHUDPosition)] = [
                ("top-left", .topLeft), ("top-right", .topRight),
                ("bottom-left", .bottomLeft), ("bottom-right", .bottomRight),
            ]
            for (number, level) in levels {
                for (name, position) in positions {
                    let options = try RuntimeOptions(arguments: arguments + [
                        "--game-hud-level", number, "--game-hud-position", name,
                    ])
                    defer { close(options.diskFD) }
                    XCTAssertEqual(options.gameHUDLevel, level)
                    XCTAssertEqual(options.gameHUDPosition, position)
                    XCTAssertEqual(options.cpus, 1)
                    XCTAssertEqual(options.memoryMiB, 1024)
                    XCTAssertEqual(options.graphicsBackend, .venus)
                    XCTAssertEqual(options.display, VMDisplayConfiguration())
                }
            }
            for (flags, level) in [(["--game-hud-position", "bottom-left"], GameHUDLevel.off),
                                   (["--game-hud", "1", "--game-hud-position", "bottom-left"], .compact)] {
                let options = try RuntimeOptions(arguments: arguments + flags)
                defer { close(options.diskFD) }
                XCTAssertEqual(options.gameHUDLevel, level)
                XCTAssertEqual(options.gameHUDPosition, .bottomLeft)
            }
        }
    }

    func testInvalidGameHUDOptionsAreRejected() throws {
        try withRuntimeArguments { arguments, _ in
            for flags in [
                ["--game-hud"], ["--game-hud", "true"], ["--game-hud", "2"],
                ["--game-hud", "1", "--game-hud", "0"],
                ["--game-hud-level"], ["--game-hud-level", "-1"], ["--game-hud-level", "5"],
                ["--game-hud-level", "1.5"], ["--game-hud-level", "compact"],
                ["--game-hud-level", "99999999999999999999"],
                ["--game-hud-level", "2", "--game-hud-level", "2"],
                ["--game-hud-position"], ["--game-hud-position", "center"],
                ["--game-hud-position", "0"], ["--game-hud-position", "top-left", "--game-hud-position", "top-right"],
                ["--game-hud", "1", "--game-hud-level", "2"],
                ["--game-hud-level", "0", "--game-hud", "0"],
            ] {
                do {
                    let options = try RuntimeOptions(arguments: arguments + flags)
                    close(options.diskFD)
                    XCTFail("Invalid HUD options were accepted: \(flags)")
                } catch { }
            }
        }
    }

    func testNetworkConfigurationDefaultsToDirectAndReadsPrivateFile() throws {
        try withRuntimeArguments { arguments, root in
            let direct = try RuntimeOptions(arguments: arguments)
            close(direct.diskFD)
            XCTAssertEqual(direct.network, VMNetworkConfiguration())

            let network = VMNetworkConfiguration(proxyEnabled: true, proxyURL: "https://localhost:8443")
            let file = root.appendingPathComponent("network.json")
            try network.write(to: file)
            let options = try RuntimeOptions(arguments: arguments + ["--network-config", file.path])
            defer { close(options.diskFD) }
            XCTAssertEqual(options.network, network)
        }
    }

    func testInvalidNetworkConfigurationCannotStart() throws {
        try withRuntimeArguments { arguments, root in
            let file = root.appendingPathComponent("network.json")
            let invalid = VMNetworkConfiguration(proxyEnabled: true, proxyURL: "http://localhost")
            try JSONEncoder().encode(invalid).write(to: file)
            for flags in [["--network-config", file.path], ["--network-config", "relative.json"],
                          ["--network-config", root.appendingPathComponent("missing.json").path],
                          ["--network-config"], ["--network-config", file.path, "--network-config", file.path]] {
                XCTAssertThrowsError(try RuntimeOptions(arguments: arguments + flags))
            }
        }
    }

    func testSharedFolderOptions() throws {
        try withRuntimeArguments { arguments, root in
            let share = root.appendingPathComponent("Mac 分享")
            try FileManager.default.createDirectory(at: share, withIntermediateDirectories: false)
            let path = share.resolvingSymlinksInPath().path
            for (flags, expected) in [
                ([], nil),
                (["--shared-folder", share.path], VMSharedFolder(path: path)),
                (["--shared-folder", share.path, "--shared-folder-read-only", "0"], VMSharedFolder(path: path, readOnly: false)),
            ] {
                let options = try RuntimeOptions(arguments: arguments + flags)
                defer { close(options.diskFD) }
                XCTAssertEqual(options.sharedFolder, expected)
            }
        }
    }

    func testInvalidSharedFolderOptionsAreRejected() throws {
        try withRuntimeArguments { arguments, root in
            for flags in [
                ["--shared-folder-read-only", "1"],
                ["--shared-folder", "relative"],
                ["--shared-folder", "/tmp/share\0suffix"],
                ["--shared-folder", root.appendingPathComponent("disk.img").path],
                ["--shared-folder", "/tmp", "--shared-folder-read-only", "invalid"],
            ] {
                do {
                    let options = try RuntimeOptions(arguments: arguments + flags)
                    close(options.diskFD)
                    XCTFail("Invalid shared folder options were accepted: \(flags)")
                } catch { }
            }
        }
    }

    func testCustomModeReachesRuntimeOptions() throws {
        try withRuntimeArguments { arguments, _ in
            let options = try RuntimeOptions(arguments: arguments + ["--display-width", "1920", "--display-height", "1080",
                                                                   "--display-refresh-rate", "90", "--host-display-id", "42"])
            defer { close(options.diskFD) }
            XCTAssertEqual(options.display, VMDisplayConfiguration(width: 1920, height: 1080, refreshRate: 90))
            XCTAssertEqual(options.hostDisplayID, 42)
        }
    }

    func testShrinkHomeOptionRequiresPositiveMultipleOfFour() throws {
        try withRuntimeArguments { arguments, _ in
            let unset = try RuntimeOptions(arguments: arguments)
            close(unset.diskFD)
            XCTAssertNil(unset.shrinkHomeKiB)
            for (flags, expected) in [
                (["--shrink-home-kib", "4"], UInt64(4)),
                (["--shrink-home-kib", "1048576"], UInt64(1048576)),
            ] {
                let options = try RuntimeOptions(arguments: arguments + flags)
                defer { close(options.diskFD) }
                XCTAssertEqual(options.shrinkHomeKiB, expected)
            }
        }
    }

    func testInvalidShrinkHomeOptionsAreRejected() throws {
        try withRuntimeArguments { arguments, _ in
            for flags in [
                ["--shrink-home-kib"], ["--shrink-home-kib", "0"], ["--shrink-home-kib", "6"],
                ["--shrink-home-kib", "-4"], ["--shrink-home-kib", "1.5"],
                ["--shrink-home-kib", "garbage"], ["--shrink-home-kib", "99999999999999999999"],
                ["--shrink-home-kib", "8", "--shrink-home-kib", "16"],
            ] {
                do {
                    let options = try RuntimeOptions(arguments: arguments + flags)
                    close(options.diskFD)
                    XCTFail("Invalid shrink option was accepted: \(flags)")
                } catch { }
            }
        }
    }

    func testInvalidModesAndMalformedArgumentsAreRejected() throws {
        try withRuntimeArguments { arguments, _ in
            for displayArguments in [
                ["--display-width", "not-a-number"], ["--display-height", "8193"],
                ["--display-refresh-rate", "0"], ["--display-width", "99999999999999999999"],
                ["--display-width", "8192", "--display-height", "8192", "--display-refresh-rate", "10000"],
                ["--display-width", "1920", "--display-width", "1280"], ["--display-refresh-rate"],
                ["--host-display-id", "0"], ["--host-display-id", "-1"], ["--host-display-id", "not-a-number"],
            ] {
                do {
                    let options = try RuntimeOptions(arguments: arguments + displayArguments)
                    close(options.diskFD)
                    XCTFail("Invalid display options were accepted: \(displayArguments)")
                } catch { }
            }
        }
    }
}
