import BrinewellCore
import Darwin
import Foundation

private struct GraphicsDiagnostics: Encodable {
    let backend: String
    let frameworkPath: String?
    let frameworkVersion: String?
    let frameworkArchitectures: [String]
    let adapterArchitectures: [String]
    let serverArchitectures: [String]
    let rosettaAvailable: Bool
    let reason: String?
}

private struct Diagnostics: Encodable {
    let host: HostCapabilities
    let configuration: VMConfiguration
    let configurationPath: String
    let runtime: String
    let graphics: GraphicsDiagnostics
}

private func architectureList(_ url: URL) -> [String] {
    guard let architectures = try? MachOArchitecture.architectures(at: url) else { return [] }
    return architectures.sorted()
}

private func frameworkBinary(_ path: String?) -> URL? {
    guard let path, path.hasPrefix("/") else { return nil }
    let url = URL(fileURLWithPath: path)
    var isDirectory: ObjCBool = false
    guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else { return nil }
    if isDirectory.boolValue {
        for candidate in [url.appendingPathComponent("Versions/A/D3DMetal"), url.appendingPathComponent("D3DMetal")]
            where FileManager.default.isReadableFile(atPath: candidate.path) {
            return candidate
        }
        return nil
    }
    return url
}

private func frameworkVersion(_ path: String?) -> String? {
    guard let path, path.hasPrefix("/") else { return nil }
    let url = URL(fileURLWithPath: path)
    for candidate in [url.appendingPathComponent("Versions/A/Resources/Info.plist"),
                      url.appendingPathComponent("Resources/Info.plist")]
        where FileManager.default.isReadableFile(atPath: candidate.path) {
        if let data = try? Data(contentsOf: candidate),
           let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] {
            return plist["CFBundleShortVersionString"] as? String
        }
    }
    return nil
}

private func rosettaAvailable() -> Bool {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/arch")
    process.arguments = ["-x86_64", "/usr/bin/true"]
    process.standardOutput = FileHandle.nullDevice
    process.standardError = FileHandle.nullDevice
    do {
        try process.run()
        process.waitUntilExit()
        return process.terminationStatus == 0
    } catch { return false }
}

private let usage = "Usage: brinewell-diagnostics [--help]"
private let arguments = Array(CommandLine.arguments.dropFirst())

if arguments == ["--help"] {
    print(usage)
} else if !arguments.isEmpty {
    FileHandle.standardError.write(Data("\(usage)\n".utf8))
    exit(2)
} else {
    do {
        let host = HostCapabilities.detect()
        let url = try ConfigurationStore.defaultURL()
        let configuration = try ConfigurationStore(url: url).load(for: host)
        let executable = Bundle.main.executableURL ?? URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL
        let contents = executable.deletingLastPathComponent().deletingLastPathComponent()
        let helper = contents.appendingPathComponent("MacOS/brinewell-vm")
        let resources = contents.appendingPathComponent("Resources/Runtime")
        let renderer = contents.appendingPathComponent("Helpers/virgl_render_server")
        let driver = contents.appendingPathComponent("Frameworks/libvulkan_kosmickrisp.dylib")
        let available = FileManager.default.isExecutableFile(atPath: helper.path)
            && ["Image", "initramfs.cpio.gz", "brinewell-layer.img", "gvproxy"].allSatisfy {
                FileManager.default.isReadableFile(atPath: resources.appendingPathComponent($0).path)
            }
            && FileManager.default.isExecutableFile(atPath: resources.appendingPathComponent("gvproxy").path)
            && FileManager.default.isExecutableFile(atPath: renderer.path)
            && (try? renderer.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
            && FileManager.default.isReadableFile(atPath: driver.path)
            && (try? driver.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
        let useMullion = configuration.graphicsBackend == .mullion
        let adapterName = useMullion ? "libmullion-native.dylib" : "libd3dmetal-native.dylib"
        let adapter = contents.appendingPathComponent("Frameworks/\(adapterName)")
        let d3dmetalPath = useMullion ? nil : configuration.d3dMetalFrameworkPath
        let framework = frameworkBinary(d3dmetalPath)
        let graphics = GraphicsDiagnostics(
            backend: configuration.graphicsBackend.rawValue,
            frameworkPath: framework?.path,
            frameworkVersion: frameworkVersion(d3dmetalPath),
            frameworkArchitectures: framework.map(architectureList) ?? [],
            adapterArchitectures: architectureList(adapter),
            serverArchitectures: architectureList(renderer),
            rosettaAvailable: rosettaAvailable(),
            reason: configuration.graphicsBackend == .venus ? "disabled by configuration" : nil
        )
        let diagnostics = Diagnostics(
            host: host,
            configuration: configuration,
            configurationPath: url.path,
            runtime: available ? "available (bundled helpers and resources present)" : "unavailable (bundled helpers or resources missing)",
            graphics: graphics
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(diagnostics)
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data("\n".utf8))
    } catch {
        FileHandle.standardError.write(Data("brinewell-diagnostics: \(error.localizedDescription)\n".utf8))
        exit(1)
    }
}
