import AppKit
import BrinewellCore
import SwiftUI

@MainActor
final class AppModel: ObservableObject {
    let host: HostCapabilities
    let vm = VMProcess()
    let deployment = DiskDeployment()
    let d3dMetalDeployment = D3DMetalDeployment()
    @Published var isShowingSetupSheet = false
    @Published var currentSetupStep: SetupStep = .systemDisk
    @Published var pendingStartAfterSetup = false
    @Published private(set) var setupReadiness: SetupReadiness
    private var dismissedReadiness: SetupReadiness?
    @Published private(set) var hasUsableDisk = false
    @Published private(set) var diskUsage: SystemDiskUsage?
    @Published private(set) var diskResizing = false
    @Published private(set) var diskResizeError: String?
    @Published var configuration: VMConfiguration {
        didSet {
            if configuration.diskImagePath != oldValue.diskImagePath { refreshDiskAvailability() }
            if configuration.diskImagePath != oldValue.diskImagePath ||
                configuration.graphicsBackend != oldValue.graphicsBackend ||
                configuration.d3dMetalFrameworkPath != oldValue.d3dMetalFrameworkPath {
                evaluateSetupReadiness()
            }
            // Automatic adaptation (for example a host display change) must not
            // persist over a blocked migration or a broken stored configuration:
            // the save would overwrite the user's file and clear loadError.
            // User-initiated edits keep the normal save path.
            let automaticWhileBlocked = applyingAutomaticChange
                && (migrationBlocked || loadError != nil)
            if configuration != oldValue && !isReloading && !automaticWhileBlocked {
                saveChanges(from: oldValue)
            }
        }
    }
    @Published private(set) var displayCapabilities: HostDisplayCapabilities?
    @Published private(set) var store: ConfigurationStore?
    @Published private(set) var loadError: String?
    @Published private(set) var saveError: String?
    private var isReloading = false
    /// Set while a non-user-initiated mutation is applied to `configuration`
    /// so didSet can keep it in memory only when persistence is unsafe.
    private var applyingAutomaticChange = false
    private var commandLineStartPending = CommandLine.arguments.dropFirst().contains("--start-vm")
    private var starting = false
    private var isTerminating = false
    private var needsTerminationWhenSettled = false
    func setTerminating(_ terminating: Bool) { isTerminating = terminating }
    let migrationReport: LegacyInstallationMigration.Report?
    private var migrationBlocked: Bool { migrationReport?.needsAttention == true }
    private var migrationAlertPresented = false
    /// Shared gate for every automatic onboarding presentation: a blocked
    /// migration or an unloadable configuration means this launch must not
    /// behave like a fresh install.
    private var canAutoPresentOnboarding: Bool { !migrationBlocked && loadError == nil }

    init(migrationReport: LegacyInstallationMigration.Report? = nil) {
        self.migrationReport = migrationReport
        host = HostCapabilities.detect()
        configuration = .defaults(for: host)
        setupReadiness = SetupReadiness(backend: .neptune, disk: .notConfigured, graphics: .d3dMetalMissing)
        reload()
        deployment.onInstalled = { [weak self] url in self?.useDisk(url, startAfterSelection: true) }
        d3dMetalDeployment.configurationStore = store
        d3dMetalDeployment.onInstalled = { [weak self] _ in self?.graphicsSetupChanged(reload: true) }
        d3dMetalDeployment.onUnsuccessfulFinish = { [weak self] in
            guard let self, self.isShowingSetupSheet else { return }
            self.refreshDiskAvailability()
            self.reconcileActiveSetupSheet()
        }
        vm.onShrinkReset = { [weak self] in self?.configuration.pendingDiskShrink = nil }
        vm.onTerminationWillSettle = { [weak self] in
            guard let self else { return }
            self.needsTerminationWhenSettled = !self.isTerminating
            self.setTerminating(true)
        }
        vm.onTerminateWhenSettled = { [weak self] in
            guard let self, self.needsTerminationWhenSettled else { return }
            self.needsTerminationWhenSettled = false
            NSApp.terminate(nil)
        }
        vm.onSettled = { [weak self] in
            self?.refreshDiskAvailability()
            self?.finishPendingDiskShrink()
        }
    }
    func refreshDisplayCapabilities() {
        let launcherScreen = NSApplication.shared.windows.first(where: { $0.title == "Brinewell" })?.screen
        let id = vm.hostDisplayID ?? launcherScreen.flatMap { HostDisplayCapabilities.screenID($0) }
        displayCapabilities = HostDisplayCapabilities.detect(displayID: id)
        if let mode = displayCapabilities?.preferredMode(for: configuration.display) {
            applyingAutomaticChange = true
            configuration.display = mode
            applyingAutomaticChange = false
        }
    }
    var configurationPath: String { store?.url.path ?? String(localized: "Configuration path unavailable") }
    private var diskIsUsable: Bool {
        guard let path = configuration.diskImagePath,
              FileManager.default.isWritableFile(atPath: path) else { return false }
        return (try? SteamOSDisk.validateSystem(at: URL(fileURLWithPath: path))) != nil
    }
    func refreshDiskAvailability() {
        hasUsableDisk = diskIsUsable
        if let path = configuration.diskImagePath, hasUsableDisk {
            diskUsage = try? SteamOSDisk.systemDiskUsage(at: URL(fileURLWithPath: path))
        } else { diskUsage = nil }
    }
    func evaluateSetupReadiness() {
        setupReadiness = SetupReadiness.evaluate(configuration, rosettaAvailable: { self.d3dMetalDeployment.rosettaAvailable })
    }
    func offerSetupIfNeeded(startAfterSetup: Bool = false, explicit: Bool = false) {
        guard canAutoPresentOnboarding, !isReloading, !vm.busy, !starting,
              !diskResizing, !deployment.busy, !d3dMetalDeployment.isActive, !isShowingSetupSheet else { return }
        evaluateSetupReadiness()
        if setupReadiness.isReadyToStart {
            dismissedReadiness = nil
            if startAfterSetup { start() }
            return
        }
        guard explicit || startAfterSetup || dismissedReadiness != setupReadiness else { return }
        currentSetupStep = setupReadiness.initialStep ?? .systemDisk
        pendingStartAfterSetup = startAfterSetup
        isShowingSetupSheet = true
    }
    func dismissSetup() {
        dismissedReadiness = setupReadiness
        pendingStartAfterSetup = false
        isShowingSetupSheet = false
        Task { await deployment.cancel(); await d3dMetalDeployment.cancelAndWait() }
    }
    func graphicsSetupChanged(reload: Bool = false) {
        if reload, let store {
            do {
                let updated = try store.load(for: host)
                isReloading = true
                configuration = updated
                isReloading = false
            } catch { saveError = error.localizedDescription; return }
        }
        reconcileActiveSetupSheet()
    }
    func reconcileActiveSetupSheet() {
        evaluateSetupReadiness()
        guard isShowingSetupSheet else { return }
        if setupReadiness.isReadyToStart {
            let shouldStart = pendingStartAfterSetup
            pendingStartAfterSetup = false
            isShowingSetupSheet = false
            dismissedReadiness = nil
            if shouldStart { start() }
        } else if currentSetupStep == .systemDisk && setupReadiness.disk.isReady && !setupReadiness.graphics.isReady {
            currentSetupStep = .graphics
        } else if currentSetupStep == .graphics && setupReadiness.graphics.isReady && !setupReadiness.disk.isReady {
            currentSetupStep = .systemDisk
        }
    }
    func useVenusForSetup() {
        setGraphicsBackend(.venus)
        guard saveError == nil else { return }
        graphicsSetupChanged()
    }
    func recheckRosetta() {
        d3dMetalDeployment.refreshRosettaStatus()
        graphicsSetupChanged()
    }
    func volumeChanged() {
        guard !deployment.busy, !d3dMetalDeployment.isActive else { return }
        refreshDiskAvailability()
        if isShowingSetupSheet { reconcileActiveSetupSheet() }
        else { offerSetupIfNeeded() }
    }
    private var startAfterDiskResize = false
    func start() {
        guard !isTerminating, !vm.busy, !deployment.busy, !d3dMetalDeployment.isActive, !starting else { return }
        guard !diskResizing else { return }
        refreshDiskAvailability()
        evaluateSetupReadiness()
        guard setupReadiness.isReadyToStart else {
            offerSetupIfNeeded(startAfterSetup: true, explicit: true)
            return
        }
        if pendingDiskShrink?.bootStarted == true {
            // The guest already shrank its file system on the previous boot;
            // finish the host-side shrink before the disk can be used again.
            if !diskResizing {
                startAfterDiskResize = true
                finishPendingDiskShrink()
            }
            return
        }
        starting = true
        defer { starting = false }
        var shrinkHomeKiB: UInt64?
        var markBootStarted: (() -> Void)?
        // While a migration failure is unresolved the guest must not receive a
        // shrink request: the matching host-side finalize stays disabled.
        if !migrationBlocked, let pending = pendingDiskShrink, !pending.bootStarted {
            do {
                shrinkHomeKiB = try SteamOSDisk.homeShrinkTargetKiB(
                    at: URL(fileURLWithPath: pending.diskImagePath),
                    toCapacityGiB: pending.capacityGiB, expected: pending.identity)
                markBootStarted = { [weak self] in
                    self?.configuration.pendingDiskShrink =
                        PendingDiskShrink(diskImagePath: pending.diskImagePath,
                                          capacityGiB: pending.capacityGiB,
                                          bootStarted: true, identity: pending.identity)
                }
            } catch {
                configuration.pendingDiskShrink = nil
                diskResizeError = error.localizedDescription
            }
        }
        // willLaunch runs after every pre-launch check passed, so bootStarted
        // is persisted only when the guest actually gets the shrink request.
        vm.start(configuration: configuration, host: host, display: displayCapabilities,
                 shrinkHomeKiB: shrinkHomeKiB, willLaunch: markBootStarted)
    }

    func startFromCommandLineIfRequested() {
        guard !isTerminating, commandLineStartPending else { return }
        guard canAutoPresentOnboarding, !diskResizing else { return }
        commandLineStartPending = false
        start()
    }
    private func useDisk(_ url: URL, startAfterSelection: Bool) {
        guard let store else { return }
        do {
            let updated = try store.update(for: host) { config in
                config.pendingDiskShrink = nil
                config.diskImagePath = url.path
            }
            isReloading = true
            configuration = updated
            isReloading = false
            saveError = nil
            graphicsSetupChanged()
        } catch { saveError = error.localizedDescription }
    }
    func reload() {
        guard !vm.busy, !deployment.busy else { return }
        isReloading = true
        defer { isReloading = false }
        saveError = nil
        do {
            let store = ConfigurationStore(url: try ConfigurationStore.defaultURL())
            self.store = store
            d3dMetalDeployment.configurationStore = store
            let loaded = try store.load(for: host)
            configuration = loaded
            loadError = nil
        } catch {
            if let store, let stored = try? store.decode() {
                configuration = stored
            }
            loadError = String(localized: "Could not load configuration (\(configurationPath)): \(error.localizedDescription)")
        }
        refreshDisplayCapabilities()
        refreshDiskAvailability()
        finishPendingDiskShrink()
    }
    private func saveChanges(from old: VMConfiguration) {
        guard !vm.busy, let store else { return }
        let changed = configuration
        do {
            guard let displayCapabilities else { throw DisplayUnavailable() }
            try displayCapabilities.validate(changed.display)
            let updated = try store.update(for: host) { fresh in
                if changed.diskImagePath != old.diskImagePath { fresh.diskImagePath = changed.diskImagePath }
                if changed.cpuCount != old.cpuCount { fresh.cpuCount = changed.cpuCount }
                if changed.memoryGiB != old.memoryGiB { fresh.memoryGiB = changed.memoryGiB }
                if changed.display != old.display { fresh.display = changed.display }
                if changed.graphicsBackend != old.graphicsBackend { fresh.graphicsBackend = changed.graphicsBackend }
                if changed.d3dMetalFrameworkPath != old.d3dMetalFrameworkPath { fresh.d3dMetalFrameworkPath = changed.d3dMetalFrameworkPath }
                if changed.neptuneApertureMiB != old.neptuneApertureMiB { fresh.neptuneApertureMiB = changed.neptuneApertureMiB }
                if changed.neptuneMultiRing != old.neptuneMultiRing { fresh.neptuneMultiRing = changed.neptuneMultiRing }
                if changed.gameHUDLevel != old.gameHUDLevel { fresh.gameHUDLevel = changed.gameHUDLevel }
                if changed.gameHUDPosition != old.gameHUDPosition { fresh.gameHUDPosition = changed.gameHUDPosition }
                if changed.sharedFolder != old.sharedFolder { fresh.sharedFolder = changed.sharedFolder }
                if changed.network != old.network { fresh.network = changed.network }
                if changed.pendingDiskShrink != old.pendingDiskShrink { fresh.pendingDiskShrink = changed.pendingDiskShrink }
            }
            isReloading = true
            configuration = updated
            isReloading = false
            loadError = nil
            saveError = nil
        } catch { saveError = String(localized: "Could not save configuration (\(store.url.path)): \(error.localizedDescription)") }
    }

    func setGameHUD(level: GameHUDLevel? = nil, position: GameHUDPosition? = nil) {
        guard !deployment.busy else { return }
        var updated = configuration
        if let level { updated.gameHUDLevel = level }
        if let position { updated.gameHUDPosition = position }
        configuration = updated
        vm.setGameHUD(level: updated.gameHUDLevel, position: updated.gameHUDPosition)
        guard vm.busy, let store else { return }
        do {
            let fresh = try store.update(for: host) {
                $0.gameHUDLevel = updated.gameHUDLevel
                $0.gameHUDPosition = updated.gameHUDPosition
            }
            isReloading = true
            configuration = fresh
            isReloading = false
            saveError = nil
        } catch { saveError = String(localized: "Could not save configuration (\(store.url.path)): \(error.localizedDescription)") }
    }

    /// Backend selection stays editable while the VM runs: it only changes the
    /// preference used by the next game launch, never boot-only settings.
    func setGraphicsBackend(_ backend: GraphicsBackend) {
        guard !deployment.busy else { return }
        if vm.busy {
            // While running (or transitioning) the live runtime must accept the
            // backend before it is persisted; keep the previous config on
            // refusal so the stored choice still matches reality.
            guard vm.state == .running, vm.setGraphicsBackend(backend) else { return }
        }
        guard let store else { return }
        do {
            let updated = try store.update(for: host) { $0.graphicsBackend = backend }
            isReloading = true
            configuration = updated
            isReloading = false
            saveError = nil
        } catch { saveError = String(localized: "Could not save configuration (\(store.url.path)): \(error.localizedDescription)") }
    }

    /// Hard migration failures mean the user's data did not move cleanly;
    /// explain where it actually is instead of letting this launch behave
    /// like a fresh install.
    func presentMigrationAlertIfNeeded() {
        guard let report = migrationReport, report.needsAttention, !migrationAlertPresented else { return }
        migrationAlertPresented = true
        let alert = NSAlert()
        alert.alertStyle = .warning
        switch report.status {
        case .conflict:
            alert.messageText = String(localized: "Brinewell found both an AppleDeck and a Brinewell data folder")
            alert.informativeText = String(localized: "Your previous data is at \(report.legacySupportURL.path), but \(report.supportURL.path) already exists, so nothing was moved. Review both folders and remove the one you do not need. Automatic startup is disabled for this launch.")
        case .pathOccupied:
            alert.messageText = String(localized: "Brinewell could not move your AppleDeck data")
            alert.informativeText = String(localized: "The path \(report.legacySupportURL.path) exists but is not a folder, so nothing was moved. Remove or rename it, then relaunch Brinewell. Automatic startup is disabled for this launch.")
        case .movedButConfigNeedsRepair:
            let configurationPath = report.supportURL.appendingPathComponent("configuration.json").path
            alert.messageText = report.movedData
                ? String(localized: "Brinewell moved your AppleDeck data but the configuration needs attention")
                : String(localized: "Brinewell could not use your saved configuration")
            if report.unreadableConfiguration {
                // A permission or I/O problem; deleting the file would not help.
                alert.informativeText = report.movedData
                    ? String(localized: "Your data was moved to \(report.supportURL.path), but the configuration file at \(configurationPath) could not be read. \(report.failureDescription ?? "") Check that the file is readable, then relaunch Brinewell. Automatic startup is disabled for this launch.")
                    : String(localized: "The configuration file at \(configurationPath) could not be read. \(report.failureDescription ?? "") Check that the file is readable, then relaunch Brinewell. Automatic startup is disabled for this launch.")
            } else if report.invalidConfiguration {
                alert.informativeText = report.movedData
                    ? String(localized: "Your data was moved to \(report.supportURL.path), but the configuration file at \(configurationPath) could not be decoded and was left unchanged. Repair or remove it, then relaunch Brinewell. Automatic startup is disabled for this launch.")
                    : String(localized: "The configuration file at \(configurationPath) could not be decoded and was left unchanged. Repair or remove it, then relaunch Brinewell. Automatic startup is disabled for this launch.")
            } else {
                alert.informativeText = report.movedData
                    ? String(localized: "Your data was moved to \(report.supportURL.path), but the configuration file could not be updated and was left unchanged. \(report.failureDescription ?? "") Automatic startup is disabled for this launch.")
                    : String(localized: "The configuration file at \(configurationPath) could not be updated and was left unchanged. \(report.failureDescription ?? "") Automatic startup is disabled for this launch.")
            }
        default:
            alert.messageText = String(localized: "Brinewell could not move your AppleDeck data")
            alert.informativeText = String(localized: "Your data is still at \(report.legacySupportURL.path). \(report.failureDescription ?? "") Automatic startup is disabled for this launch.")
        }
        alert.runModal()
    }

    private struct DisplayUnavailable: LocalizedError {
        var errorDescription: String? { String(localized: "Could not read the available display modes for the host display.") }
    }
    func chooseD3DMetalFramework() {
        guard !vm.busy, !deployment.busy, !diskResizing, !d3dMetalDeployment.isActive else { return }
        let panel = NSOpenPanel()
        panel.title = String(localized: "Import Local File")
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.treatsFilePackagesAsDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else { return }
        if isDirectory.boolValue || url.pathExtension.lowercased() == "dmg" {
            d3dMetalDeployment.importLocalItem(at: url)
        } else {
            do {
                _ = try D3DMetalFramework.inspect(at: url, allowBareBinary: true)
                guard let store else { return }
                let updated = try store.update(for: host) { $0.d3dMetalFrameworkPath = url.path }
                isReloading = true
                configuration = updated
                isReloading = false
                saveError = nil
                graphicsSetupChanged()
            } catch { saveError = error.localizedDescription }
        }
    }

    func chooseSharedFolder() {
        guard !vm.busy, !deployment.busy else { return }
        let panel = NSOpenPanel()
        panel.title = String(localized: "Choose Shared Folder")
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let folder = VMSharedFolder(path: url.path)
        do {
            _ = try folder.resolvedPath()
            configuration.sharedFolder = folder
        } catch { saveError = error.localizedDescription }
    }

    func chooseDisk(startAfterSelection: Bool = false) {
        guard !vm.busy, !deployment.busy, !diskResizing else { return }
        let panel = NSOpenPanel()
        panel.title = String(localized: "Choose Existing System Disk")
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try SteamOSDisk.validateSystem(at: url)
            guard FileManager.default.isWritableFile(atPath: url.path) else {
                saveError = String(localized: "The selected system disk is not writable.")
                return
            }
            useDisk(url, startAfterSelection: startAfterSelection)
        } catch { saveError = error.localizedDescription }
    }

    private var diskResizeTask: Task<Void, Never>?
    var onDiskResizeFinished: (() -> Void)?
    func waitForDiskResize() async { await diskResizeTask?.value }

    var pendingDiskShrink: PendingDiskShrink? {
        guard let pending = configuration.pendingDiskShrink,
              pending.diskImagePath == configuration.diskImagePath else { return nil }
        return pending
    }

    /// Grow applies on the host immediately; shrink is scheduled for the next
    /// boot so the guest can reduce its home file system first.
    func applyDiskCapacity(_ newCapacityGiB: Int) {
        guard !vm.busy, !deployment.busy, !diskResizing, !d3dMetalDeployment.isActive,
              let usage = diskUsage, let path = configuration.diskImagePath,
              newCapacityGiB >= usage.minimumCapacityGiB,
              newCapacityGiB <= usage.maximumCapacityGiB else { return }
        let newBytes = UInt64(newCapacityGiB) * 1_073_741_824
        guard newBytes != usage.capacityBytes else { return }
        let url = URL(fileURLWithPath: path)
        diskResizeError = nil
        if newBytes > usage.capacityBytes {
            configuration.pendingDiskShrink = nil
            diskResizing = true
            diskResizeTask = Task {
                do {
                    try await Task.detached { try SteamOSDisk.growSystem(at: url, toCapacityGiB: newCapacityGiB) }.value
                } catch {
                    diskResizeError = error.localizedDescription
                }
                diskResizing = false
                refreshDiskAvailability()
                let done = onDiskResizeFinished
                onDiskResizeFinished = nil
                done?()
            }
        } else {
            do {
                _ = try SteamOSDisk.homeShrinkTargetKiB(at: url, toCapacityGiB: newCapacityGiB,
                                                        expected: usage.identity)
                configuration.pendingDiskShrink = PendingDiskShrink(
                    diskImagePath: path, capacityGiB: newCapacityGiB, identity: usage.identity)
            } catch {
                diskResizeError = error.localizedDescription
            }
        }
    }

    func cancelDiskShrink() {
        guard !vm.busy, !diskResizing else { return }
        configuration.pendingDiskShrink = nil
    }

    /// Disk errors that can succeed on retry keep the scheduled shrink; any
    /// other failure clears it and is shown to the user.
    private static let retryableDiskErrors: Set<String> = [
        String(localized: "The SteamOS disk is in use. Shut down the virtual machine first."),
        String(localized: "Could not lock the system disk for resizing."),
        String(localized: "Could not lock the staged disk image."),
    ]

    /// Host-side second phase of a scheduled shrink. Runs at app launch, before
    /// the next VM start, and after VM teardown before app termination.
    private func finishPendingDiskShrink() {
        guard !vm.busy, !deployment.busy, !diskResizing, !migrationBlocked,
              let pending = pendingDiskShrink, pending.bootStarted,
              let path = configuration.diskImagePath else { return }
        let url = URL(fileURLWithPath: path)
        diskResizing = true
        diskResizeError = nil
        diskResizeTask = Task {
            do {
                try await Task.detached {
                    try SteamOSDisk.shrinkSystem(at: url, toCapacityGiB: pending.capacityGiB,
                                                 expected: pending.identity)
                }.value
                configuration.pendingDiskShrink = nil
            } catch {
                let message = error.localizedDescription
                diskResizeError = String(localized: "The disk could not be shrunk. \(message)")
                if !Self.retryableDiskErrors.contains(message) {
                    configuration.pendingDiskShrink = nil
                }
            }
            diskResizing = false
            refreshDiskAvailability()
            let done = onDiskResizeFinished
            onDiskResizeFinished = nil
            done?()
            let relaunch = startAfterDiskResize
            startAfterDiskResize = false
            if !isTerminating {
                if relaunch { start() }
                startFromCommandLineIfRequested()
            }
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    var vm: VMProcess?
    var deployment: DiskDeployment?
    var model: AppModel?
    private var quitting = false
    func applicationWillFinishLaunching(_ notification: Notification) {
        NSApplication.shared.setActivationPolicy(.regular)
    }
    private func finishTermination(_ sender: NSApplication) {
        if let vm, vm.busy {
            vm.onExit = { [weak self, weak sender] in
                guard let self, let sender else { return }
                self.finishTermination(sender)
            }
            vm.stop()
            return
        }
        Task { @MainActor in
            if let model {
                await model.waitForDiskResize()
                await model.deployment.cancel()
                await model.d3dMetalDeployment.cancelAndWait()
            }
            guard quitting else { return }
            quitting = false
            sender.reply(toApplicationShouldTerminate: true)
        }
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if quitting { return .terminateLater }
        model?.setTerminating(true)
        if let vm, vm.busy {
            let alert = NSAlert()
            alert.messageText = String(localized: "The virtual machine is still running")
            alert.informativeText = String(localized: "Shut down the virtual machine before quitting Brinewell?")
            alert.addButton(withTitle: String(localized: "Shut Down and Quit"))
            alert.addButton(withTitle: String(localized: "Cancel"))
            guard alert.runModal() == .alertFirstButtonReturn else {
                model?.setTerminating(false)
                return .terminateCancel
            }
        }
        if vm?.busy == true || model?.diskResizing == true || model?.deployment.busy == true || model?.d3dMetalDeployment.isActive == true {
            quitting = true
            finishTermination(sender)
            return .terminateLater
        }
        return .terminateNow
    }
}

@main
@MainActor
struct BrinewellApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model: AppModel

    // Runs before AppModel is created, so the configuration load, disk-shrink
    // finalize and language preference all see the migrated locations.
    init() {
        signal(SIGPIPE, SIG_IGN)
        let report = LegacyInstallationMigration.migrateIfNeeded()
        _model = StateObject(wrappedValue: AppModel(migrationReport: report))
    }
    var body: some Scene {
        Window("Brinewell", id: "main") {
            MainView(model: model, vm: model.vm, deployment: model.deployment) {
                delegate.vm = model.vm
                delegate.deployment = model.deployment
                delegate.model = model
            }
        }.defaultSize(width: 540, height: 620)
        .defaultLaunchBehavior(.presented)
        Settings {
            SettingsView(model: model, vm: model.vm, deployment: model.deployment)
                .frame(width: 660, height: 680)
        }
    }
}

private struct MainView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var vm: VMProcess
    @ObservedObject var deployment: DiskDeployment
    @State private var confirmForce = false
    let bindDelegate: () -> Void

    var body: some View {
        Form {
            Section("SteamOS") {
                LabeledContent("Status", value: vm.state.title)
                if let error = vm.error { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                if let warning = vm.workContextWarning {
                    Text(warning).foregroundStyle(.orange).textSelection(.enabled)
                }
                HStack {
                    Button("Start") { model.start() }
                        .disabled(vm.busy || deployment.busy || model.loadError != nil || model.diskResizing)
                    Button("Shut Down") { vm.stop() }.disabled(!vm.busy || vm.state == .stopping)
                    Button("Force Stop") { confirmForce = true }.disabled(!vm.busy)
                    SettingsLink { Text("Open Settings") }
                }
                if let path = vm.runDirectory?.path { LabeledContent("Run Directory", value: path).textSelection(.enabled) }
                if let path = vm.logPath { LabeledContent("Host Log", value: path).textSelection(.enabled) }
            }
            UUPluginView(vm: vm)
            Section("Host") {
                LabeledContent("Architecture", value: model.host.architecture)
                LabeledContent("Logical CPUs", value: "\(model.host.logicalCPUCount)")
                LabeledContent("Unified Memory", value: ByteCountFormatter.string(fromByteCount: Int64(model.host.physicalMemoryBytes), countStyle: .memory))
                LabeledContent("GPU", value: model.host.metalDeviceName ?? String(localized: "Unavailable"))
            }
            Section("Configuration") {
                LabeledContent("Disk", value: model.hasUsableDisk ? (model.configuration.diskImagePath ?? "") : String(localized: "No usable system disk")).textSelection(.enabled)
                if !model.hasUsableDisk {
                    Button("Choose System Disk or Install SteamOS") { model.offerSetupIfNeeded(explicit: true) }.disabled(vm.busy || model.diskResizing)
                }
                if let error = model.loadError {
                    Text(error).foregroundStyle(.red).textSelection(.enabled)
                    Button("Reload Configuration") { model.reload() }.disabled(vm.busy)
                }
                if let error = model.saveError { Text(error).foregroundStyle(.red).textSelection(.enabled) }
            }
        }
        .formStyle(.grouped)
        .onAppear {
            bindDelegate()
            model.presentMigrationAlertIfNeeded()
            model.refreshDisplayCapabilities()
            model.startFromCommandLineIfRequested()
            if !model.setupReadiness.isReadyToStart { model.offerSetupIfNeeded(startAfterSetup: true) }
        }
        .sheet(isPresented: $model.isShowingSetupSheet, onDismiss: { model.dismissSetup() }) {
            SetupView(model: model, diskDeployment: deployment, d3dMetalDeployment: model.d3dMetalDeployment,
                      readiness: model.setupReadiness, currentStep: $model.currentSetupStep,
                      onUseVenus: { model.useVenusForSetup() }, onDismiss: { model.dismissSetup() })
        }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didChangeScreenNotification)) { _ in model.refreshDisplayCapabilities() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didChangeScreenParametersNotification)) { _ in model.refreshDisplayCapabilities() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in model.volumeChanged() }
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didUnmountNotification)) { _ in model.volumeChanged() }
        .onReceive(NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didMountNotification)) { _ in model.volumeChanged() }
        .onChange(of: vm.hostDisplayID) { model.refreshDisplayCapabilities() }
        .confirmationDialog("Force stopping may corrupt guest data.", isPresented: $confirmForce) {
            Button("Force Stop Virtual Machine", role: .destructive) { vm.forceStop() }
            Button("Cancel", role: .cancel) { }
        }
    }
}

private struct SettingsView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var vm: VMProcess
    @ObservedObject var deployment: DiskDeployment
    @State private var language = AppLanguage.selected
    @State private var requestedGiB: Int?
    @State private var confirmApply = false
    @ObservedObject private var graphicsDeployment: D3DMetalDeployment

    init(model: AppModel, vm: VMProcess, deployment: DiskDeployment) {
        self.model = model
        self.vm = vm
        self.deployment = deployment
        self.graphicsDeployment = model.d3dMetalDeployment
    }

    private var capacityValid: Bool {
        guard let request = requestedGiB, let usage = model.diskUsage else { return false }
        return UInt64(request) * 1_073_741_824 != usage.capacityBytes
            && request >= usage.minimumCapacityGiB
            && request <= usage.maximumCapacityGiB
    }

    private func capacityLabel(_ bytes: UInt64) -> String {
        let gib = Double(bytes) / 1_073_741_824
        if bytes % 1_073_741_824 == 0 {
            return String(localized: "\(Int(gib)) GB")
        }
        return String(localized: "\(gib.formatted(.number.precision(.fractionLength(1)))) GB")
    }

    private var applyMessage: String {
        guard let request = requestedGiB, let usage = model.diskUsage else { return "" }
        let currentGiB = Int(usage.capacityBytes / 1_073_741_824)
        if request > currentGiB {
            return String(localized: "Expand the system disk from \(currentGiB) GB to \(request) GB?")
        }
        return String(localized: "The disk will be shrunk to \(request) GB the next time the virtual machine starts. SteamOS reduces its home file system during startup, which may take several minutes; the image is reduced after the virtual machine shuts down.")
    }

    private func applyCapacityChange() {
        guard let request = requestedGiB, let usage = model.diskUsage,
              request >= usage.minimumCapacityGiB,
              request <= usage.maximumCapacityGiB else { return }
        model.applyDiskCapacity(request)
    }

    private var resolutionSelection: Binding<DisplayResolution> {
        Binding(get: { model.configuration.display.resolution }, set: { resolution in
            var requested = model.configuration.display
            requested.resolution = resolution
            if let mode = model.displayCapabilities?.preferredMode(for: requested) {
                model.configuration.display = mode
            }
        })
    }

    private var displayError: String? {
        do {
            guard let capabilities = model.displayCapabilities else { return String(localized: "Could not read the available display modes for the host display.") }
            try capabilities.validate(model.configuration.display)
            return nil
        } catch { return error.localizedDescription }
    }

    var body: some View {
        Form {
            Section {
                Picker("Language", selection: $language) {
                    Text("Follow System").tag(AppLanguage.system)
                    Text(verbatim: "English").tag(AppLanguage.english)
                    Text(verbatim: "简体中文").tag(AppLanguage.simplifiedChinese)
                }
                .onChange(of: language) { _, selection in selection.save() }
                if language != AppLanguage.atLaunch {
                    Text("Quit and reopen Brinewell to apply a language change.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            Section("Guest Disk") {
                if let path = model.configuration.diskImagePath {
                    Text(path).textSelection(.enabled)
                    if let usage = model.diskUsage {
                        LabeledContent("Current Capacity", value: capacityLabel(usage.capacityBytes))
                        LabeledContent("Used on Mac", value: ByteCountFormatter.string(fromByteCount: Int64(usage.allocatedBytes), countStyle: .file))
                        if let pending = model.pendingDiskShrink {
                            Text(pending.bootStarted
                                 ? String(localized: "Shrinking to \(pending.capacityGiB) GB will finish the next time Brinewell opens or before the next startup.")
                                 : String(localized: "Shrinking to \(pending.capacityGiB) GB is scheduled for the next startup."))
                                .font(.callout).foregroundStyle(.secondary)
                            Button("Cancel Shrink") { model.cancelDiskShrink() }
                                .disabled(graphicsDeployment.isActive)
                        } else {
                            HStack {
                                TextField("New Capacity", value: $requestedGiB, format: .number)
                                    .disabled(graphicsDeployment.isActive)
                                Text("GB").foregroundStyle(.secondary)
                            }
                            Text("Between \(usage.minimumCapacityGiB) and \(usage.maximumCapacityGiB) GB.")
                                .font(.callout).foregroundStyle(.secondary)
                            Button("Apply…") { confirmApply = true }
                                .disabled(!capacityValid || vm.busy || deployment.busy || model.diskResizing || graphicsDeployment.isActive)
                        }
                    }
                    if model.diskResizing {
                        HStack {
                            ProgressView().controlSize(.small)
                            Text("Resizing the system disk…")
                        }
                    }
                    if let error = model.diskResizeError {
                        Text(error).foregroundStyle(.red).textSelection(.enabled)
                    }
                    Button("Clear Disk Path") {
                        model.configuration.pendingDiskShrink = nil
                        model.configuration.diskImagePath = nil
                    }
                } else { Text("No disk selected").foregroundStyle(.secondary) }
                Button("Choose Existing File") { model.chooseDisk() }
            }.disabled(vm.busy || model.diskResizing)
            Section("Shared Folder") {
                if let folder = model.configuration.sharedFolder {
                    Text(folder.path).textSelection(.enabled)
                    Toggle("Read Only", isOn: Binding(
                        get: { model.configuration.sharedFolder?.readOnly ?? true },
                        set: { model.configuration.sharedFolder?.readOnly = $0 }
                    ))
                    Text("After the next boot, open /run/brinewell/shared in SteamOS.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                HStack {
                    Button(model.configuration.sharedFolder == nil ? String(localized: "Choose Folder") : String(localized: "Change Folder")) {
                        model.chooseSharedFolder()
                    }
                    if model.configuration.sharedFolder != nil {
                        Button("Remove Share") { model.configuration.sharedFolder = nil }
                    }
                }
            }.disabled(vm.busy)
            Section("Network") {
                Toggle("Enable Proxy", isOn: $model.configuration.network.proxyEnabled)
                TextField("Proxy Address", text: $model.configuration.network.proxyURL,
                          prompt: Text("socks5://127.0.0.1:7890"))
                Text("SOCKS5, HTTP, and HTTPS are supported. Include a port in the address. With the proxy off, traffic goes directly through the Mac’s physical network interface and bypasses TUN.")
                    .font(.callout).foregroundStyle(.secondary)
                Text("HTTP and HTTPS proxies do not support UDP. Games and services that rely on UDP may fail to connect. Use a SOCKS5 proxy with UDP support if needed.")
                    .font(.callout).foregroundStyle(.secondary)
            }.disabled(vm.busy)
            Section("Resources") {
                Stepper("CPU: \(model.configuration.cpuCount)", value: $model.configuration.cpuCount, in: 1...model.host.maximumCPUs)
                Stepper("Memory: \(model.configuration.memoryGiB) GiB", value: $model.configuration.memoryGiB, in: 1...model.host.maximumMemoryGiB)
            }.disabled(vm.busy)
            Section("Virtual Machine Display") {
                if let capabilities = model.displayCapabilities {
                    LabeledContent("Host Display", value: capabilities.name)
                    Picker("Resolution", selection: resolutionSelection) {
                        ForEach(capabilities.resolutions, id: \.self) { resolution in
                            Text(verbatim: resolution.label).tag(resolution)
                        }
                    }
                    Picker("Refresh Rate", selection: $model.configuration.display.refreshRate) {
                        ForEach(capabilities.refreshRates(for: model.configuration.display.resolution), id: \.self) { rate in
                            Text("\(rate) Hz").tag(rate)
                        }
                    }
                    Text("Up to \(capabilities.maximumResolution.label) at \(capabilities.maximumRefreshRate) Hz. Available modes depend on the current host display.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                Text("The Steam interface follows the display resolution. The window scales proportionally and stays within the limits of the current display.")
                    .font(.callout).foregroundStyle(.secondary)
                if let error = displayError { Text(error).foregroundStyle(.red) }
            }.disabled(vm.busy)
            Section("Graphics Backend") {
                Picker("Backend", selection: Binding(
                    get: { model.configuration.graphicsBackend },
                    set: { model.setGraphicsBackend($0) }
                )) {
                    Text("Neptune / D3DMetal").tag(GraphicsBackend.neptune)
                    Text("Neptune / Mullion").tag(GraphicsBackend.mullion)
                    Text("Venus Only").tag(GraphicsBackend.venus)
                }.disabled(graphicsDeployment.isActive)
                if vm.state == .running {
                    Text("Backend changes apply to the next game launch; the framework, aperture, and multi-ring settings still only apply at boot.")
                        .font(.callout).foregroundStyle(.secondary)
                    if let applied = vm.graphicsApplied {
                        Text("Active: \(applied.title)").font(.callout).foregroundStyle(.secondary)
                    }
                    if let error = vm.graphicsError { Text(error).foregroundStyle(.red) }
                }
                Group {
                    if model.configuration.graphicsBackend == .neptune {
                        if let path = model.configuration.d3dMetalFrameworkPath {
                            if let inspection = try? D3DMetalFramework.inspect(at: URL(fileURLWithPath: path), allowBareBinary: true) {
                                LabeledContent("Version", value: inspection.version?.displayVersion ?? String(localized: "Unknown"))
                                Text(inspection.version?.isRecommendedContextVersion == true
                                     ? String(localized: "4.0b3 context integration (subject to runtime verification)")
                                     : String(localized: "Standard rendering"))
                                    .font(.callout).foregroundStyle(.secondary)
                            } else {
                                Text(String(localized: "The previously configured D3DMetal framework could not be used:"))
                                    .foregroundStyle(.red)
                                if case .d3dMetalInvalid(let reason) = model.setupReadiness.graphics { Text(reason).foregroundStyle(.red) }
                            }
                            Text(path).textSelection(.enabled)
                        } else { Text("Not configured").foregroundStyle(.secondary) }
                        if !graphicsDeployment.rosettaAvailable {
                            Text("Rosetta 2 is required to run D3DMetal on Apple Silicon.").foregroundStyle(.orange)
                            Button("Recheck") { model.recheckRosetta() }
                        }
                        if graphicsDeployment.isActive {
                            if case .downloading(let name, let received, let total) = graphicsDeployment.phase,
                               let total, total > 0 {
                                Text(name)
                                ProgressView(value: Double(received) / Double(total))
                            } else {
                                ProgressView()
                            }
                            Button("Cancel") { Task { await graphicsDeployment.cancelAndWait() } }
                        } else {
                            if case .failed(let message) = graphicsDeployment.phase { Text(message).foregroundStyle(.red) }
                            HStack {
                                Button(model.configuration.d3dMetalFrameworkPath == nil ? String(localized: "Download from Apple") : String(localized: "Update from Apple")) { graphicsDeployment.startAppleDownload() }
                                Button("Import Local File") { model.chooseD3DMetalFramework() }
                                if model.configuration.d3dMetalFrameworkPath != nil {
                                    Button("Clear") { model.configuration.d3dMetalFrameworkPath = nil; model.evaluateSetupReadiness() }
                                }
                            }.disabled(vm.busy || model.diskResizing || model.isShowingSetupSheet)
                        }
                    }
                    if model.configuration.graphicsBackend != .venus {
                        Stepper("Neptune aperture: \(model.configuration.neptuneApertureMiB) MiB",
                                value: $model.configuration.neptuneApertureMiB, in: 8192...65536, step: 8192)
                        Toggle("Experimental Multi-Ring (Global)", isOn: $model.configuration.neptuneMultiRing)
                    }
                }.disabled(vm.busy || model.diskResizing)
            }
            Section("Game Performance") {
                Picker("Overlay Level", selection: Binding(
                    get: { model.configuration.gameHUDLevel },
                    set: { model.setGameHUD(level: $0) }
                )) {
                    ForEach(GameHUDLevel.allCases, id: \.self) { level in
                        Text(level.title).tag(level)
                    }
                }
                Text(model.configuration.gameHUDLevel.detail)
                    .font(.callout).foregroundStyle(.secondary)
                Picker("Overlay Position", selection: Binding(
                    get: { model.configuration.gameHUDPosition },
                    set: { model.setGameHUD(position: $0) }
                )) {
                    ForEach(GameHUDPosition.allCases, id: \.self) { position in
                        Text(position.title).tag(position)
                    }
                }.disabled(model.configuration.gameHUDLevel == .off)
                Text("Game frame times measure the interval between successful frame submissions. CPU, GPU, memory, and battery metrics cover the whole Mac. A dash (—) indicates unavailable data.")
                    .font(.callout).foregroundStyle(.secondary)
                Text("Frame statistics are available for Neptune and Vulkan X11 games. Native Wayland games are currently unsupported. The SteamOS performance overlay can stay off.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            Section("Configuration File") {
                Text(model.configurationPath).textSelection(.enabled)
                if let error = model.loadError {
                    Text(error).foregroundStyle(.red).textSelection(.enabled)
                    Button("Reload Configuration") { model.reload() }.disabled(vm.busy)
                }
                if let error = model.saveError { Text(error).foregroundStyle(.red).textSelection(.enabled) }
            }
        }.formStyle(.grouped)
            .disabled(deployment.busy || model.diskResizing)
            .confirmationDialog("Resize System Disk", isPresented: $confirmApply) {
                Button("Apply") { applyCapacityChange() }
                Button("Cancel", role: .cancel) { }
            } message: {
                Text(applyMessage)
            }
            .onChange(of: model.diskUsage) { requestedGiB = nil }
            .sheet(isPresented: Binding(
                get: { graphicsDeployment.isPresentingLoginSheet && !model.isShowingSetupSheet },
                set: { graphicsDeployment.isPresentingLoginSheet = $0 }
            ), onDismiss: {
                if case .signingIn = graphicsDeployment.phase { Task { await graphicsDeployment.cancelAndWait() } }
            }) {
                AppleDeveloperLoginView(stagingDirectory: D3DMetalDeployment.downloadDirectory, deployment: graphicsDeployment) { event in
                    switch event {
                    case .authorizedForNativeDownload(let artifact, let authorization):
                        graphicsDeployment.handleAuthorizedDownload(artifact: artifact, authorization: authorization)
                    case .downloadedToLocalFile(let url, let name):
                        graphicsDeployment.handleWebKitDownloadedDMG(fileURL: url, artifactName: name)
                    case .cancelled: Task { await graphicsDeployment.cancelAndWait() }
                    case .failed(let message):
                        graphicsDeployment.isPresentingLoginSheet = false
                        graphicsDeployment.phase = .failed(message)
                    }
                }
            }
    }
}
