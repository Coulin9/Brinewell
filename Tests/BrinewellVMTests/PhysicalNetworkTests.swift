import SystemConfiguration
import XCTest
@testable import BrinewellVMRuntime

final class PhysicalNetworkTests: XCTestCase {
    private func service(_ id: String, _ interface: String, type: String = kSCNetworkInterfaceTypeEthernet as String,
                         enabled: Bool = true, configuredDNS: [String] = [],
                         automaticDNS: [String] = ["192.168.31.1"]) -> PhysicalNetwork.Service {
        .init(id: id, interface: interface, type: type, enabled: enabled,
              configuredDNS: configuredDNS, automaticDNS: automaticDNS)
    }

    func testUsesPhysicalServicePriorityDespiteActiveTUNAndBridge() throws {
        let services = [service("wifi", "en0"), service("wired", "en7"),
                        service("tun", "utun6"), service("bridge", "bridge0")]
        let selected = try PhysicalNetwork.select(
            services: services, order: ["tun", "bridge", "wired", "wifi"],
            activeInterfaces: ["utun6", "bridge0", "en0", "en7"]
        )
        XCTAssertEqual(selected.interface, "en7")
        XCTAssertEqual(selected.dnsServers, ["192.168.31.1"])
    }

    func testSkipsDisconnectedDisabledAndNonphysicalServices() throws {
        let services = [service("disconnected", "en7"), service("disabled", "en8", enabled: false),
                        service("vpn", "en9", type: kSCNetworkInterfaceTypeIPSec as String),
                        service("wifi", "en0", type: kSCNetworkInterfaceTypeIEEE80211 as String)]
        let selected = try PhysicalNetwork.select(
            services: services, order: ["disconnected", "disabled", "vpn", "wifi"],
            activeInterfaces: ["en0", "en8", "en9"]
        )
        XCTAssertEqual(selected.interface, "en0")
    }

    func testMissingServiceOrderIsDeterministic() throws {
        let services = [service("z-wifi", "en0"), service("a-wired", "en7")]
        let selected = try PhysicalNetwork.select(services: services, order: [], activeInterfaces: ["en0", "en7"])
        XCTAssertEqual(selected.interface, "en7")
    }

    func testUsesOnlySelectedServiceDNSAndScopesLinkLocalIPv6() throws {
        let services = [service("wifi", "en0", automaticDNS: ["192.168.31.1", "fe80::1", "192.168.31.1"]),
                        service("tun", "utun6", automaticDNS: ["198.18.0.2"])]
        let selected = try PhysicalNetwork.select(services: services, order: ["tun", "wifi"], activeInterfaces: ["utun6", "en0"])
        XCTAssertEqual(selected.dnsServers, ["192.168.31.1", "fe80::1%en0"])
    }

    func testExplicitDNSOverridesDHCPAndRejectsLocalProxyResolvers() throws {
        let services = [service("wifi", "en0", configuredDNS: ["127.0.0.1", "::1", "0.0.0.0", "invalid", "10.0.0.53"])]
        let selected = try PhysicalNetwork.select(services: services, order: ["wifi"], activeInterfaces: ["en0"])
        XCTAssertEqual(selected.dnsServers, ["10.0.0.53"])
    }

    func testNoDNSDoesNotFallBackToAnotherService() throws {
        let services = [service("wifi", "en0", automaticDNS: []), service("tun", "utun6")]
        let selected = try PhysicalNetwork.select(services: services, order: ["wifi"], activeInterfaces: ["en0", "utun6"])
        XCTAssertEqual(selected.dnsServers, [])
    }

    func testNoPhysicalConnectionReportsActionableError() {
        XCTAssertThrowsError(try PhysicalNetwork.select(services: [service("tun", "utun6")], order: ["tun"], activeInterfaces: ["utun6"])) { error in
            XCTAssertTrue(error.localizedDescription.contains("Wi-Fi"))
        }
    }

    func testOnlyUsableUnicastAddressesMakeInterfaceActive() {
        for address in ["192.168.31.9", "10.0.0.9", "2001:db8::1", "fd00::1"] {
            XCTAssertTrue(PhysicalNetwork.isRoutableAddress(address), address)
        }
        for address in ["0.0.0.0", "127.0.0.1", "169.254.3.9", "224.0.0.1", "::", "::1", "fe80::1%en0", "ff02::1", "invalid"] {
            XCTAssertFalse(PhysicalNetwork.isRoutableAddress(address), address)
        }
    }
}
