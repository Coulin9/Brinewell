import BrinewellCore
import CKrun
import Darwin
import Foundation

final class RuntimeNetwork {
    let socket: String
    let apiSocketPath: String
    private(set) var sshPort: Int = 0
    private let directory: URL
    private var process: Process?
    private var lifeline: Pipe?

    init(directory: URL) throws {
        self.directory = directory
        socket = directory.appendingPathComponent("net.sock").path
        apiSocketPath = directory.appendingPathComponent("api.sock").path
        // sockaddr_un.sun_path includes a terminating NUL.
        guard socket.utf8.count < 104, apiSocketPath.utf8.count < 104 else {
            throw RuntimeError("run directory is too long for network sockets")
        }
    }

    func start(binary: String, sshPort: Int, configuration: VMNetworkConfiguration) throws {
        self.sshPort = sshPort
        try configuration.validate()
        let physical = try PhysicalNetwork.detect()
        guard configuration.proxyEnabled || !physical.dnsServers.isEmpty else {
            throw RuntimeError(String(localized: "The selected physical interface has no usable DNS configuration. Check the Mac’s network settings and try again."))
        }
        let outbound = OutboundConfiguration(
            proxyEnabled: configuration.proxyEnabled,
            proxyURL: configuration.proxyEnabled ? configuration.proxyURL : "",
            interface: physical.interface,
            dnsServers: configuration.proxyEnabled ? ["1.1.1.1", "223.5.5.5"] : physical.dnsServers,
            bootstrapDNSServers: physical.dnsServers
        )
        let outboundURL = directory.appendingPathComponent("outbound.json")
        guard FileManager.default.createFile(atPath: outboundURL.path,
                                            contents: try JSONEncoder().encode(outbound),
                                            attributes: [.posixPermissions: 0o600]) else {
            throw RuntimeError(String(localized: "Could not write the network configuration."))
        }
        let mode = configuration.proxyEnabled ? "proxy" : "direct"
        report("BRINEWELL network mode=\(mode) interface=\(physical.interface)")
        let script = """
        exec 3<&0
        "$@" </dev/null >/dev/null 2>&1 3<&- &
        p=$!
        trap 'kill $p 2>/dev/null; wait $p; exit 0' TERM INT HUP
        ( while read -r _ <&3; do :; done; kill $p 2>/dev/null ) &
        wait $p
        """
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", script, "gvproxy", binary,
                       "-listen-vfkit", "unixgram://" + socket,
                       "-listen", "unix://" + apiSocketPath,
                       "-ssh-port", "-1",
                       "-outbound-config", outboundURL.path,
                       "-log-file", directory.appendingPathComponent("gvproxy.log").path,
                       "-pid-file", directory.appendingPathComponent("gvproxy.pid").path]
        let pipe = Pipe()
        guard fcntl(pipe.fileHandleForWriting.fileDescriptor, F_SETFD, FD_CLOEXEC) == 0 else {
            throw RuntimeError("cannot secure gvproxy lifeline: \(String(cString: strerror(errno)))")
        }
        p.standardInput = pipe
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        try p.run()
        try? pipe.fileHandleForReading.close()
        process = p
        lifeline = pipe
        let deadline = Date().addingTimeInterval(10)
        while !FileManager.default.fileExists(atPath: socket) {
            guard p.isRunning else {
                let log = (try? String(contentsOf: directory.appendingPathComponent("gvproxy.log"), encoding: .utf8)) ?? ""
                throw RuntimeError("gvproxy exited (\(p.terminationStatus)): \(log.suffix(1024))")
            }
            guard Date() < deadline else { throw RuntimeError("gvproxy did not create its socket") }
            usleep(20_000)
        }
        guard p.isRunning else {
            throw RuntimeError("gvproxy exited before network startup completed")
        }
    }

    func attach(context: UInt32) throws {
        var mac: [UInt8] = [0x5a, 0x94, 0xef, 0xe4, 0x0c, 0xee]
        try krunCheck("krun_add_net_unixgram", socket.withCString {
            krun_add_net_unixgram(context, $0, -1, &mac, (1 << 0) | (1 << 1) | (1 << 7) | (1 << 10) | (1 << 11) | (1 << 14), 1)
        })
    }

    func stop() {
        if let process {
            if process.isRunning { kill(process.processIdentifier, SIGTERM) }
            process.waitUntilExit()
        }
        try? lifeline?.fileHandleForWriting.close()
        lifeline = nil
        process = nil
    }
}

private struct OutboundConfiguration: Encodable {
    let proxyEnabled: Bool
    let proxyURL: String
    let interface: String
    let dnsServers: [String]
    let bootstrapDNSServers: [String]
}
