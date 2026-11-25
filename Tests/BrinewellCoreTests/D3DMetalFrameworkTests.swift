import Foundation
import XCTest
@testable import BrinewellCore

final class D3DMetalFrameworkTests: XCTestCase {
    private func temporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: url) }
        return url
    }

    func testResolutionAndVersions() throws {
        let root = try temporaryDirectory()
        let bundle = root.appendingPathComponent("D3DMetal.framework")
        let resources = bundle.appendingPathComponent("Versions/A/Resources")
        try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: true)
        let binary = bundle.appendingPathComponent("Versions/A/D3DMetal")
        try Data([0xcf, 0xfa, 0xed, 0xfe, 7, 0, 0, 1]).write(to: binary)
        XCTAssertEqual(try D3DMetalFramework.resolve(at: bundle).binaryURL, binary.resolvingSymlinksInPath().standardizedFileURL)
        XCTAssertFalse(try D3DMetalFramework.resolve(at: binary).isWholeBundle)
        XCTAssertThrowsError(try D3DMetalFramework.resolve(at: binary, allowBareBinary: false))
        let version = try PropertyListSerialization.data(fromPropertyList: ["CFBundleShortVersionString": "4.0b3"], format: .binary, options: 0)
        try version.write(to: resources.appendingPathComponent("version.plist"))
        let info = try PropertyListSerialization.data(fromPropertyList: ["CFBundleVersion": "42", "SourceVersion": "33039000000000"], format: .xml, options: 0)
        try info.write(to: resources.appendingPathComponent("Info.plist"))
        let inspected = try D3DMetalFramework.inspect(at: bundle)
        XCTAssertEqual(inspected.version?.bundleVersion, "42")
        XCTAssertTrue(try XCTUnwrap(inspected.version).isRecommendedContextVersion)
        XCTAssertFalse(D3DMetalVersionInfo(shortVersion: "3.0", bundleVersion: nil, sourceVersion: nil).isRecommendedContextVersion)
        XCTAssertFalse(D3DMetalVersionInfo(shortVersion: "4.0b4", bundleVersion: nil, sourceVersion: nil).isRecommendedContextVersion)
        try FileManager.default.moveItem(at: binary, to: bundle.appendingPathComponent("D3DMetal"))
        XCTAssertEqual(try D3DMetalFramework.resolve(at: bundle).binaryURL.lastPathComponent, "D3DMetal")
        XCTAssertEqual(try D3DMetalFramework.resolve(at: bundle).binaryURL.deletingLastPathComponent().path, bundle.resolvingSymlinksInPath().standardizedFileURL.path)
    }

    func testEscapingSymlink() throws {
        let root = try temporaryDirectory()
        let bundle = root.appendingPathComponent("D3DMetal.framework")
        try FileManager.default.createDirectory(at: bundle, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(atPath: bundle.appendingPathComponent("escape").path, withDestinationPath: root.path)
        XCTAssertThrowsError(try D3DMetalFramework.validateBundleSymlinks(at: bundle)) { error in
            guard case D3DMetalFrameworkError.escapingSymlink = error else { return XCTFail("\(error)") }
        }
    }

    func testSignatureArgumentsAndFailure() throws {
        let root = try temporaryDirectory()
        let bundle = root.appendingPathComponent("D3DMetal.framework")
        let version = bundle.appendingPathComponent("Versions/A")
        try FileManager.default.createDirectory(at: version, withIntermediateDirectories: true)
        let binary = version.appendingPathComponent("D3DMetal")
        try Data().write(to: binary)
        try FileManager.default.createSymbolicLink(atPath: bundle.appendingPathComponent("Versions/Current").path, withDestinationPath: "A")
        let calls = SignatureCalls()
        let success: D3DMetalCommandRunner = { executable, arguments in
            XCTAssertEqual(executable.path, "/usr/bin/codesign")
            calls.append(arguments)
            return (0, "", "")
        }
        try D3DMetalFramework.verifyAppleCodeSignature(bundleURL: bundle, runner: success)
        XCTAssertEqual(calls.values, [
            ["--verify", "--deep", "--strict", "--architecture", "x86_64", bundle.path],
            ["--verify", "--strict", "-R", D3DMetalFramework.appleSigningRequirement, bundle.path],
            ["--verify", "--strict", "--architecture", "x86_64", "-R", D3DMetalFramework.appleSigningRequirement, binary.resolvingSymlinksInPath().path],
        ])
        XCTAssertThrowsError(try D3DMetalFramework.verifyAppleCodeSignature(bundleURL: bundle, runner: { _, _ in (1, "", "bad\nerror") })) { error in
            XCTAssertEqual(error as? D3DMetalFrameworkError, .signatureVerificationFailed("bad error"))
        }
    }

    func testRejectsMultipleVersions() throws {
        let root = try temporaryDirectory().appendingPathComponent("D3DMetal.framework")
        for version in ["A", "B"] {
            try FileManager.default.createDirectory(at: root.appendingPathComponent("Versions/" + version), withIntermediateDirectories: true)
        }
        XCTAssertThrowsError(try D3DMetalFramework.resolve(at: root))
        XCTAssertThrowsError(try D3DMetalFramework.verifyAppleCodeSignature(bundleURL: root, runner: { _, _ in (0, "", "") }))
    }

    func testRejectsHiddenAndRealCurrentVersions() throws {
        for extra in [".hidden", "Current"] {
            let root = try temporaryDirectory().appendingPathComponent("D3DMetal.framework")
            for version in ["A", extra] {
                try FileManager.default.createDirectory(at: root.appendingPathComponent("Versions/" + version), withIntermediateDirectories: true)
            }
            XCTAssertThrowsError(try D3DMetalFramework.resolve(at: root)) { error in
                guard case D3DMetalFrameworkError.signatureVerificationFailed = error else { return XCTFail("\(error)") }
            }
        }
    }

    func testUpdateRepairsInvalidConfiguration() throws {
        let root = try temporaryDirectory()
        let store = ConfigurationStore(url: root.appendingPathComponent("config.json"))
        let host = HostCapabilities(logicalCPUCount: 8, physicalMemoryBytes: 16 * 1_073_741_824, architecture: "arm64", metalDeviceName: nil)
        var invalid = VMConfiguration.defaults(for: host)
        invalid.cpuCount = 999
        invalid.d3dMetalFrameworkPath = "/tmp/example.framework"
        try JSONEncoder().encode(invalid).write(to: store.url)
        try store.update(for: host) { $0.cpuCount = 4 }
        let repaired = try store.load(for: host)
        XCTAssertEqual(repaired.cpuCount, 4)
        XCTAssertEqual(repaired.d3dMetalFrameworkPath, "/tmp/example.framework")
    }

    func testLocalFrameworkIfAvailable() throws {
        let bundle = URL(fileURLWithPath: "/Users/mi/steamOS_VM_D3DMetal/Frameworks/D3DMetal.framework")
        guard FileManager.default.fileExists(atPath: bundle.path) else { throw XCTSkip("local framework unavailable") }
        let inspection = try D3DMetalFramework.inspect(at: bundle, verifyAppleSignature: true)
        XCTAssertEqual(inspection.version?.shortVersion, "4.0b3")
    }

    func testReadinessMatrix() throws {
        var config = VMConfiguration(cpuCount: 1, memoryGiB: 1)
        let disk = URL(fileURLWithPath: "/tmp/disk.img")
        let framework = URL(fileURLWithPath: "/tmp/D3DMetal.framework")
        let inspection = D3DMetalInspection(resolvedBundleURL: framework, binaryURL: framework.appendingPathComponent("D3DMetal"), version: nil, isWholeBundle: true)
        func evaluate(_ exists: Bool = true, rosetta: Bool = true) -> SetupReadiness {
            SetupReadiness.evaluate(config, diskExists: { _ in exists }, validateDisk: { _ in }, inspectFramework: { _ in inspection }, rosettaAvailable: { rosetta })
        }
        XCTAssertEqual(evaluate().neededSteps, [.systemDisk, .graphics])
        config.diskImagePath = disk.path
        XCTAssertEqual(evaluate().neededSteps, [.graphics])
        config.d3dMetalFrameworkPath = framework.path
        XCTAssertTrue(evaluate().isReadyToStart)
        XCTAssertEqual(evaluate(rosetta: false).graphics, .rosettaMissing)
        XCTAssertEqual(evaluate(false).disk, .missingConfigured(disk))
        XCTAssertEqual(evaluate(false).neededSteps, [.systemDisk])
        config.diskImagePath = nil
        XCTAssertEqual(evaluate().neededSteps, [.systemDisk])
        config.d3dMetalFrameworkPath = nil
        XCTAssertEqual(evaluate().nextStep(after: .systemDisk), .graphics)
        for backend in [GraphicsBackend.automatic, .neptune] {
            config.graphicsBackend = backend
            XCTAssertEqual(evaluate().graphics, .d3dMetalMissing)
            config.d3dMetalFrameworkPath = framework.path
            XCTAssertEqual(evaluate(rosetta: false).graphics, .rosettaMissing)
            config.d3dMetalFrameworkPath = nil
        }
        for backend in [GraphicsBackend.venus, .mullion] {
            config.graphicsBackend = backend
            XCTAssertEqual(evaluate().neededSteps, [.systemDisk])
            XCTAssertEqual(evaluate().graphics, .notRequiredForBackend(backend))
        }
    }

    func testConfigurationUpdatesPreserveOtherFields() throws {
        let root = try temporaryDirectory()
        let store = ConfigurationStore(url: root.appendingPathComponent("config.json"))
        let host = HostCapabilities(logicalCPUCount: 8, physicalMemoryBytes: 16 * 1_073_741_824, architecture: "arm64", metalDeviceName: nil)
        try store.update(for: host) { $0.diskImagePath = "/tmp/disk.img" }
        try store.update(for: host) { $0.d3dMetalFrameworkPath = "/tmp/D3DMetal.framework" }
        DispatchQueue.concurrentPerform(iterations: 20) { index in
            try! store.update(for: host) { configuration in
                if index.isMultiple(of: 2) { configuration.diskImagePath = "/tmp/disk.img" }
                else { configuration.d3dMetalFrameworkPath = "/tmp/D3DMetal.framework" }
            }
        }
        let result = try store.load(for: host)
        XCTAssertEqual(result.diskImagePath, "/tmp/disk.img")
        XCTAssertEqual(result.d3dMetalFrameworkPath, "/tmp/D3DMetal.framework")
    }
}

private final class SignatureCalls: @unchecked Sendable {
    private let lock = NSLock()
    private var calls: [[String]] = []
    func append(_ value: [String]) { lock.withLock { calls.append(value) } }
    var values: [[String]] { lock.withLock { calls } }
}
