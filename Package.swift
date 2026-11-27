// swift-tools-version: 6.0
import Foundation
import PackageDescription

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
let environment = ProcessInfo.processInfo.environment
let include = environment["BRINEWELL_LIBKRUN_INCLUDE"] ?? root.appendingPathComponent("libkrun/include").path
let frameworks = environment["BRINEWELL_LIBKRUN_DIR"] ?? root.appendingPathComponent("Brinewell/.build/out/libkrun").path

let package = Package(
    name: "Brinewell",
    platforms: [.macOS("26.0")],
    products: [
        .library(name: "BrinewellCore", targets: ["BrinewellCore"]),
        .library(name: "BrinewellDisplay", targets: ["BrinewellDisplay"]),
        .library(name: "BrinewellVMRuntime", targets: ["BrinewellVMRuntime"]),
        .executable(name: "Brinewell", targets: ["BrinewellApp"]),
        .executable(name: "brinewell-vm", targets: ["BrinewellVM"]),
        .executable(name: "brinewell-diagnostics", targets: ["BrinewellDiagnostics"]),
    ],
    targets: [
        .systemLibrary(name: "CKrun"),
        .target(name: "BrinewellCore", linkerSettings: [.linkedFramework("Metal"), .linkedFramework("AppKit")]),
        .target(name: "BrinewellDisplay", linkerSettings: [
            .linkedFramework("AppKit"), .linkedFramework("Metal"), .linkedFramework("MetalKit"),
            .linkedFramework("IOSurface"), .linkedFramework("QuartzCore"),
        ]),
        // The VM runtime (libkrun VMM + guest window/input/network) lives in a
        // library so both the launcher (in-process VM) and the standalone
        // brinewell-vm executable can run it. The launcher runs the VM in its
        // own process so game mode binds to its CFBundleExecutable.
        .target(name: "BrinewellVMRuntime", dependencies: ["CKrun", "BrinewellCore", "BrinewellDisplay"],
                swiftSettings: [.unsafeFlags(["-Xcc", "-I\(include)"])],
                linkerSettings: [.unsafeFlags(["-L", frameworks, "-lkrun.1", "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks", "-Xlinker", "-rpath", "-Xlinker", frameworks]), .linkedFramework("AppKit"), .linkedFramework("GameController"), .linkedFramework("SystemConfiguration")]),
        .executableTarget(name: "BrinewellApp", dependencies: ["BrinewellCore", "BrinewellVMRuntime"],
                          swiftSettings: [.unsafeFlags(["-Xcc", "-I\(include)"])],
                          linkerSettings: [.unsafeFlags(["-L", frameworks, "-lkrun.1", "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks", "-Xlinker", "-rpath", "-Xlinker", frameworks]), .linkedFramework("SwiftUI"), .linkedFramework("AppKit"), .linkedFramework("GameController"), .linkedFramework("WebKit")]),
        .executableTarget(name: "BrinewellVM", dependencies: ["BrinewellVMRuntime"],
                          swiftSettings: [.unsafeFlags(["-Xcc", "-I\(include)"])],
                          linkerSettings: [.unsafeFlags(["-L", frameworks, "-lkrun.1", "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks", "-Xlinker", "-rpath", "-Xlinker", frameworks]), .linkedFramework("AppKit"), .linkedFramework("GameController")]),
        .executableTarget(name: "BrinewellDiagnostics", dependencies: ["BrinewellCore"]),
        .testTarget(name: "BrinewellCoreTests", dependencies: ["BrinewellCore"]),
        .testTarget(name: "BrinewellDisplayTests", dependencies: ["BrinewellDisplay"]),
        .testTarget(name: "BrinewellVMTests", dependencies: ["BrinewellVMRuntime"],
                    swiftSettings: [.unsafeFlags(["-Xcc", "-I\(include)"])],
                    linkerSettings: [.unsafeFlags(["-L", frameworks, "-Xlinker", "-rpath", "-Xlinker", frameworks])]),
    ],
    swiftLanguageModes: [.v6]
)
