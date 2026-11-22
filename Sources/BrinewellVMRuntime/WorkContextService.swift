import Darwin
import Foundation
import ServiceManagement

public enum WorkContextNotice {
    public static func warning(for line: String) -> String? {
        guard line.hasPrefix("BRINEWELL work-context unavailable") else { return nil }
        var message = String(localized: "The renderer cannot share its work context right now. Full screen remains available. To avoid slowdowns, turn off Game Mode in the macOS Game menu or the game overlay settings.")
        if line.contains("reason=background-service-approval") {
            message += String(localized: " To enable sharing, allow the Brinewell background service in Login Items & Extensions in System Settings, then restart the virtual machine.")
        }
        return message
    }
}

/// Introduces the VM and renderer; their work connection is direct.
/// This status describes propagation capability. macOS owns Game Mode.
@MainActor
final class WorkContextService {
    static let name = "org.brinewell.render-context"
    typealias Status = @convention(c) () -> Int32
    typealias Activity = @convention(c) (Bool) -> Void
    typealias Probe = @Sendable @convention(c) (UnsafePointer<CChar>, UInt32) -> Int32
    private var library: UnsafeMutableRawPointer?
    private var status: Status?
    private var updateActivity: Activity?
    private var active = false
    private var monitor: Task<Void, Never>?
    private var lastStatus: Int32?
    private var startupGeneration: UInt64 = 0
    var onReport: ((String) -> Void)?

    func start(renderer: URL, supportsBackend: Bool) async {
        startupGeneration &+= 1
        let generation = startupGeneration
        // Activity can arrive before registration finishes. Replacing the
        // connection must preserve the VM window's latest activity state.
        stopMonitoring()
        guard supportsBackend else {
            emit("BRINEWELL work-context unavailable reason=backend")
            return
        }
        let path = renderer.deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Frameworks/libvirglrenderer.1.dylib").path
        guard let handle = dlopen(path, RTLD_NOW | RTLD_LOCAL) else {
            emit("BRINEWELL work-context unavailable reason=renderer-library")
            return
        }
        // A detached probe can still be executing when stop() invalidates this
        // startup. Keep its code loaded until the awaited call has returned.
        var adopted = false
        defer { if !adopted { dlclose(handle) } }
        guard let symbol = dlsym(handle, "virgl_renderer_get_work_context_status"),
              let activitySymbol = dlsym(handle, "virgl_renderer_set_work_context_active"),
              let probeSymbol = dlsym(handle, "virgl_renderer_probe_work_context_service") else {
            emit("BRINEWELL work-context unavailable reason=renderer-capability")
            return
        }
        let probe = unsafeBitCast(probeSymbol, to: Probe.self)
        let service = SMAppService.agent(plistName: Self.name + ".plist")
        do {
            // Do not re-register a service the user has disabled. An enabled
            // service must be refreshed after an in-place app update so launchd
            // does not keep the previous executable or property list cached.
            try checkStartup(generation)
            let version = try Self.registrationVersion(renderer: renderer)
            let key = Self.name + ".registration-version"
            if service.status == .enabled && UserDefaults.standard.string(forKey: key) != version {
                try await service.unregister()
            }
            try checkStartup(generation)
            if Self.needsInitialRegistration(service.status) { try service.register() }
            guard registrationEnabled(service) else { return }

            var ready = try await waitForBroker(probe)
            try checkStartup(generation)
            if !ready {
                // launchd may invalidate a stale registration after rejecting
                // its executable. Give that asynchronous work a bounded probe
                // phase, then refresh once using the current permission state.
                guard service.status == .enabled || Self.needsInitialRegistration(service.status) else {
                    _ = registrationEnabled(service)
                    return
                }
                emit("BRINEWELL work-context broker-refresh")
                if service.status == .enabled { try await service.unregister() }
                try checkStartup(generation)
                if Self.needsInitialRegistration(service.status) { try service.register() }
                guard registrationEnabled(service) else { return }
                ready = try await waitForBroker(probe)
                try checkStartup(generation)
            }
            guard ready else {
                if service.status == .requiresApproval {
                    _ = registrationEnabled(service)
                } else {
                    emit("BRINEWELL work-context unavailable reason=broker-startup")
                }
                return
            }
            guard registrationEnabled(service) else { return }
            // Registration status alone does not prove the broker can launch.
            UserDefaults.standard.set(version, forKey: key)
            startMonitoring(status: unsafeBitCast(symbol, to: Status.self),
                            updateActivity: unsafeBitCast(activitySymbol, to: Activity.self))
            library = handle
            adopted = true
            setenv("NPT_WORK_CONTEXT_SERVICE", Self.name, 1)
        } catch is CancellationError {
            return
        } catch {
            guard startupGeneration == generation, !Task.isCancelled else { return }
            let reason = service.status == .requiresApproval ? "background-service-approval" : "registration"
            emit("BRINEWELL work-context unavailable reason=\(reason) detail=\(error.localizedDescription)")
            return
        }
    }

    private func registrationEnabled(_ service: SMAppService) -> Bool {
        guard service.status == .enabled else {
            let reason = service.status == .requiresApproval ? "background-service-approval" : "service-unavailable"
            emit("BRINEWELL work-context unavailable reason=\(reason)")
            return false
        }
        return true
    }

    func waitForBroker(_ probe: Probe, timeout: Duration = .seconds(15)) async throws -> Bool {
        let generation = startupGeneration
        let name = Self.name
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            try checkStartup(generation)
            let remaining = clock.now.duration(to: deadline).components
            let milliseconds = remaining.seconds * 1_000 + remaining.attoseconds / 1_000_000_000_000_000
            let probeTimeout = UInt32(max(1, min(1_000, milliseconds)))
            let ready = await Task.detached {
                name.withCString { probe($0, probeTimeout) == 1 }
            }.value
            // Cancellation cannot unload the library under an in-flight C
            // call. Each service reply wait is at most one second; the native
            // probe also drains its local cancellation callbacks before return.
            try checkStartup(generation)
            if ready { return true }
            let remainingWait = clock.now.duration(to: deadline)
            if remainingWait > .zero {
                try await Task.sleep(for: min(.milliseconds(250), remainingWait))
            }
        }
        try checkStartup(generation)
        return false
    }

    private func checkStartup(_ generation: UInt64) throws {
        try Task.checkCancellation()
        if startupGeneration != generation { throw CancellationError() }
    }

    func setActive(_ active: Bool) {
        self.active = active
        // Full-screen completion may leave activity unchanged but still needs
        // a check after macOS has updated its scheduling policies.
        updateActivity?(active)
    }

    func startMonitoring(status: Status, updateActivity: Activity) {
        stopMonitoring()
        self.status = status
        self.updateActivity = updateActivity
        updateActivity(active)
        reportStatus()
        monitor = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .milliseconds(500)) }
                catch { return }
                self?.poll()
            }
        }
    }

    static func needsInitialRegistration(_ status: SMAppService.Status) -> Bool {
        // A correctly packaged but never registered LaunchAgent can be notFound.
        status == .notRegistered || status == .notFound
    }

    static func registrationVersion(renderer: URL) throws -> String {
        let contents = renderer.deletingLastPathComponent().deletingLastPathComponent()
        let files = [renderer,
                     contents.appendingPathComponent("Library/LaunchAgents/\(name).plist"),
                     contents.appendingPathComponent("Frameworks/libvirglrenderer.1.dylib")]
        return try files.map { url in
            // URL resource values may cache the helper's previous attributes.
            let values = try FileManager.default.attributesOfItem(atPath: url.path)
            guard let size = values[.size] as? NSNumber, let date = values[.modificationDate] as? Date else {
                throw RuntimeError("Cannot identify background service: \(url.path)")
            }
            return "\(url.path):\(size):\(date.timeIntervalSince1970)"
        }.joined(separator: "\n")
    }

    private func emit(_ line: String) {
        report(line)
        onReport?(line)
    }

    func poll() {
        // This checks actual worker policy; unchanged XPC readiness must not
        // suppress it. Background windows send no maintenance requests.
        if active { updateActivity?(true) }
        reportStatus()
    }

    private func reportStatus() {
        guard let current = status?(), current != lastStatus else { return }
        lastStatus = current
        if current < 0 {
            emit("BRINEWELL work-context unavailable reason=renderer-connection-or-framework")
        } else {
            emit("BRINEWELL work-context \(current == 1 ? "ready" : "pending")")
        }
    }

    func stop() {
        startupGeneration &+= 1
        active = false
        stopMonitoring()
    }

    private func stopMonitoring() {
        monitor?.cancel()
        monitor = nil
        updateActivity?(false)
        updateActivity = nil
        status = nil
        if let library { dlclose(library) }
        library = nil
        lastStatus = nil
        unsetenv("NPT_WORK_CONTEXT_SERVICE")
    }
}
