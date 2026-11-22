import AppKit
import BrinewellVMRuntime
import Darwin
import Foundation

@main
struct RuntimeMain {
    @MainActor
    static func main() {
        signal(SIGPIPE, SIG_IGN)
        do {
            let arguments = Array(CommandLine.arguments.dropFirst())
            if arguments.first == "--prepare-steamos" {
                try DiskInstallerVM(arguments: arguments).run()
            }
            let options = try RuntimeOptions(arguments: arguments)
            do {
                let app = NSApplication.shared
                app.setActivationPolicy(.regular)
                let runtime = try Runtime(options: options)
                var signals: [DispatchSourceSignal] = []
                for number in [SIGTERM, SIGINT] {
                    signal(number, SIG_IGN)
                    let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
                    source.setEventHandler { runtime.shutdown() }
                    source.resume()
                    signals.append(source)
                }
                Task { @MainActor in
                    do { try await runtime.start() }
                    catch {
                        if error is CancellationError { exit(0) }
                        let message = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
                        report("BRINEWELL error \(message)")
                        exit(1)
                    }
                }
                app.run()
                withExtendedLifetime((runtime, signals)) {}
            } catch {
                close(options.diskFD)
                throw error
            }
        } catch {
            let message = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
            report("BRINEWELL error \(message)")
            exit(1)
        }
    }
}
