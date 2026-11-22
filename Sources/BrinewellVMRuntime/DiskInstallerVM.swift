import BrinewellCore
import CKrun
import Darwin
import Foundation

/// A short-lived, headless helper for work on an offline SteamOS disk.
/// Guest poweroff terminates this process; it must never run in the launcher.
@MainActor
public final class DiskInstallerVM {
    private enum Operation {
        case recovery(source: String, guid: String)
    }
    private let operation: Operation
    private let target: String
    private let resources: URL
    private let runDirectory: URL
    private var descriptors: [Int32] = []
    private var reader: DispatchSourceRead?
    private var line: [UInt8] = []

    public init(arguments: [String]) throws {
        let accepted: Set<String> = ["--prepare-steamos", "--disk", "--disk-guid", "--resources", "--run-directory"]
        var values: [String: String] = [:]
        var index = 0
        while index < arguments.count {
            let key = arguments[index]
            guard accepted.contains(key), values[key] == nil, index + 1 < arguments.count else {
                throw RuntimeError("invalid installer option: \(key)")
            }
            values[key] = arguments[index + 1]
            index += 2
        }
        guard let target = values["--disk"], target.hasPrefix("/"),
              let resourcePath = values["--resources"], resourcePath.hasPrefix("/"),
              let runPath = values["--run-directory"], runPath.hasPrefix("/") else {
            throw RuntimeError("missing installer options")
        }
        guard let source = values["--prepare-steamos"], source.hasPrefix("/"),
              let guid = values["--disk-guid"], UUID(uuidString: guid) != nil else {
            throw RuntimeError("invalid disk operation")
        }
        operation = .recovery(source: source, guid: guid.lowercased())
        self.target = target
        resources = URL(fileURLWithPath: resourcePath, isDirectory: true)
        runDirectory = URL(fileURLWithPath: runPath, isDirectory: true)
    }

    public func run() throws -> Never {
        do {
            var directoryStat = stat()
            guard lstat(runDirectory.path, &directoryStat) == 0,
                  directoryStat.st_mode & S_IFMT == S_IFDIR,
                  directoryStat.st_uid == getuid(), directoryStat.st_mode & 0o777 == 0o700 else {
                throw RuntimeError("installer run directory must be owner-only")
            }
            for name in ["Image", "initramfs.cpio.gz"] {
                let url = resources.appendingPathComponent(name)
                guard (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true,
                      FileManager.default.isReadableFile(atPath: url.path) else {
                    throw RuntimeError("missing installer resource: \(name)")
                }
            }
            let writable = true
            let targetFD = try openFile(target, flags: (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW)
            guard flock(targetFD, LOCK_EX | LOCK_NB) == 0 else {
                throw RuntimeError(String(localized: "The SteamOS disk is in use. Shut down the virtual machine first."))
            }
            guard diskFDMatchesPath(targetFD, path: target) else {
                throw RuntimeError("disk was replaced while it was being opened; try again")
            }
            let cmdline: String
            let supportingDisk: String
            switch operation {
            case .recovery(let source, let guid):
                try SteamOSDisk.validateInstaller(at: URL(fileURLWithPath: source))
                let sourceFD = try openFile(source, flags: O_RDONLY | O_NOFOLLOW)
                var targetStat = stat(), sourceStat = stat()
                guard fstat(targetFD, &targetStat) == 0, fstat(sourceFD, &sourceStat) == 0,
                      targetStat.st_mode & S_IFMT == S_IFREG, sourceStat.st_mode & S_IFMT == S_IFREG,
                      targetStat.st_size >= 64 * 1024 * 1024 * 1024,
                      targetStat.st_dev != sourceStat.st_dev || targetStat.st_ino != sourceStat.st_ino else {
                    throw RuntimeError("installer and target must be distinct regular disk images")
                }
                supportingDisk = source
                cmdline = "brinewell.recovery=1 brinewell.target_guid=\(guid)"
            }
            let console = try openFile(runDirectory.appendingPathComponent("console.log").path,
                                       flags: O_WRONLY | O_CREAT | O_EXCL)
            let null = try openFile("/dev/null", flags: O_RDONLY)
            let output = try makePipe()
            let input = try makePipe()
            let context = UInt32(try check(krun_create_ctx(), "create installer VM"))
            do {
                try check(krun_set_vm_config(context, 2, 2048), "configure installer VM")
                try check(krun_disable_implicit_vsock(context), "disable vsock")
                try check(krun_disable_implicit_console(context), "configure console")
                let device = UInt32(try check(krun_add_virtio_console_multiport(context), "add console"))
                try check(krun_add_console_port_inout(context, device, "", null, console), "add system console")
                try check(krun_add_console_port_inout(context, device, "fx.progress", input[0], output[1]), "add progress port")
                let kernel = resources.appendingPathComponent("Image").path
                let initrd = resources.appendingPathComponent("initramfs.cpio.gz").path
                try check(krun_set_kernel(context, kernel, 0, initrd, "console=hvc0 loglevel=4 \(cmdline)"), "set installer kernel")
                try check(krun_add_disk2(context, "vda", target, 0, !writable), "attach target disk")
                try check(krun_add_disk2(context, "vdb", supportingDisk, 0, true), "attach supporting disk read-only")
            } catch {
                _ = krun_free_ctx(context)
                throw error
            }
            let flags = fcntl(output[0], F_GETFL)
            guard flags >= 0, fcntl(output[0], F_SETFL, flags | O_NONBLOCK) == 0 else {
                throw RuntimeError("cannot read installer progress")
            }
            let reader = DispatchSource.makeReadSource(fileDescriptor: output[0], queue: .main)
            reader.setEventHandler { [self] in
                var buffer = [UInt8](repeating: 0, count: 4096)
                let count = Darwin.read(output[0], &buffer, buffer.count)
                guard count > 0 else { return }
                for byte in buffer.prefix(count) {
                    if byte == 10 {
                        let value = String(decoding: line, as: UTF8.self)
                        line.removeAll(keepingCapacity: true)
                        guard value.hasPrefix("provision ") else { continue }
                        let status = String(value.dropFirst("provision ".count))
                        report("BRINEWELL prepare \(status)")
                        if status == "done" {
                            let acknowledgement = Array("prepared\n".utf8)
                            _ = acknowledgement.withUnsafeBytes { Darwin.write(input[1], $0.baseAddress, $0.count) }
                        }
                    } else if line.count < 65536 { line.append(byte) }
                    else { report("BRINEWELL error invalid installer progress"); Darwin.exit(1) }
                }
            }
            self.reader = reader
            reader.resume()
            let thread = Thread {
                let result = krun_start_enter(context)
                report("BRINEWELL error installer VM exited unexpectedly: \(result)")
                Darwin.exit(1)
            }
            thread.name = "brinewell-disk-installer"
            thread.stackSize = 16 << 20
            thread.start()
            dispatchMain()
        } catch {
            for fd in descriptors { Darwin.close(fd) }
            descriptors.removeAll()
            throw error
        }
    }

    private func openFile(_ path: String, flags: Int32) throws -> Int32 {
        let fd = Darwin.open(path, flags | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw RuntimeError("cannot open \(path): \(String(cString: strerror(errno)))") }
        descriptors.append(fd)
        return fd
    }

    private func makePipe() throws -> [Int32] {
        var pair: [Int32] = [-1, -1]
        guard pipe(&pair) == 0 else { throw RuntimeError("cannot create installer pipe") }
        descriptors += pair
        for fd in pair { _ = fcntl(fd, F_SETFD, FD_CLOEXEC) }
        return pair
    }

    @discardableResult
    private func check(_ result: Int32, _ operation: String) throws -> Int32 {
        guard result >= 0 else { throw RuntimeError("cannot \(operation): \(result)") }
        return result
    }
}
