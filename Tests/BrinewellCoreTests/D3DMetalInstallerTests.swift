import Foundation
import XCTest
@testable import BrinewellCore

final class D3DMetalInstallerTests: XCTestCase {
    private func fixture() throws -> (URL, URL) {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: root) }
        let bundle = root.appendingPathComponent("source/D3DMetal.framework")
        let resources = bundle.appendingPathComponent("Versions/A/Resources")
        try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: true)
        try Data([0xcf, 0xfa, 0xed, 0xfe, 7, 0, 0, 1]).write(to: bundle.appendingPathComponent("Versions/A/D3DMetal"))
        let plist = try PropertyListSerialization.data(fromPropertyList: ["CFBundleShortVersionString": "4.0b3"], format: .xml, options: 0)
        try plist.write(to: resources.appendingPathComponent("version.plist"))
        try FileManager.default.createSymbolicLink(atPath: bundle.appendingPathComponent("D3DMetal").path, withDestinationPath: "Versions/A/D3DMetal")
        return (root, bundle)
    }

    func testImportAndRollback() async throws {
        let (root, source) = try fixture()
        let directory = root.appendingPathComponent("installed")
        let store = ConfigurationStore(url: root.appendingPathComponent("configuration.json"))
        let installer = D3DMetalInstaller(requireAppleSignature: false)
        let result = try await installer.installFramework(from: source, into: directory) { url in
            try store.update { $0.d3dMetalFrameworkPath = url.path }
        }
        XCTAssertFalse(result.replacedExisting)
        XCTAssertTrue(FileManager.default.fileExists(atPath: source.path))
        XCTAssertEqual(try FileManager.default.destinationOfSymbolicLink(atPath: result.installedFrameworkURL.appendingPathComponent("D3DMetal").path), "Versions/A/D3DMetal")
        XCTAssertEqual(try store.decode().d3dMetalFrameworkPath, result.installedFrameworkURL.path)
        let marker = result.installedFrameworkURL.appendingPathComponent("marker")
        try Data("old".utf8).write(to: marker)
        do {
            _ = try await installer.installFramework(from: source, into: directory) { _ in throw CocoaError(.fileWriteUnknown) }
            XCTFail("Expected failed commit")
        } catch let error as D3DMetalInstallerError {
            guard case .commitFailed = error else { return XCTFail("\(error)") }
        }
        XCTAssertEqual(try Data(contentsOf: marker), Data("old".utf8))
        XCTAssertEqual(try store.decode().d3dMetalFrameworkPath, result.installedFrameworkURL.path)
    }

    func testRealDiskImage() async throws {
        let (root, source) = try fixture()
        let payload = root.appendingPathComponent("payload/redist/lib/external")
        try FileManager.default.createDirectory(at: payload, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: source, to: payload.appendingPathComponent("D3DMetal.framework"))
        let outerPayload = root.appendingPathComponent("outer")
        try FileManager.default.createDirectory(at: outerPayload, withIntermediateDirectories: true)
        let inner = outerPayload.appendingPathComponent("Evaluation environment for Windows games 4.0 beta 3.dmg")
        let image = root.appendingPathComponent("fixture.dmg")
        let created = try D3DMetalInstaller.defaultProcessRunner(URL(fileURLWithPath: "/usr/bin/hdiutil"), ["create", "-quiet", "-srcfolder", root.appendingPathComponent("payload").path, "-format", "UDZO", inner.path], nil)
        XCTAssertEqual(created.exitCode, 0, String(decoding: created.stderr, as: UTF8.self))
        let outer = try D3DMetalInstaller.defaultProcessRunner(URL(fileURLWithPath: "/usr/bin/hdiutil"), ["create", "-quiet", "-srcfolder", outerPayload.path, "-format", "UDZO", image.path], nil)
        XCTAssertEqual(outer.exitCode, 0, String(decoding: outer.stderr, as: UTF8.self))
        let before = try mountedPrivatePaths()
        let result = try await D3DMetalInstaller(requireAppleSignature: false).installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }
        XCTAssertEqual(result.inspection.version?.shortVersion, "4.0b3")
        XCTAssertTrue(FileManager.default.fileExists(atPath: image.path))
        XCTAssertEqual(try mountedPrivatePaths().subtracting(before), [])
    }

    func testEscapingSymlinkAndSignatureFailure() async throws {
        let (root, source) = try fixture()
        let destination = root.appendingPathComponent("installed")
        let escape = source.appendingPathComponent("escape")
        try FileManager.default.createSymbolicLink(atPath: escape.path, withDestinationPath: "../../outside")
        do {
            _ = try await D3DMetalInstaller(requireAppleSignature: false).installFramework(from: source, into: destination) { _ in }
            XCTFail("Expected invalid bundle")
        } catch let error as D3DMetalInstallerError {
            guard case .invalidBundle = error else { return XCTFail("\(error)") }
        }
        try FileManager.default.removeItem(at: escape)
        do {
            _ = try await D3DMetalInstaller(signatureVerifier: { _ in throw CocoaError(.fileReadCorruptFile) }).installFramework(from: source, into: destination) { _ in }
            XCTFail("Expected verification failure")
        } catch let error as D3DMetalInstallerError {
            guard case .verificationFailed = error else { return XCTFail("\(error)") }
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.appendingPathComponent("D3DMetal.framework").path))
    }
    private func mountedPrivatePaths() throws -> Set<String> {
        let result = try D3DMetalInstaller.defaultProcessRunner(URL(fileURLWithPath: "/usr/bin/hdiutil"), ["info", "-plist"], nil)
        XCTAssertEqual(result.exitCode, 0)
        let plist = try XCTUnwrap(PropertyListSerialization.propertyList(from: result.stdout, format: nil) as? [String: Any])
        let images = try XCTUnwrap(plist["images"] as? [[String: Any]])
        return Set(images.flatMap { $0["system-entities"] as? [[String: Any]] ?? [] }.compactMap { $0["mount-point"] as? String }.filter { $0.contains("/D3DMetal-") })
    }

    func testFallbackAndSourcePreservation() async throws {
        let (root, bundle) = try fixture()
        let image = root.appendingPathComponent("source.dmg")
        try Data("source".utf8).write(to: image)
        let fake = FakeMounts(bundle: bundle, mode: .fallback)
        let installer = D3DMetalInstaller(processRunner: fake.run, requireAppleSignature: false)
        _ = try await installer.installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }
        XCTAssertTrue(fake.calls.contains { $0.first == "convert" && $0.contains("UDTO") })
        XCTAssertEqual(try Data(contentsOf: image), Data("source".utf8))
        XCTAssertEqual(fake.detachments, ["/dev/fake1"])
    }

    func testReverseDetachAfterVerificationFailure() async throws {
        let (root, bundle) = try fixture()
        let image = root.appendingPathComponent("source.dmg")
        try Data().write(to: image)
        let fake = FakeMounts(bundle: bundle, mode: .nested)
        let installer = D3DMetalInstaller(processRunner: fake.run, signatureVerifier: { _ in throw CocoaError(.fileReadCorruptFile) })
        do { _ = try await installer.installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }; XCTFail("Expected verification failure") }
        catch let error as D3DMetalInstallerError { guard case .verificationFailed = error else { return XCTFail("\(error)") } }
        XCTAssertEqual(fake.detachments, ["/dev/fake2", "/dev/fake1"])
    }

    func testAmbiguousFrameworksAndImages() async throws {
        for mode in [FakeMounts.Mode.frameworks, .images] {
            let (root, bundle) = try fixture()
            let image = root.appendingPathComponent("source.dmg")
            try Data().write(to: image)
            let fake = FakeMounts(bundle: bundle, mode: mode)
            do { _ = try await D3DMetalInstaller(processRunner: fake.run, requireAppleSignature: false).installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }; XCTFail("Expected ambiguity") }
            catch let error as D3DMetalInstallerError { guard case .ambiguousCandidate = error else { return XCTFail("\(error)") } }
            XCTAssertEqual(fake.detachments, ["/dev/fake1"])
        }
    }

    func testUnmountedWholeDeviceIsDetached() async throws {
        let (root, bundle) = try fixture()
        let image = root.appendingPathComponent("source.dmg")
        try Data().write(to: image)
        let fake = FakeMounts(bundle: bundle, mode: .unmounted)
        do {
            _ = try await D3DMetalInstaller(processRunner: fake.run, requireAppleSignature: false).installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }
            XCTFail("Expected mount failure")
        } catch let error as D3DMetalInstallerError {
            guard case .mountFailed = error else { return XCTFail("\(error)") }
        }
        XCTAssertEqual(fake.detachments, ["/dev/fake1"])
    }

    func testMalformedUnmountedExternalImagePreservesExistingAttachment() async throws {
        let (root, bundle) = try fixture()
        let image = root.appendingPathComponent("source.dmg")
        try Data().write(to: image)
        let fake = FakeMounts(bundle: bundle, mode: .malformedUnmounted, existingImage: image.path)
        do {
            _ = try await D3DMetalInstaller(processRunner: fake.run, requireAppleSignature: false).installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }
            XCTFail("Expected failure")
        } catch let error as D3DMetalInstallerError {
            guard case .mountFailed = error else { return XCTFail("\(error)") }
        }
        XCTAssertEqual(fake.detachments, ["/dev/fake2"])
    }

    func testMalformedAttachIsReconciled() async throws {
        let (root, bundle) = try fixture()
        let image = root.appendingPathComponent("source.dmg")
        try Data().write(to: image)
        let fake = FakeMounts(bundle: bundle, mode: .malformed)
        do { _ = try await D3DMetalInstaller(processRunner: fake.run, requireAppleSignature: false).installFramework(from: image, into: root.appendingPathComponent("installed")) { _ in }; XCTFail("Expected failure") }
        catch let error as D3DMetalInstallerError { guard case .mountFailed = error else { return XCTFail("\(error)") } }
        XCTAssertEqual(fake.detachments, ["/dev/fake1"])
        XCTAssertFalse(FileManager.default.fileExists(atPath: try XCTUnwrap(fake.jobPath)))
    }

}


private final class FakeMounts: @unchecked Sendable {
    enum Mode { case fallback, nested, frameworks, images, malformed, unmounted, malformedUnmounted }
    private let lock = NSLock()
    private let bundle: URL
    private let mode: Mode
    private var recorded: [[String]] = []
    private var mounts: [(String, String)] = []
    private var imagePaths: [String: String] = [:]
    private var attempts = 0
    private var rootPath: String?
    init(bundle: URL, mode: Mode, existingImage: String? = nil) {
        self.bundle = bundle
        self.mode = mode
        if let existingImage {
            mounts.append(("/dev/fake1", "/Volumes/user"))
            imagePaths["/dev/fake1"] = existingImage
        }
    }
    var calls: [[String]] { lock.withLock { recorded } }
    var detachments: [String] { calls.filter { $0.first == "detach" }.compactMap { $0.last } }
    var jobPath: String? { lock.withLock { rootPath } }
    func run(_ executable: URL, _ args: [String], _ input: Data?) throws -> (exitCode: Int32, stdout: Data, stderr: Data) {
        try lock.withLock {
            recorded.append(args)
            func plist(_ value: Any) throws -> Data { try PropertyListSerialization.data(fromPropertyList: value, format: .xml, options: 0) }
            if args.first == "info" {
                let entries: [[String: Any]] = mounts.map { device, mount in
                    var entity = ["dev-entry": device]
                    if !mount.isEmpty { entity["mount-point"] = mount }
                    return ["image-path": imagePaths[device] ?? "", "system-entities": [entity]]
                }
                return (0, try plist(["images": entries]), Data())
            }
            if args.first == "detach" { mounts.removeAll { $0.0 == args.last }; return (0, Data(), Data()) }
            if args.first == "convert" { return (0, Data(), Data()) }
            guard args.first == "attach", let index = args.firstIndex(of: "-mountrandom") else { return (1, Data(), Data()) }
            attempts += 1
            if mode == .fallback && attempts == 1 { return (1, Data(), Data()) }
            let root = URL(fileURLWithPath: args[index + 1])
            rootPath = root.deletingLastPathComponent().path
            let mount = root.appendingPathComponent("volume")
            try FileManager.default.createDirectory(at: mount, withIntermediateDirectories: true)
            let device = "/dev/fake\(mounts.count + 1)"
            mounts.append((device, mode == .malformedUnmounted ? "" : mount.path))
            imagePaths[device] = args.last
            if mode == .malformedUnmounted { return (0, Data("bad".utf8), Data()) }
            if mode == .unmounted { return (0, try plist(["system-entities": [["dev-entry": device]]]), Data()) }
            if mode == .nested && mounts.count == 1 || mode == .images {
                try Data().write(to: mount.appendingPathComponent("one.dmg"))
                if mode == .images { try Data().write(to: mount.appendingPathComponent("two.dmg")) }
            } else if mode != .malformed {
                try FileManager.default.copyItem(at: bundle, to: mount.appendingPathComponent("D3DMetal.framework"))
                if mode == .frameworks {
                    let other = mount.appendingPathComponent("other")
                    try FileManager.default.createDirectory(at: other, withIntermediateDirectories: true)
                    try FileManager.default.copyItem(at: bundle, to: other.appendingPathComponent("D3DMetal.framework"))
                }
            }
            if mode == .malformed { return (0, Data("bad".utf8), Data()) }
            return (0, try plist(["system-entities": [["dev-entry": device, "mount-point": mount.path]]]), Data())
        }
    }
}
