import Darwin
import Foundation
import XCTest
@testable import BrinewellCore

final class VMNetworkConfigurationTests: XCTestCase {
    private let host = HostCapabilities(logicalCPUCount: 4, physicalMemoryBytes: 8 * 1_073_741_824,
                                        architecture: "test", metalDeviceName: nil)

    func testLegacyConfigurationDefaultsToDirectNetworking() throws {
        let configuration = try JSONDecoder().decode(VMConfiguration.self, from: Data(#"{"cpuCount":2,"memoryGiB":4}"#.utf8))
        XCTAssertEqual(configuration.network, VMNetworkConfiguration())
    }

    func testSupportedProxyAddresses() {
        for address in ["socks5://127.0.0.1:7890", "http://proxy.example:8080", "https://proxy.example:443",
                        "socks5://[::1]:1080", "http://test-user:test-password@localhost:8080"] {
            XCTAssertNoThrow(try VMNetworkConfiguration(proxyEnabled: true, proxyURL: address).validate())
        }
    }

    func testInvalidEnabledProxyAddressesAreRejectedWithoutEchoingAddress() {
        for address in ["", "127.0.0.1:7890", "ftp://localhost:21", "http://localhost", "http://:8080",
                        "socks5://localhost:0", "socks5://localhost:65536", "http://localhost:port",
                        "http://localhost:8080/", "http://localhost:8080/path", "http://localhost:8080?query",
                        "http://localhost:8080#fragment", "http://local host:8080",
                        "http://test-user:private-test-password@localhost"] {
            XCTAssertThrowsError(try VMNetworkConfiguration(proxyEnabled: true, proxyURL: address).validate()) { error in
                XCTAssertFalse(error.localizedDescription.contains("private-test-password"))
            }
        }
    }

    func testDraftsCanBeSavedButEnabledInvalidProxyCannotStart() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
        var configuration = VMConfiguration(cpuCount: 2, memoryGiB: 4,
                                            network: VMNetworkConfiguration(proxyEnabled: true, proxyURL: "unfinished"))
        try store.save(configuration, for: host)
        XCTAssertEqual(try store.load(for: host), configuration)
        XCTAssertThrowsError(try configuration.validate(for: host))
        configuration.network.proxyEnabled = false
        try store.save(configuration, for: host)
        XCTAssertEqual(try store.load(for: host).network.proxyURL, "unfinished")
        XCTAssertNoThrow(try configuration.validate(for: host))
        let attributes = try FileManager.default.attributesOfItem(atPath: store.url.path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o600)
    }

    func testRuntimeFileIsPrivateAndDoesNotReplaceExistingFile() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("network.json")
        let configuration = VMNetworkConfiguration(proxyEnabled: true, proxyURL: "socks5://localhost:1080")
        try configuration.write(to: url)
        XCTAssertEqual(try JSONDecoder().decode(VMNetworkConfiguration.self, from: Data(contentsOf: url)), configuration)
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o600)
        XCTAssertThrowsError(try VMNetworkConfiguration().write(to: url))
        XCTAssertEqual(try JSONDecoder().decode(VMNetworkConfiguration.self, from: Data(contentsOf: url)), configuration)
    }
}
