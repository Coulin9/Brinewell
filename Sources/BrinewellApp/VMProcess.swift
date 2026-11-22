import AppKit
import BrinewellCore
import BrinewellVMRuntime
import Darwin
import Foundation

@MainActor
final class VMProcess: ObservableObject {
    enum State {
        case idle, starting, running, stopping, failed

        var title: String {
            switch self {
            case .idle: String(localized: "Idle")
            case .starting: String(localized: "Starting")
            case .running: String(localized: "Running")
            case .stopping: String(localized: "Stopping")
            case .failed: String(localized: "Failed")
            }
        }
    }
    @Published private(set) var state: State = .idle
    @Published private(set) var error: String?
    @Published private(set) var workContextWarning: String?
    @Published private(set) var uu = UUPluginStatus()
    @Published private(set) var graphicsApplied: GraphicsBackend?
    @Published private(set) var graphicsError: String?
    @Published private(set) var runDirectory: URL?
    @Published private(set) var hostDisplayID: UInt32?
    var busy: Bool { state == .starting || state == .running || state == .stopping }
    var logPath: String? { runDirectory?.appendingPathComponent("host.log").path }
    private var runtime: Runtime?
    private var graphicsRequested: GraphicsBackend?
    private var graphicsTimeout: Task<Void, Never>?
    private var runtimeLog: RuntimeLog?
    private var session = UUID()
    private var requestedStop = false
    private var fatalErrorReceived = false
    private var wasReady = false
    var onExit: (() -> Void)?
    var onSettled: (() -> Void)?

    func start(configuration: VMConfiguration, host: HostCapabilities, display: HostDisplayCapabilities?,
               shrinkHomeKiB: UInt64? = nil, willLaunch: (() -> Void)? = nil) {
        guard !busy, runtime == nil else { return }
        workContextWarning = nil
        uu = UUPluginStatus()
        resetGraphics()
        do {
            try configuration.validate(for: host)
            guard let display, let currentDisplay = HostDisplayCapabilities.detect(displayID: display.id) else {
                throw Failure(String(localized: "Could not read the available display modes for the host display."))
            }
            try currentDisplay.validate(configuration.display)
            guard let disk = configuration.diskImagePath, FileManager.default.isWritableFile(atPath: disk) else {
                throw Failure(String(localized: "Choose an existing writable disk image."))
            }
            let contents = Bundle.main.bundleURL.appendingPathComponent("Contents")
            let resources = contents.appendingPathComponent("Resources/Runtime")
            for name in ["Image", "initramfs.cpio.gz", "brinewell-layer.img", "gvproxy"] {
                let path = resources.appendingPathComponent(name).path
                guard FileManager.default.isReadableFile(atPath: path), name != "gvproxy" || FileManager.default.isExecutableFile(atPath: path) else {
                    throw Failure(String(localized: "Runtime resource unavailable: \(path)"))
                }
            }
            let renderer = contents.appendingPathComponent("Helpers/virgl_render_server")
            let driver = contents.appendingPathComponent("Frameworks/libvulkan_kosmickrisp.dylib")
            guard FileManager.default.isExecutableFile(atPath: renderer.path),
                  (try? renderer.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
                throw Failure(String(localized: "Runtime renderer unavailable: \(renderer.path)"))
            }
            guard FileManager.default.isReadableFile(atPath: driver.path),
                  (try? driver.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
                throw Failure(String(localized: "Runtime Vulkan driver unavailable: \(driver.path)"))
            }
            let directory = URL(fileURLWithPath: "/tmp/brinewell-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            runDirectory = directory
            let networkConfig = directory.appendingPathComponent("network.json")
            try configuration.network.write(to: networkConfig)
            let log = directory.appendingPathComponent("host.log")
            runtimeLog = try RuntimeLog(url: log)

            // Build the same CLI argument vector the standalone VM used, so
            // RuntimeOptions (disk FD, neptune probe, etc.) behaves identically.
            var arguments = [
                "--resources", resources.path, "--disk", disk,
                "--cpus", "\(configuration.cpuCount)", "--memory-gib", "\(configuration.memoryGiB)",
                "--display-width", "\(configuration.display.width)",
                "--display-height", "\(configuration.display.height)",
                "--display-refresh-rate", "\(configuration.display.refreshRate)",
                "--host-display-id", "\(currentDisplay.id)",
                "--run-directory", directory.path,
                "--network-config", networkConfig.path,
                "--graphics-backend", configuration.graphicsBackend.rawValue,
                "--neptune-aperture-mib", "\(configuration.neptuneApertureMiB)",
                "--neptune-multi-ring", configuration.neptuneMultiRing ? "1" : "0",
                "--game-hud-level", String(configuration.gameHUDLevel.rawValue),
                "--game-hud-position", configuration.gameHUDPosition.rawValue,
            ]
            if let shrinkHomeKiB {
                arguments += ["--shrink-home-kib", "\(shrinkHomeKiB)"]
            }
            if let framework = configuration.d3dMetalFrameworkPath {
                arguments += ["--d3dmetal-framework", framework]
            }
            if let folder = configuration.sharedFolder {
                arguments += ["--shared-folder", folder.path, "--shared-folder-read-only", folder.readOnly ? "1" : "0"]
            }
            let options = try RuntimeOptions(arguments: arguments)
            let token = UUID()
            session = token
            let rt: Runtime
            do { rt = try Runtime(options: options) }
            catch { close(options.diskFD); throw error }
            // Single-process mode: route state lines and VM exit back to this
            // object on the main actor while retaining the host log output.
            reportHandler = { [weak self] line in
                Task { @MainActor in self?.receive(line, session: token) }
            }
            rt.onExit = { [weak self] result in
                Task { @MainActor in self?.vmExited(result, session: token) }
            }
            rt.onGraphicsApplied = { [weak self] mode in
                guard let self, self.session == token else { return }
                self.graphicsApplied = mode
                self.graphicsError = nil
                if mode == self.graphicsRequested {
                    self.graphicsTimeout?.cancel()
                    self.graphicsTimeout = nil
                }
            }
            runtime = rt
            requestedStop = false
            fatalErrorReceived = false
            wasReady = false
            error = nil
            willLaunch?()
            state = .starting
            Task { @MainActor [self] in
                do { try await rt.start() }
                catch {
                    guard session == token else { return }
                    if requestedStop || error is CancellationError {
                        vmExited(0, session: token)
                    } else {
                        startFailed(error)
                    }
                }
            }
        } catch {
            startFailed(error)
        }
    }

    func setGameHUD(level: GameHUDLevel, position: GameHUDPosition) {
        runtime?.setGameHUD(level: level, position: position)
    }

    func installUU() {
        guard uu.canInstall, let runtime, !requestedStop else { return }
        do {
            try runtime.installUU()
            uu.beginInstallation()
        } catch { uu.fail((error as? LocalizedError)?.errorDescription ?? String(describing: error)) }
    }

    func refreshUUStatus() {
        guard uu.canRefresh, let runtime, !requestedStop else { return }
        do { try runtime.refreshUUStatus() }
        catch { uu.fail((error as? LocalizedError)?.errorDescription ?? String(describing: error)) }
    }

    /// Live backend switch applied on the next game launch. The runtime
    /// rejects a backend that failed startup validation.
    @discardableResult
    func setGraphicsBackend(_ backend: GraphicsBackend) -> Bool {
        guard state == .running, let runtime else { return false }
        do {
            try runtime.setGraphicsBackend(backend)
            graphicsError = nil
            graphicsRequested = backend
            graphicsTimeout?.cancel()
            graphicsTimeout = Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 5_000_000_000)
                guard let self, !Task.isCancelled, self.state == .running,
                      self.graphicsRequested == backend,
                      self.graphicsApplied != backend else { return }
                self.graphicsError = String(localized: "The guest did not confirm the graphics backend change.")
            }
            return true
        } catch {
            graphicsError = error.localizedDescription
            return false
        }
    }

    private func startFailed(_ failure: Error) {
        let message = (failure as? LocalizedError)?.errorDescription ?? String(describing: failure)
        report("BRINEWELL error \(message)")
        error = message
        state = .failed
        runtime = nil
        workContextWarning = nil
        uu = UUPluginStatus()
        resetGraphics()
        reportHandler = nil
        runtimeLog = nil
        onSettled?()
    }

    private func resetGraphics() {
        graphicsTimeout?.cancel()
        graphicsTimeout = nil
        graphicsRequested = nil
        graphicsApplied = nil
        graphicsError = nil
    }

    private func receive(_ line: String, session token: UUID) {
        guard token == session, runtime != nil else { return }
        switch line {
        case "BRINEWELL state booting": break
        case "BRINEWELL state running":
            if !requestedStop && !fatalErrorReceived {
                wasReady = true
                state = .running
            }
        case "BRINEWELL state stopping":
            state = .stopping
        default:
            if line.hasPrefix("BRINEWELL uu ") {
                let event = String(line.dropFirst("BRINEWELL uu ".count))
                if event.hasPrefix("qrcode "), event.utf8.count <= 24_000, uu.phase == .installed,
                   let data = Data(base64Encoded: String(event.dropFirst("qrcode ".count))),
                   let identifier = UUQRCode.bindingCode(fromTerminalData: data) {
                    uu.receive("binding \(identifier)")
                } else { uu.receive(event) }
            }
            if line.hasPrefix("BRINEWELL work-context unavailable") {
                workContextWarning = WorkContextNotice.warning(for: line)
            } else if line.hasPrefix("BRINEWELL work-context pending") || line.hasPrefix("BRINEWELL work-context ready") {
                // This describes our render path, not the system's Game Mode state.
                workContextWarning = nil
            }
            if line.hasPrefix("BRINEWELL display screen "),
               let id = UInt32(line.dropFirst("BRINEWELL display screen ".count)) {
                hostDisplayID = id
            }
            if line.hasPrefix("BRINEWELL error ") {
                error = String(line.dropFirst("BRINEWELL error ".count))
                fatalErrorReceived = true
                state = .stopping
            }
        }
    }

    private func vmExited(_ result: Int32, session token: UUID) {
        guard token == session else { return }
        let normal = result == 0
        runtime = nil
        workContextWarning = nil
        uu = UUPluginStatus()
        resetGraphics()
        reportHandler = nil
        runtimeLog = nil
        if fatalErrorReceived || !normal || (!wasReady && !requestedStop) {
            state = .failed
            if error == nil { error = String(localized: "The virtual machine process exited (status \(result)).") }
        } else { state = .idle }
        onSettled?()
        onExit?()
    }

    func stop() {
        guard let runtime, !requestedStop else { return }
        requestedStop = true
        state = .stopping
        runtime.shutdown()
    }

    func forceStop() {
        guard runtime != nil else { return }
        requestedStop = true
        state = .stopping
        // In-process: no separate process to SIGKILL. Tear down the runtime;
        // the guest shutdown key is best-effort, then we drop the reference so
        // the VMM thread is left to unwind (it cannot exit the launcher).
        runtime?.shutdown()
    }
}

private struct Failure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}
