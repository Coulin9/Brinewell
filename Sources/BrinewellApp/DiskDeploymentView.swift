import AppKit
import SwiftUI

struct DiskDeploymentView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var deployment: DiskDeployment
    @State private var installing = false

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(installing ? String(localized: "Install SteamOS") : String(localized: "Set Up System Disk")).font(.headline)
                Spacer()
            }.padding(20)
            Form {
                if !installing {
                    Section {
                        if case .missingConfigured(let url) = model.setupReadiness.disk {
                            Text("The configured system disk is missing. Reconnect its drive or choose an existing disk.")
                                .foregroundStyle(.orange)
                            Text(url.path).textSelection(.enabled)
                        } else {
                            Text("No usable system disk was found.")
                        }
                        Button("Choose Existing System Disk") { model.chooseDisk(startAfterSelection: true) }
                            .buttonStyle(.borderedProminent)
                        Text("Use an existing Brinewell system disk.").font(.callout).foregroundStyle(.secondary)
                    }
                    Section {
                        Button("Fresh Install") { installing = true }
                        Text("Create a new disk and install SteamOS.").font(.callout).foregroundStyle(.secondary)
                    }
                } else {
                    Section("Installation Image") {
                        Picker("Source", selection: $deployment.source) {
                            Text("Download from Valve").tag(DiskDeployment.Source.valve)
                            Text("Local Image").tag(DiskDeployment.Source.local)
                        }
                        if deployment.source == .valve {
                            Text("SteamOS · ARM64").foregroundStyle(.secondary)
                        } else {
                            LabeledContent("Image", value: deployment.installerURL?.lastPathComponent ?? String(localized: "None selected"))
                            Button("Choose Installation Image") { chooseInstaller() }
                            Text("Supports ARM64 recovery images in .img, .raw, and .img.bz2 format.")
                                .font(.callout).foregroundStyle(.secondary)
                        }
                    }.disabled(deployment.busy)
                    Section("New System Disk") {
                        Picker("Capacity", selection: $deployment.capacityGiB) {
                            ForEach([64, 128, 256, 512, 1024], id: \.self) { size in
                                Text(size == 1024 ? "1 TiB" : "\(size) GiB").tag(size)
                            }
                        }
                        LabeledContent("Location") {
                            Text(deployment.destination.path).lineLimit(2).truncationMode(.middle).textSelection(.enabled)
                        }
                        Button("Change Location") { chooseDestination() }
                    }.disabled(deployment.busy)
                    Section("Virtual Machine Resources") {
                        Stepper("CPU: \(model.configuration.cpuCount)", value: $model.configuration.cpuCount,
                                in: 1...model.host.maximumCPUs)
                        Stepper("Memory: \(model.configuration.memoryGiB) GiB", value: $model.configuration.memoryGiB,
                                in: 1...model.host.maximumMemoryGiB)
                    }.disabled(deployment.busy)
                    if deployment.busy || deployment.phase == .paused {
                        Section {
                            Text(progressTitle)
                            if let progress = deployment.progress { ProgressView(value: progress) }
                            else if deployment.busy { ProgressView().controlSize(.small) }
                            if deployment.phase == .downloading {
                                Text(downloadSize).font(.caption).foregroundStyle(.secondary).monospacedDigit()
                            }
                        }
                    }
                    if let error = deployment.error { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                }
                if let error = model.saveError { Text(error).foregroundStyle(.red).textSelection(.enabled) }
            }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)
            Divider()
            HStack {
                if installing && !deployment.busy {
                    Button("Back") { installing = false }.buttonStyle(.bordered)
                }
                Spacer()
                if deployment.busy {
                    Button("Stop Installation") { Task { await deployment.cancel() } }.buttonStyle(.bordered)
                } else {
                    Button("Cancel") { model.dismissSetup() }
                        .buttonStyle(.bordered).keyboardShortcut(.cancelAction)
                    if installing {
                        Button(deployment.phase == .paused ? String(localized: "Resume Installation") : String(localized: "Install")) { deployment.install() }
                            .buttonStyle(.borderedProminent)
                            .keyboardShortcut(.defaultAction)
                            .disabled(deployment.source == .local && deployment.installerURL == nil)
                    }
                }
            }.padding(16)
        }
        .frame(width: 540, height: installing ? 640 : 350)
        .background(.regularMaterial)
        .interactiveDismissDisabled(deployment.busy)
    }

    private var progressTitle: String {
        switch deployment.phase {
        case .downloading: String(localized: "Downloading SteamOS…")
        case .verifying: String(localized: "Verifying installation image…")
        case .preparing: String(localized: "Installing SteamOS…")
        case .paused: String(localized: "Installation stopped")
        case .failed: String(localized: "Installation failed")
        case .idle: String(localized: "Ready to install")
        }
    }

    private var downloadSize: String {
        let received = ByteCountFormatter.string(fromByteCount: deployment.receivedBytes, countStyle: .file)
        guard let total = deployment.totalBytes else { return received }
        return "\(received) / \(ByteCountFormatter.string(fromByteCount: total, countStyle: .file))"
    }

    private func chooseInstaller() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Choose SteamOS Installation Image")
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        deployment.installerURL = url
    }

    private func chooseDestination() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Choose a Location for the New System Disk")
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        deployment.destination = url
    }
}
