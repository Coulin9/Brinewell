import Darwin
import Foundation

/// Captures native VMM and inherited renderer output as well as runtime reports.
/// The launcher owns one instance for the lifetime of its in-process VM.
public final class RuntimeLog {
    private let savedOutput: Int32
    private let savedError: Int32
    private var restored = false

    public init(url: URL) throws {
        let log = open(url.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o600)
        guard log >= 0 else { throw Self.failure("cannot open host log") }
        defer { Darwin.close(log) }
        let output = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3)
        guard output >= 0 else { throw Self.failure("cannot save stdout") }
        let error = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)
        guard error >= 0 else {
            let failure = Self.failure("cannot save stderr")
            Darwin.close(output)
            throw failure
        }
        fflush(nil)
        guard dup2(log, STDOUT_FILENO) >= 0, dup2(log, STDERR_FILENO) >= 0 else {
            let failure = Self.failure("cannot redirect host log")
            _ = dup2(output, STDOUT_FILENO)
            _ = dup2(error, STDERR_FILENO)
            Darwin.close(output)
            Darwin.close(error)
            throw failure
        }
        savedOutput = output
        savedError = error
    }

    public func restore() {
        guard !restored else { return }
        fflush(nil)
        _ = dup2(savedOutput, STDOUT_FILENO)
        _ = dup2(savedError, STDERR_FILENO)
        Darwin.close(savedOutput)
        Darwin.close(savedError)
        restored = true
    }

    public func close() { restore() }

    deinit { restore() }

    private static func failure(_ operation: String) -> NSError {
        NSError(domain: NSPOSIXErrorDomain, code: Int(errno),
                userInfo: [NSLocalizedDescriptionKey: "\(operation): \(String(cString: strerror(errno)))"])
    }
}
