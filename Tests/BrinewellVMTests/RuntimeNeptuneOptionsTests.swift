import BrinewellCore
import Darwin
import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class RuntimeNeptuneOptionsTests: XCTestCase {
    private var packageRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }

    private func withPackagedRuntime(_ arguments: [String], check: (RuntimeOptions) throws -> Void) throws {
        let contents = packageRoot.appendingPathComponent("build/Brinewell.app/Contents", isDirectory: true)
        let resources = contents.appendingPathComponent("Resources/Runtime", isDirectory: true)
        guard FileManager.default.fileExists(atPath: resources.path) else {
            throw XCTSkip("built app bundle is unavailable")
        }

        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let disk = root.appendingPathComponent("disk.img")
        try Data(count: 1 << 20).write(to: disk)
        let run = root.appendingPathComponent("run", isDirectory: true)
        try FileManager.default.createDirectory(at: run, withIntermediateDirectories: false,
                                                attributes: [.posixPermissions: 0o700])

        let options = try RuntimeOptions(arguments: [
            "--resources", resources.path,
            "--disk", disk.path,
            "--cpus", "1",
            "--memory-gib", "1",
            "--run-directory", run.path,
        ] + arguments)
        defer { close(options.diskFD) }
        try check(options)
    }

    func testNeptunePreflightAcceptsPackagedUniversalWorkerAndExternalFramework() throws {
        let framework = packageRoot.deletingLastPathComponent()
            .appendingPathComponent("Frameworks/D3DMetal.framework", isDirectory: true)
        guard FileManager.default.fileExists(atPath: framework.path) else {
            throw XCTSkip("D3DMetal framework is unavailable")
        }
        try withPackagedRuntime([
            "--graphics-backend", "neptune",
            "--d3dmetal-framework", framework.path,
            "--neptune-aperture-mib", "32768",
        ]) { options in
            let support = try XCTUnwrap(options.neptune)
            XCTAssertEqual(options.graphicsBackend, .neptune)
            XCTAssertEqual(support.apertureMiB, 32768)
            XCTAssertFalse(support.multiRing)
            guard case .d3dmetal(_, let version) = support.backend else {
                return XCTFail("expected the D3DMetal backend")
            }
            XCTAssertEqual(version, "4.0b3")
            XCTAssertTrue(try MachOArchitecture.architectures(at: support.adapter).contains("x86_64"))
            XCTAssertEqual(try MachOArchitecture.architectures(at: support.server), ["arm64", "x86_64"])
        }
    }

    func testMullionPreflightUsesPackagedArm64Backend() throws {
        let adapter = packageRoot.appendingPathComponent("build/Brinewell.app/Contents/Frameworks/libmullion-native.dylib")
        guard FileManager.default.isReadableFile(atPath: adapter.path) else {
            throw XCTSkip("packaged Mullion backend is unavailable")
        }
        try withPackagedRuntime(["--graphics-backend", "mullion", "--neptune-multi-ring", "1"]) { options in
            let support = try XCTUnwrap(options.neptune)
            XCTAssertEqual(options.graphicsBackend, .mullion)
            guard case .mullion = support.backend else {
                return XCTFail("expected the Mullion backend")
            }
            XCTAssertEqual(support.adapter.path, adapter.path)
            XCTAssertTrue(try MachOArchitecture.architectures(at: support.adapter).contains("arm64"))
            XCTAssertTrue(support.multiRing)
        }
    }

    func testUnspecifiedBackendDefaultsToD3DMetal() throws {
        let framework = packageRoot.deletingLastPathComponent()
            .appendingPathComponent("Frameworks/D3DMetal.framework", isDirectory: true)
        guard FileManager.default.fileExists(atPath: framework.path) else {
            throw XCTSkip("D3DMetal framework is unavailable")
        }
        try withPackagedRuntime(["--d3dmetal-framework", framework.path]) { options in
            XCTAssertEqual(options.graphicsBackend, .neptune)
            let support = try XCTUnwrap(options.neptune)
            guard case .d3dmetal = support.backend else {
                return XCTFail("expected the D3DMetal backend")
            }
        }
    }

    func testMissingDefaultFrameworkDoesNotFallBackToVenus() throws {
        do {
            try withPackagedRuntime([]) { _ in XCTFail("D3DMetal requires a framework") }
            XCTFail("missing framework was accepted")
        } catch let error as RuntimeError {
            XCTAssertEqual(error.description, "Neptune is not available: D3DMetal framework path is not configured")
        }
    }

    func testVenusCanStillBeSelectedWithoutD3DMetal() throws {
        try withPackagedRuntime(["--graphics-backend", "venus"]) { options in
            XCTAssertEqual(options.graphicsBackend, .venus)
            // Venus itself never depends on Rosetta or the framework; the
            // initial support is simply any validated Neptune fallback.
            XCTAssertEqual(options.neptune != nil,
                           options.neptuneD3DMetal != nil || options.neptuneMullion != nil)
            XCTAssertTrue(options.availableBackends.contains(.venus))
        }
    }

    func testAutomaticIsNoLongerARuntimeBackend() throws {
        do {
            try withPackagedRuntime(["--graphics-backend", "automatic"]) { _ in
                XCTFail("automatic backend is no longer supported")
            }
            XCTFail("automatic backend was accepted")
        } catch let error as RuntimeError {
            XCTAssertEqual(error.description, "invalid graphics backend: automatic")
        }
    }
}
