import AppKit
import BrinewellCore
import SwiftUI

func adminErrorMessage(_ code: VMAdminErrorCode) -> String {
    switch code {
    case .unitMasked: String(localized: "SSH is masked in SteamOS. Unmask it in the terminal.")
    case .unitNotFound: String(localized: "The SSH service was not found in SteamOS.")
    case .sshApplyFailed: String(localized: "Could not change the SSH service.")
    case .sshPartiallyManaged: String(localized: "SSH could not be fully configured. Check its service and socket in SteamOS.")
    case .invalidPassword: String(localized: "The password is invalid.")
    case .accountMissing: String(localized: "The steamos account was not found.")
    case .accountLocked: String(localized: "The steamos account is locked. Confirm unlocking to continue.")
    case .passwordApplyFailed: String(localized: "Could not update the password.")
    case .notConnected, .disconnected: String(localized: "The SteamOS admin service is disconnected.")
    case .timeout: String(localized: "The SteamOS admin request timed out. Refresh and try again.")
    case .busy: String(localized: "Another admin operation is in progress. Try again.")
    case .ok, .invalidRequest, .unknownCommand, .internalError, .unknown:
        String(localized: "The SteamOS admin operation failed.")
    }
}

struct RemoteAccessSettingsSection: View {
    @ObservedObject var model: AppModel
    @ObservedObject var vm: VMProcess
    @State private var showingPasswordSheet = false
    @State private var copied = false
    @State private var passwordUpdated = false

    private var live: Bool { vm.state == .running && vm.adminConnected }
    private var canChangeSSH: Bool { live && !vm.isUpdatingSSH && !(vm.adminSnapshot.map { $0.ssh.isMasked && !$0.ssh.isActive } ?? false) }
    private var endpoint: VMSSHEndpoint { VMSSHEndpoint(port: vm.sshPort) }

    var body: some View {
        Section("Remote Access") {
            HStack {
                Toggle("SSH Access", isOn: Binding(
                    get: { vm.adminSnapshot?.ssh.isActive ?? false },
                    set: { vm.setGuestSSH(enabled: $0) }
                )).disabled(!canChangeSSH)
                if vm.isUpdatingSSH { ProgressView().controlSize(.small) }
            }
            sshStatus
            Text("Turning off SSH prevents new connections but does not disconnect existing sessions.")
                .font(.callout).foregroundStyle(.secondary)
            if let error = vm.adminActionError {
                Text(error).foregroundStyle(.red).textSelection(.enabled)
            }
            HStack {
                Text(verbatim: endpoint.command).font(.system(.callout, design: .monospaced)).textSelection(.enabled)
                Button(copied ? "Copied" : "Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(endpoint.command, forType: .string)
                    copied = true
                    Task { try? await Task.sleep(for: .seconds(2)); copied = false }
                }
            }
            LabeledContent("Host", value: endpoint.host)
            LabeledContent("Port", value: String(endpoint.port))
            LabeledContent("User", value: endpoint.username)
            forwardStatus
            LabeledContent("SteamOS sudo password") {
                HStack {
                    Text(passwordStatus)
                    Button(vm.adminSnapshot?.password.state == .unset ? "Set Password…" : "Change Password…") {
                        showingPasswordSheet = true
                    }.disabled(!live || vm.isUpdatingPassword)
                }
            }
            if passwordUpdated { Text("sudo password updated.").foregroundStyle(.green) }
        }
        .onAppear { syncContext(); if live { vm.requestAdminRefresh() } }
        .onChange(of: model.configuration.diskImagePath) { _, _ in syncContext() }
        .onChange(of: vm.state) { _, _ in syncContext() }
        .sheet(isPresented: $showingPasswordSheet) {
            ChangeSudoPasswordSheet(vm: vm) {
                passwordUpdated = true
                Task { try? await Task.sleep(for: .seconds(3)); passwordUpdated = false }
            }
        }
    }

    private func syncContext() { vm.syncAdminDiskContext(diskPath: model.configuration.diskImagePath) }

    @ViewBuilder private var sshStatus: some View {
        if live, let ssh = vm.adminSnapshot?.ssh {
            if ssh.isMasked {
                if ssh.isActive {
                    Text("SSH is masked. You can stop it, but cannot start it here.").foregroundStyle(.orange)
                } else {
                    Text("SSH is masked in SteamOS. Unmask it in the terminal.").foregroundStyle(.orange)
                }
            } else if ssh.isRunningOnly {
                HStack {
                    Text("SSH is running but is not enabled at boot.")
                    Button("Enable at Boot") { vm.setGuestSSH(enabled: true) }.disabled(!canChangeSSH)
                }
            } else if ssh.isBootOnly {
                HStack {
                    Text("SSH is stopped but is enabled at boot.")
                    Button("Disable at Boot") { vm.setGuestSSH(enabled: false) }.disabled(!canChangeSSH)
                }
            } else if ssh.isSocketOnly {
                Text("SSH is enabled on demand through a socket.")
            } else if ssh.isActive && ssh.isEnabledOnBoot {
                Text("SSH is running and enabled at boot.")
            } else {
                Text("SSH is off.")
            }
            if ssh.isPort22UnavailableWhileActive {
                Text("SSH is running but not listening on guest port 22. Local forwarding is unavailable.")
                    .foregroundStyle(.orange)
            }
        } else if vm.adminSnapshot != nil, let date = vm.adminLastSyncedAt {
            Text("Showing the last synchronized state (\(date.formatted(date: .abbreviated, time: .shortened))). Start the virtual machine for live status and changes.")
                .font(.callout).foregroundStyle(.secondary)
        } else {
            Text("Start the virtual machine to read the SSH and password status.")
                .font(.callout).foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private var forwardStatus: some View {
        if live || vm.sshForwardState != .unpublished {
            switch vm.sshForwardState {
            case .published:
                Label("Local port forwarding is ready", systemImage: "circle.fill").foregroundStyle(.green)
            case .publishing:
                HStack { ProgressView().controlSize(.small); Text("Configuring local port forwarding…") }
            case .unpublishing:
                HStack { ProgressView().controlSize(.small); Text("Closing local port forwarding...") }
            case .conflict(let message):
                Text("Local port \(vm.sshPort) is in use by another application. Do not connect to this address. (\(message))")
                    .foregroundStyle(.red).textSelection(.enabled)
            case .error(let message):
                Text("Local port forwarding failed: \(message)").foregroundStyle(.red).textSelection(.enabled)
            case .unpublished:
                Text("Local port forwarding is off").foregroundStyle(.secondary)
            }
        } else {
            Text("Available after starting the virtual machine and enabling SSH.").foregroundStyle(.secondary)
        }
    }

    private var passwordStatus: String {
        switch vm.adminSnapshot?.password.state {
        case .set: String(localized: "Set")
        case .unset: String(localized: "Not set")
        case .locked: String(localized: "Locked")
        case .unknown, .none: String(localized: "Unknown (start the virtual machine)")
        }
    }
}

struct ChangeSudoPasswordSheet: View {
    @ObservedObject var vm: VMProcess
    let onSuccess: () -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var password = ""
    @State private var confirmation = ""
    @State private var unlock = false
    @State private var lockedByServer = false
    @State private var error: VMAdminErrorCode?

    private var requiresUnlock: Bool { vm.adminSnapshot?.password.state == .locked || lockedByServer }
    private var validation: VMPasswordValidationError? {
        VMPasswordValidation.validate(password: password, confirmation: confirmation)
    }
    private var validationMessage: String? {
        guard !password.isEmpty || !confirmation.isEmpty, let validation else { return nil }
        switch validation {
        case .empty: return String(localized: "Enter a new password.")
        case .tooLong: return String(localized: "The password must be at most 256 bytes.")
        case .unsupportedCharacter: return String(localized: "The password cannot contain a colon, newline, or NUL character.")
        case .mismatch: return String(localized: "The passwords do not match.")
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(vm.adminSnapshot?.password.state == .unset ? "Set SteamOS sudo password" : "Change SteamOS sudo password")
                .font(.headline)
            Text("This password is used for sudo authorization and SSH password login for the steamos user in the SteamOS virtual machine.")
                .font(.callout)
            VStack(alignment: .leading, spacing: 10) {
                SecureField("New password", text: $password).onSubmit { submit() }
                SecureField("Confirm new password", text: $confirmation).onSubmit { submit() }
                if let validationMessage { Text(validationMessage).foregroundStyle(.red).font(.callout) }
                if requiresUnlock {
                    Text("The steamos account is locked.").foregroundStyle(.orange)
                    Toggle("Unlock steamos account and set new password", isOn: $unlock)
                }
                if let error { Text(adminErrorMessage(error)).foregroundStyle(.red).textSelection(.enabled) }
            }.disabled(vm.isUpdatingPassword)
            HStack {
                if vm.isUpdatingPassword { ProgressView().controlSize(.small) }
                Spacer()
                Button("Cancel") { close() }.keyboardShortcut(.cancelAction)
                Button("Save") { submit() }.keyboardShortcut(.defaultAction)
                    .disabled(validation != nil || (requiresUnlock && !unlock) || vm.isUpdatingPassword)
            }
        }
        .padding(24).frame(width: 500)
        .onDisappear { clear() }
        .onChange(of: vm.adminConnected) { _, connected in if !connected { close() } }
        .onChange(of: vm.state) { _, state in if state != .running { close() } }
    }

    private func clear() { password = ""; confirmation = "" }
    private func close() { clear(); dismiss() }
    private func submit() {
        guard validation == nil, !vm.isUpdatingPassword, !requiresUnlock || unlock else { return }
        error = nil
        let submittedPassword = password
        let confirmUnlock = requiresUnlock && unlock
        clear()
        Task {
            let result = await vm.setGuestSudoPassword(submittedPassword, confirmUnlock: confirmUnlock)
            switch result {
            case .success: clear(); onSuccess(); dismiss()
            case .failure(let code): error = code; if code == .accountLocked { lockedByServer = true }
            }
        }
    }
}
