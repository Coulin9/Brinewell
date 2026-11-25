import AppKit
import BrinewellCore
import SwiftUI
import UniformTypeIdentifiers

struct GraphicsSetupView: View {
    @ObservedObject var d3dMetalDeployment: D3DMetalDeployment
    let readiness: SetupReadiness
    let onDismiss: () -> Void
    let onRecheck: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "display").accessibilityHidden(true)
                Text("Graphics Setup").font(.headline)
                Spacer()
            }.padding(20)
            Form {
                Section {
                    Text("Neptune uses Apple’s D3DMetal (Game Porting Toolkit) for DirectX 11 and 12 games. Sign in with a free Apple Account to download, verify, and configure it automatically.")
                        .font(.callout)
                }
                if !d3dMetalDeployment.rosettaAvailable {
                    Section("Rosetta 2 Required") {
                        Text("Rosetta 2 is required to run D3DMetal on Apple Silicon.")
                        HStack {
                            Text(verbatim: "softwareupdate --install-rosetta")
                                .font(.system(.callout, design: .monospaced)).textSelection(.enabled)
                            Button("Copy") {
                                NSPasteboard.general.clearContents()
                                NSPasteboard.general.setString("softwareupdate --install-rosetta", forType: .string)
                            }
                            Button("Recheck", action: onRecheck)
                        }
                    }
                }
                if case .rosettaMissing = readiness.graphics {
                    Text("D3DMetal is installed.").foregroundStyle(.secondary)
                }
                if d3dMetalDeployment.isActive {
                    Section { progressArea }
                } else {
                    Section {
                        if case .d3dMetalInvalid(let reason) = readiness.graphics {
                            Text("The previously configured D3DMetal framework could not be used:")
                                .font(.callout).foregroundStyle(.secondary)
                            Text(reason).font(.callout).textSelection(.enabled)
                        }
                        if case .failed(let message) = d3dMetalDeployment.phase {
                            Text(message).foregroundStyle(.red).textSelection(.enabled)
                        }
                        Text("Apple’s Game Porting Toolkit is downloaded directly from Apple and is subject to Apple’s Software License Agreement. Brinewell does not bundle Apple software.")
                            .font(.callout).foregroundStyle(.secondary)
                        Button {
                            d3dMetalDeployment.startAppleDownload()
                        } label: {
                            if case .failed = d3dMetalDeployment.phase { Text("Retry") }
                            else { Text("Sign in to Apple Account") }
                        }
                        .buttonStyle(.borderedProminent)
                    }
                }
                Section {
                    DisclosureGroup("Other Options") {
                        VStack(alignment: .leading, spacing: 8) {
                            Button("Import Local DMG or Framework…") { importLocalItem() }
                            Button("Open Apple Downloads in Browser") {
                                NSWorkspace.shared.open(AppleDeveloperCatalog.downloadsPageURL)
                            }
                        }.padding(.top, 8)
                    }.disabled(d3dMetalDeployment.isActive)
                }
            }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)
            Divider()
            HStack {
                Spacer()
                if d3dMetalDeployment.isActive {
                    Button("Cancel") { Task { await d3dMetalDeployment.cancelAndWait() } }
                } else {
                    Button("Cancel", action: onDismiss).keyboardShortcut(.cancelAction)
                }
            }.padding(16)
        }
        .frame(width: 540, height: 580)
        .background(.regularMaterial)
        .interactiveDismissDisabled(d3dMetalDeployment.isActive)
        .sheet(isPresented: $d3dMetalDeployment.isPresentingLoginSheet, onDismiss: {
            if case .signingIn = d3dMetalDeployment.phase {
                Task { await d3dMetalDeployment.cancelAndWait() }
            }
        }) {
            AppleDeveloperLoginView(stagingDirectory: D3DMetalDeployment.downloadDirectory, deployment: d3dMetalDeployment) { event in
                switch event {
                case .authorizedForNativeDownload(let artifact, let authorization):
                    d3dMetalDeployment.handleAuthorizedDownload(artifact: artifact, authorization: authorization)
                case .downloadedToLocalFile(let url, let artifactName):
                    d3dMetalDeployment.handleWebKitDownloadedDMG(fileURL: url, artifactName: artifactName)
                case .cancelled:
                    Task { await d3dMetalDeployment.cancelAndWait() }
                case .failed(let message):
                    d3dMetalDeployment.isPresentingLoginSheet = false
                    d3dMetalDeployment.phase = .failed(message)
                }
            }
        }
    }

    @ViewBuilder private var progressArea: some View {
        switch d3dMetalDeployment.phase {
        case .signingIn:
            ProgressView("Signing in to Apple Account…")
        case .fetchingCatalog:
            ProgressView("Querying Apple Developer Downloads…")
        case .downloading(let name, let received, let total):
            Text(name).font(.callout)
            if let total, total > 0 {
                ProgressView(value: Double(received) / Double(total))
                Text("\(byteCount(received)) / \(byteCount(total))")
                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
            } else {
                ProgressView()
                Text(byteCount(received)).font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
        case .installing(let stage):
            ProgressView(stageTitle(stage))
        case .idle, .failed:
            EmptyView()
        }
    }

    private func byteCount(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    private func stageTitle(_ stage: D3DMetalInstallStage) -> String {
        switch stage {
        case .mounting: String(localized: "Mounting disk image…")
        case .extracting: String(localized: "Extracting D3DMetal…")
        case .verifying: String(localized: "Verifying D3DMetal…")
        case .installing: String(localized: "Installing D3DMetal…")
        }
    }

    private func importLocalItem() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Import Local DMG or Framework…")
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.treatsFilePackagesAsDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [UTType(filenameExtension: "dmg") ?? .diskImage,
                                     UTType(filenameExtension: "framework") ?? .bundle]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        d3dMetalDeployment.importLocalItem(at: url)
    }
}
