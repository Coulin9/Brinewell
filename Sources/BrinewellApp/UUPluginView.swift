import AppKit
import BrinewellCore
import CoreImage.CIFilterBuiltins
import SwiftUI

struct UUPluginView: View {
    @ObservedObject var vm: VMProcess

    var body: some View {
        Section("UU Game Booster") {
            Text(NSLocalizedString(vm.uu.message, comment: "SteamOS UU installation service message"))
                .foregroundStyle(vm.uu.phase == .failed ? .red : .secondary)
                .textSelection(.enabled)
            if vm.uu.phase == .installing {
                ProgressView(value: Double(vm.uu.progress), total: 100)
                Text("Keep the virtual machine running while Brinewell prepares dependencies and installs the plugin.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            HStack {
                if vm.uu.phase != .installed {
                    Button(vm.uu.phase == .failed ? String(localized: "Retry Installation") : String(localized: "Install UU")) { vm.installUU() }
                        .disabled(!vm.uu.canInstall || vm.state == .stopping)
                }
                Button("Refresh Status") { vm.refreshUUStatus() }
                    .disabled(!vm.uu.canRefresh || vm.state == .stopping)
            }
            if vm.uu.phase == .installed {
                if let code = vm.uu.bindingCode, let image = qrImage(code) {
                    Image(nsImage: image).interpolation(.none)
                        .resizable().scaledToFit().frame(width: 184, height: 184)
                        .padding(24).background(.white)
                        .accessibilityLabel("UU Device Pairing QR Code")
                    Text("Scan the code in the UU mobile app to pair this device. Select your game and enable acceleration before launching it.")
                        .font(.callout).foregroundStyle(.secondary)
                } else {
                    Text("The plugin is running. Click Refresh Status to see the pairing code when it is ready.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
        }
    }

    private func qrImage(_ code: String) -> NSImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(code.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage,
              let image = CIContext().createCGImage(output, from: output.extent) else { return nil }
        return NSImage(cgImage: image, size: NSSize(width: image.width, height: image.height))
    }
}
