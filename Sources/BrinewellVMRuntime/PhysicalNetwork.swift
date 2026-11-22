import Darwin
import Foundation
import SystemConfiguration

struct PhysicalNetwork {
    let interface: String
    let dnsServers: [String]

    struct Service {
        let id: String
        let interface: String
        let type: String
        let enabled: Bool
        let configuredDNS: [String]
        let automaticDNS: [String]
    }

    static func detect() throws -> PhysicalNetwork {
        guard let preferences = SCPreferencesCreate(nil, "Brinewell" as CFString, nil),
              let set = SCNetworkSetCopyCurrent(preferences),
              let services = SCNetworkSetCopyServices(set) as? [SCNetworkService],
              let store = SCDynamicStoreCreate(nil, "Brinewell" as CFString, nil, nil) else {
            throw DetectionError(String(localized: "Could not read the Mac’s physical network configuration."))
        }
        let order = SCNetworkSetGetServiceOrder(set) as? [String] ?? []
        let candidates = services.compactMap { service -> Service? in
            guard let networkInterface = SCNetworkServiceGetInterface(service),
                  let name = SCNetworkInterfaceGetBSDName(networkInterface) as String?,
                  let type = SCNetworkInterfaceGetInterfaceType(networkInterface) as String?,
                  let id = SCNetworkServiceGetServiceID(service) as String? else { return nil }
            let stateKey = SCDynamicStoreKeyCreateNetworkServiceEntity(
                nil, kSCDynamicStoreDomainState, id as CFString, kSCEntNetDNS
            )
            let state = SCDynamicStoreCopyValue(store, stateKey) as? [String: Any]
            let dnsProtocol = SCNetworkServiceCopyProtocol(service, kSCNetworkProtocolTypeDNS)
            let configuration = dnsProtocol.flatMap { SCNetworkProtocolGetConfiguration($0) as? [String: Any] }
            return Service(
                id: id, interface: name, type: type, enabled: SCNetworkServiceGetEnabled(service),
                configuredDNS: configuration?[kSCPropNetDNSServerAddresses as String] as? [String] ?? [],
                automaticDNS: state?[kSCPropNetDNSServerAddresses as String] as? [String] ?? []
            )
        }
        return try select(services: candidates, order: order, activeInterfaces: activeInterfaces())
    }

    static func select(services: [Service], order: [String], activeInterfaces: Set<String>) throws -> PhysicalNetwork {
        // A TUN can own the default route. Use the user's physical service order,
        // and verify live addresses instead of trusting cached service state.
        let physicalServices = services.filter { service in
            let isPhysicalType = service.type == kSCNetworkInterfaceTypeEthernet as String
                || service.type == kSCNetworkInterfaceTypeIEEE80211 as String
            let suffix = service.interface.dropFirst(2)
            let isPhysicalName = service.interface.hasPrefix("en")
                && !suffix.isEmpty && suffix.allSatisfy(\.isNumber)
            return service.enabled && isPhysicalType && isPhysicalName
                && activeInterfaces.contains(service.interface)
        }.sorted { first, second in
            let firstRank = order.firstIndex(of: first.id) ?? Int.max
            let secondRank = order.firstIndex(of: second.id) ?? Int.max
            return firstRank == secondRank ? first.id < second.id : firstRank < secondRank
        }
        guard let service = physicalServices.first else {
            throw DetectionError(String(localized: "No usable physical network interface was found. Connect to Wi-Fi or Ethernet and try again."))
        }
        // Explicit service DNS overrides DHCP. Never consult global DNS, which
        // may belong to the host's VPN/TUN, or silently choose a public resolver.
        let servers = service.configuredDNS.isEmpty ? service.automaticDNS : service.configuredDNS
        var dnsServers: [String] = []
        for server in servers {
            guard let address = dnsAddress(server, interface: service.interface),
                  !dnsServers.contains(address) else { continue }
            dnsServers.append(address)
        }
        return PhysicalNetwork(interface: service.interface, dnsServers: dnsServers)
    }

    private static func activeInterfaces() throws -> Set<String> {
        var addresses: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&addresses) == 0 else {
            throw DetectionError(String(localized: "Could not read the Mac’s network interfaces: \(String(cString: strerror(errno)))"))
        }
        defer { freeifaddrs(addresses) }
        var active: Set<String> = []
        var next = addresses
        while let current = next {
            defer { next = current.pointee.ifa_next }
            let entry = current.pointee
            let requiredFlags = UInt32(IFF_UP | IFF_RUNNING)
            guard entry.ifa_flags & requiredFlags == requiredFlags,
                  entry.ifa_flags & UInt32(IFF_LOOPBACK) == 0,
                  let address = entry.ifa_addr else { continue }
            let family = Int32(address.pointee.sa_family)
            guard family == AF_INET || family == AF_INET6 else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(address, socklen_t(address.pointee.sa_len), &host,
                              socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 else { continue }
            let numericAddress = String(decoding: host.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
            guard isRoutableAddress(numericAddress) else { continue }
            active.insert(String(cString: entry.ifa_name))
        }
        return active
    }

    static func isRoutableAddress(_ address: String) -> Bool {
        guard let bytes = addressBytes(address) else { return false }
        if bytes.count == 4 {
            return bytes[0] != 0 && bytes[0] != 127 && bytes[0] < 224
                && !(bytes[0] == 169 && bytes[1] == 254)
        }
        return !bytes.allSatisfy { $0 == 0 }
            && !(bytes.dropLast().allSatisfy { $0 == 0 } && bytes.last == 1)
            && bytes[0] != 0xff && !(bytes[0] == 0xfe && bytes[1] & 0xc0 == 0x80)
    }

    private static func dnsAddress(_ address: String, interface: String) -> String? {
        guard let bytes = addressBytes(address) else { return nil }
        if bytes.count == 16 && bytes[0] == 0xfe && bytes[1] & 0xc0 == 0x80 {
            // DHCP can publish an unscoped link-local IPv6 resolver.
            return address.split(separator: "%")[0] + "%" + interface
        }
        return isRoutableAddress(address) ? address : nil
    }

    private static func addressBytes(_ address: String) -> [UInt8]? {
        guard let hostPart = address.split(separator: "%", maxSplits: 1).first else { return nil }
        let host = String(hostPart)
        var ipv4 = in_addr()
        if inet_pton(AF_INET, host, &ipv4) == 1 {
            return withUnsafeBytes(of: ipv4) { Array($0) }
        }
        var ipv6 = in6_addr()
        if inet_pton(AF_INET6, host, &ipv6) == 1 {
            return withUnsafeBytes(of: ipv6) { Array($0) }
        }
        return nil
    }

    private struct DetectionError: LocalizedError, CustomStringConvertible {
        let description: String
        init(_ description: String) { self.description = description }
        var errorDescription: String? { description }
    }
}
