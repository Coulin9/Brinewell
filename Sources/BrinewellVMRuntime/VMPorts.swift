import BrinewellCore
import CKrun
import Darwin
import Foundation

@MainActor
final class VMPorts {
    private let runDirectory: URL
    private let padWriter: (([UInt8]) -> Int)?
    private let graphicsWriter: (([UInt8]) -> Int)?
    private let graphicsResolver: any GameGraphicsBackendResolving
    private let availableBackends: Set<GraphicsBackend>
    private var graphicsGeneration: UInt64 = 0
    private var graphicsResolutions: [String: Task<Void, Never>] = [:]
    private var descriptors: [Int32] = []
    private var master: Int32 = -1
    private var slave: Int32 = -1
    private var progressRead: Int32 = -1
    private var progressInput: Int32 = -1
    private var progressOutput: Int32 = -1
    private var padRead: Int32 = -1
    private var padWrite: Int32 = -1
    private var padInput: Int32 = -1
    private var padOutput: Int32 = -1
    private var uuRead: Int32 = -1
    private var uuWrite: Int32 = -1
    private var uuInput: Int32 = -1
    private var uuOutput: Int32 = -1
    private var uuLine = [UInt8]()
    private var uuOverflow = false
    private var uuReady = false
    private var graphicsRead: Int32 = -1
    private var graphicsWrite: Int32 = -1
    private var graphicsInput: Int32 = -1
    private var graphicsOutput: Int32 = -1
    private var graphicsLine = [UInt8]()
    private var graphicsOverflow = false
    private var graphicsPending = [UInt8]()
    private var graphicsFramePartial = false
    private var graphicsWriteSource: DispatchSourceWrite?
    private var graphicsReady = false
    // The host owns the desired preference: retained for guest reconnects and
    // early UI changes, and never marked applied on a stale acknowledgement.
    private var graphicsDesired: GraphicsBackend?
    private var graphicsApplied: GraphicsBackend?
    private var desiredDisplayHDR: String?
    var onGraphicsApplied: ((GraphicsBackend) -> Void)?
    var onGraphicsRequest: ((GraphicsBackend) throws -> Void)?
    private var telemetryRead: Int32 = -1
    private var telemetryWrite: Int32 = -1
    private var telemetryInput: Int32 = -1
    private var telemetryOutput: Int32 = -1
    private var clipboardRead: Int32 = -1
    private var clipboardWrite: Int32 = -1
    private var clipboardInput: Int32 = -1
    private var clipboardOutput: Int32 = -1
    private var clipboardWriteSource: DispatchSourceWrite?
    private var clipboardPending = [UInt8]()
    private var clipboardOffset = 0
    private lazy var clipboard = VMClipboard { [weak self] bytes in
        guard let self else { return }
        self.clipboardPending = bytes
        self.clipboardOffset = 0
        self.flushClipboard()
    }
    private var telemetryLine = [UInt8]()
    private var telemetryOverflow = false
    private var telemetryReplyInterrupted = false
    private var telemetry: HostHardwareTelemetry?
    private var ownsFIFO = false
    private var fifoRead: Int32 = -1
    private var consoleLog: Int32 = -1
    private var progressLog: Int32 = -1
    private var sources: [DispatchSourceRead] = []
    private var writeSource: DispatchSourceWrite?
    private var pending = [UInt8]()
    private var progressLine = [UInt8]()
    private var padLine = [UInt8]()
    private var fifoLine = [UInt8]()
    private var progressOverflow = false
    private var padOverflow = false
    private var fifoOverflow = false
    private var ready = false
    private var physicalPending: GamepadState?
    private var physicalResetPending = false
    private(set) lazy var input = GamepadInputState(
        emit: { [weak self] state, reset in self?.queuePhysical(state, reset: reset) },
        sendScript: { [weak self] line, releasing in self?.send(line, releasing: releasing) ?? false },
        cancelRelease: { [weak self] in self?.release?.cancel(); self?.release = nil },
        physicalPending: { [weak self] in
            guard let self else { return false }
            return self.physicalPending != nil || self.physicalResetPending
        }
    )
    private lazy var physicalGamepad = PhysicalGamepad(input: input)
    private var release: DispatchWorkItem?
    private var closed = false
    private var closing = false
    private var onReady: (() -> Void)?
    private var onPointerFocus: ((String) -> Void)?
    private var onGameFrame: ((GameFrameSample) -> Void)?
    private var onShutdown: (() -> Void)?
    private var onFrameDump: ((String) throws -> Void)?
    private var onFullScreen: ((Bool) throws -> Void)?
    private static let maxLine = 65536
    private static let maxPending = 65536

    private static func error(_ message: String) -> NSError {
        NSError(domain: "Brinewell.VMPorts", code: Int(errno), userInfo: [NSLocalizedDescriptionKey: message])
    }

    init(runDirectory: URL, padWriter: (([UInt8]) -> Int)? = nil,
         graphicsWriter: (([UInt8]) -> Int)? = nil,
         graphicsResolver: any GameGraphicsBackendResolving = PCGamingWikiResolver(),
         availableBackends: Set<GraphicsBackend> = [.venus]) throws {
        self.runDirectory = runDirectory
        self.padWriter = padWriter
        self.graphicsWriter = graphicsWriter
        self.graphicsResolver = graphicsResolver
        self.availableBackends = availableBackends
        do {
            var m: Int32 = -1, s: Int32 = -1
            guard openpty(&m, &s, nil, nil, nil) == 0 else { throw Self.error("openpty failed") }
            descriptors += [m, s]; master = m; slave = s
            var term = termios()
            guard tcgetattr(s, &term) == 0 else { throw Self.error("tcgetattr failed") }
            cfmakeraw(&term)
            guard tcsetattr(s, TCSANOW, &term) == 0 else { throw Self.error("tcsetattr failed") }
            let progress = try makePort()
            progressRead = progress.0; progressOutput = progress.1
            progressInput = progress.2
            let pad = try makePort()
            padRead = pad.0; padOutput = pad.1; padInput = pad.2; padWrite = pad.3
            let uu = try makePort()
            (uuRead, uuOutput, uuInput, uuWrite) = uu
            (graphicsRead, graphicsOutput, graphicsInput, graphicsWrite) = try makePort()
            let telemetry = try makePort()
            (telemetryRead, telemetryOutput, telemetryInput, telemetryWrite) = telemetry
            (clipboardRead, clipboardOutput, clipboardInput, clipboardWrite) = try makePort()
            for fd in descriptors {
                guard fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 else { throw Self.error("fcntl CLOEXEC failed") }
            }
            for fd in [master, progressRead, padRead, padWrite, uuRead, uuWrite, telemetryRead, telemetryWrite] { try nonblocking(fd) }
            try nonblocking(graphicsRead)
            try nonblocking(graphicsWrite)
            try nonblocking(clipboardRead)
            try nonblocking(clipboardWrite)
        } catch {
            for fd in descriptors { Darwin.close(fd) }
            descriptors.removeAll()
            throw error
        }
    }

    deinit {
        for fd in descriptors { Darwin.close(fd) }
    }

    private func makePort() throws -> (Int32, Int32, Int32, Int32) {
        var out: [Int32] = [-1, -1], input: [Int32] = [-1, -1]
        guard pipe(&out) == 0 else { throw Self.error("pipe failed") }
        descriptors += out
        guard pipe(&input) == 0 else { throw Self.error("pipe failed") }
        descriptors += input
        return (out[0], out[1], input[0], input[1])
    }

    private func nonblocking(_ fd: Int32) throws {
        let flags = fcntl(fd, F_GETFL)
        guard flags >= 0, fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 else {
            throw Self.error("fcntl nonblocking failed")
        }
    }

    func attach(context: UInt32) throws {
        func check(_ result: Int32, _ name: String) throws {
            guard result >= 0 else { throw Self.error("\(name) failed: \(result)") }
        }
        try check(krun_disable_implicit_console(context), "krun_disable_implicit_console")
        let console = krun_add_virtio_console_multiport(context)
        try check(console, "krun_add_virtio_console_multiport")
        try check(krun_add_console_port_tty(context, UInt32(console), "", slave), "hvc0")
        try check(krun_add_console_port_inout(context, UInt32(console), "fx.progress", progressInput, progressOutput), "fx.progress")
        try check(krun_add_console_port_inout(context, UInt32(console), "fx.pad", padInput, padOutput), "fx.pad")
        try check(krun_add_console_port_inout(context, UInt32(console), "fx.telemetry", telemetryInput, telemetryOutput), "fx.telemetry")
        try check(krun_add_console_port_inout(context, UInt32(console), "fx.uu", uuInput, uuOutput), "fx.uu")
        try check(krun_add_console_port_inout(context, UInt32(console), "fx.graphics",
                                              graphicsInput, graphicsOutput), "fx.graphics")
        try check(krun_add_console_port_inout(context, UInt32(console), "fx.clipboard",
                                              clipboardInput, clipboardOutput), "fx.clipboard")
    }

    func startTelemetry(sampler: URL) {
        guard FileManager.default.isExecutableFile(atPath: sampler.path) else { return }
        let telemetry = HostHardwareTelemetry()
        do {
            try telemetry.start(executable: sampler)
            try watch(telemetryRead) { [weak self] bytes in self?.readTelemetry(bytes) }
            self.telemetry = telemetry
        } catch {
            telemetry.stop()
            report("BRINEWELL telemetry unavailable: \(error)")
        }
    }

    private func readTelemetry(_ bytes: [UInt8]) {
        for line in Self.lines(bytes, buffer: &telemetryLine, overflow: &telemetryOverflow) {
            let words = line.split(separator: " ")
            guard words.count == 2, words[0] == "hardware", let id = UInt64(words[1]),
                  let sample = telemetry?.sample() else { continue }
            // A long reply can be partially written. Terminate it before sending a fresh sample.
            let delimiter = telemetryReplyInterrupted ? "\n" : ""
            let reply = Array("\(delimiter)hardware \(id) \(sample.wireValues)\n".utf8)
            let written = reply.withUnsafeBytes { Darwin.write(telemetryWrite, $0.baseAddress, $0.count) }
            if written > 0 { telemetryReplyInterrupted = written != reply.count }
        }
    }

    func hardwareSample() -> HostHardwareTelemetry.Snapshot? { telemetry?.sample() }

    func stopTelemetry() {
        telemetry?.stop()
        telemetry = nil
    }

    func start(onReady: @escaping () -> Void, onShutdown: @escaping () -> Void,
               onFrameDump: @escaping (String) throws -> Void,
               onPointerFocus: @escaping (String) -> Void,
               onFullScreen: ((Bool) throws -> Void)? = nil,
               onGameFrame: ((GameFrameSample) -> Void)? = nil) throws {
        self.onReady = onReady; self.onShutdown = onShutdown; self.onFrameDump = onFrameDump
        self.onPointerFocus = onPointerFocus
        self.onFullScreen = onFullScreen
        self.onGameFrame = onGameFrame
        let fifo = runDirectory.appendingPathComponent("control.fifo").path
        guard mkfifo(fifo, 0o600) == 0 else { throw Self.error("cannot create control.fifo") }
        ownsFIFO = true
        fifoRead = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC)
        guard fifoRead >= 0 else { throw Self.error("cannot open control.fifo") }
        descriptors.append(fifoRead)
        let dummy = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC)
        guard dummy >= 0 else { throw Self.error("cannot open control.fifo writer") }
        descriptors.append(dummy)
        consoleLog = try openLog("console.log")
        progressLog = try openLog("progress.log")
        try watch(master) { [weak self] bytes in self?.writeLog(self?.consoleLog ?? -1, bytes) }
        try watch(progressRead) { [weak self] bytes in self?.readProgress(bytes) }
        try watch(padRead) { [weak self] bytes in self?.readPad(bytes) }
        try watch(uuRead) { [weak self] bytes in self?.readUU(bytes) }
        try watch(graphicsRead) { [weak self] bytes in self?.readGraphics(bytes) }
        try watch(clipboardRead) { [weak self] bytes in self?.clipboard.receive(bytes) }
        try watch(fifoRead) { [weak self] bytes in self?.readFIFO(bytes) }
        physicalGamepad.start()
    }

    private func openLog(_ name: String) throws -> Int32 {
        let fd = open(runDirectory.appendingPathComponent(name).path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw Self.error("cannot open \(name)") }
        descriptors.append(fd)
        return fd
    }

    private func duplicateForSource(_ fd: Int32) throws -> Int32 {
        let duplicate = dup(fd)
        guard duplicate >= 0 else { throw Self.error("dup failed") }
        guard fcntl(duplicate, F_SETFD, FD_CLOEXEC) == 0 else {
            let error = Self.error("fcntl CLOEXEC failed")
            Darwin.close(duplicate)
            throw error
        }
        return duplicate
    }

    private func watch(_ fd: Int32, consume: @escaping ([UInt8]) -> Void) throws {
        let duplicate = try duplicateForSource(fd)
        let source = DispatchSource.makeReadSource(fileDescriptor: duplicate, queue: .main)
        source.setEventHandler { [weak self] in
            guard let self, !self.closed else { return }
            var buffer = [UInt8](repeating: 0, count: 4096)
            let count = Darwin.read(duplicate, &buffer, buffer.count)
            if count > 0 { consume(Array(buffer[..<count])) }
        }
        source.setCancelHandler { Darwin.close(duplicate) }
        sources.append(source)
        source.resume()
    }

    private func writeLog(_ fd: Int32, _ bytes: [UInt8]) {
        var offset = 0
        while offset < bytes.count {
            let n = bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress!.advanced(by: offset), bytes.count - offset) }
            if n > 0 { offset += n } else if n < 0 && errno == EINTR { continue } else { break }
        }
    }
    private static func lines(_ bytes: [UInt8], buffer: inout [UInt8], overflow: inout Bool) -> [String] {
        var result: [String] = []
        for byte in bytes {
            if byte == 10 {
                if !overflow { result.append(String(decoding: buffer, as: UTF8.self)) }
                buffer.removeAll(keepingCapacity: true); overflow = false
            } else if !overflow {
                if buffer.count == Self.maxLine { buffer.removeAll(); overflow = true }
                else { buffer.append(byte) }
            }
        }
        return result
    }

    func readProgress(_ bytes: [UInt8]) {
        guard !closed else { return }
        for line in Self.lines(bytes, buffer: &progressLine, overflow: &progressOverflow) {
            guard !closed else { return }
            // Present telemetry is transient: do not turn it into a permanent
            // four-times-per-second log stream for every game swapchain.
            if line.hasPrefix("game-frame") {
                if let sample = GameFrameSample(line: line) { onGameFrame?(sample) }
                continue
            }
            writeLog(progressLog, Array((line + "\n").utf8))
            if line == "ready" { onReady?() }
            else if line.hasPrefix("focus ") { onPointerFocus?(line) }
        }
    }

    enum UUCommand: String {
        case install, status
    }

    func sendUU(_ command: UUCommand) throws {
        guard !closed, uuReady else { throw RuntimeError(String(localized: "The UU installation service is not ready. Wait for SteamOS to start.")) }
        // Both commands fit within PIPE_BUF, so a nonblocking write is atomic.
        let bytes = Array((command.rawValue + "\n").utf8)
        var count: Int
        repeat {
            count = bytes.withUnsafeBytes { Darwin.write(uuWrite, $0.baseAddress!, $0.count) }
        } while count < 0 && errno == EINTR
        guard count == bytes.count else { throw RuntimeError(String(localized: "Could not send the UU installation request. Try again later.")) }
    }

    func readUU(_ bytes: [UInt8]) {
        guard !closed else { return }
        for line in Self.lines(bytes, buffer: &uuLine, overflow: &uuOverflow) {
            if line == "hello" { uuReady = true }
            report("BRINEWELL uu \(line)")
        }
    }

    /// Records the desired backend and forwards it once the guest service is
    /// connected. A later ack only counts when it matches the newest desired.
    func setGraphicsBackend(_ backend: GraphicsBackend) throws {
        guard !closed else { throw RuntimeError(String(localized: "The SteamOS connection is closed.")) }
        graphicsDesired = backend
        sendGraphicsBackend()
    }

    private func sendGraphicsBackend() {
        guard graphicsReady, let backend = graphicsDesired else { return }
        enqueueGraphicsControl("backend \(backend.rawValue)\n")
    }

    // Control frames precede queued resolutions, but never interrupt a frame
    // whose prefix has already been written to the stream.
    private func enqueueGraphicsControl(_ line: String) {
        var index = 0
        if graphicsFramePartial {
            guard let end = graphicsPending.firstIndex(of: 10) else { return }
            index = end + 1
        }
        while index < graphicsPending.count {
            if graphicsPending[index...].starts(with: "resolved ".utf8) { break }
            guard let end = graphicsPending[index...].firstIndex(of: 10) else { break }
            index = end + 1
        }
        graphicsPending.insert(contentsOf: line.utf8, at: index)
        flushGraphics()
    }

    public func sendDisplayHDR(supported: Bool, maxLuminanceNits: UInt16,
        maxFrameAverageLuminanceNits: UInt16, minLuminance10000ths: UInt16, sdrReferenceNits: UInt16) {
        guard !closed else { return }
        let line = "hdr \(supported ? 1 : 0) \(maxLuminanceNits) \(maxFrameAverageLuminanceNits) \(minLuminance10000ths) \(sdrReferenceNits)\n"
        guard line != desiredDisplayHDR else { return }
        desiredDisplayHDR = line
        if graphicsReady { enqueueGraphicsControl(line) }
    }

    func flushGraphics() {
        while !graphicsPending.isEmpty {
            let written = graphicsWriter?(graphicsPending) ?? graphicsPending.withUnsafeBytes {
                Darwin.write(graphicsWrite, $0.baseAddress!, $0.count)
            }
            if written > 0 {
                graphicsFramePartial = graphicsPending[written - 1] != 10
                graphicsPending.removeFirst(written)
            } else if written < 0 && errno == EINTR {
                continue
            } else if written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) {
                if graphicsWriter == nil, graphicsWriteSource == nil, let duplicate = try? duplicateForSource(graphicsWrite) {
                    let source = DispatchSource.makeWriteSource(fileDescriptor: duplicate, queue: .main)
                    source.setEventHandler { [weak self] in
                        guard let self, !self.closed else { return }
                        self.flushGraphics()
                    }
                    source.setCancelHandler { Darwin.close(duplicate) }
                    graphicsWriteSource = source
                    source.resume()
                }
                return
            } else {
                report("BRINEWELL graphics error: cannot write backend request")
                graphicsPending.removeAll()
                graphicsFramePartial = false
                graphicsReady = false
                resetGraphicsResolutions()
                graphicsWriteSource?.cancel()
                graphicsWriteSource = nil
                return
            }
        }
        graphicsWriteSource?.cancel()
        graphicsWriteSource = nil
    }

    private func resetGraphicsResolutions() {
        graphicsGeneration &+= 1
        for task in graphicsResolutions.values { task.cancel() }
        graphicsResolutions.removeAll()
    }

    private func resolveGraphics(_ line: String) {
        let words = line.split(separator: " ", omittingEmptySubsequences: false)
        guard words.count == 3, words[0] == "resolve" else {
            report("BRINEWELL graphics invalid request: \(line)"); return
        }
        let id = String(words[1]), appText = String(words[2])
        guard id.utf8.count == 32, id.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
              !appText.isEmpty, appText.utf8.allSatisfy({ (48...57).contains($0) }),
              let appID = UInt32(appText), appID > 0, String(appID) == appText else {
            report("BRINEWELL graphics invalid request: \(line)"); return
        }
        guard graphicsReady, !closed else { return }
        guard graphicsResolutions[id] == nil else { return }
        if graphicsDesired != .automatic {
            let desired = graphicsDesired?.rawValue ?? "unset"
            if enqueueGraphicsResolution(id: id, backend: "venus") {
                report("BRINEWELL graphics resolved app=\(appID) backend=\(GraphicsBackend.venus.rawValue) mode=\(desired)")
            }
            return
        }
        guard graphicsResolutions.count < 16 else { return }
        let generation = graphicsGeneration
        let resolver = graphicsResolver, available = availableBackends
        graphicsResolutions[id] = Task { [weak self] in
            let resolution = await resolver.resolveForGuest(appID: appID, availableBackends: available)
            guard let self, !self.closed, self.graphicsReady, self.graphicsGeneration == generation,
                  self.graphicsResolutions.removeValue(forKey: id) != nil else { return }
            let wire: String
            switch resolution {
            case .concrete(let backend):
                wire = backend.isConcrete && available.contains(backend) ? backend.rawValue : "venus"
            case .probeLocal(let allowMullion):
                wire = allowMullion && available.contains(.mullion) ? "probe" : "probe_nomullion"
            }
            if self.enqueueGraphicsResolution(id: id, backend: wire) {
                report("BRINEWELL graphics resolved app=\(appID) backend=\(wire)")
            }
        }
    }

    private func enqueueGraphicsResolution(id: String, backend: String) -> Bool {
        let frame = Array("resolved \(id) \(backend)\n".utf8)
        // Count a partial first frame conservatively, since its prefix is gone.
        var resolutionBytes = 0
        var start = 0
        while start < graphicsPending.count, let end = graphicsPending[start...].firstIndex(of: 10) {
            if (start == 0 && graphicsFramePartial) || graphicsPending[start...].starts(with: "resolved ".utf8) {
                resolutionBytes += end - start + 1
            }
            start = end + 1
        }
        guard resolutionBytes + frame.count <= 4096 else { return false }
        graphicsPending.append(contentsOf: frame)
        flushGraphics()
        return true
    }

    func readGraphics(_ bytes: [UInt8]) {
        guard !closed else { return }
        for line in Self.lines(bytes, buffer: &graphicsLine, overflow: &graphicsOverflow) {
            if line == "ready" {
                resetGraphicsResolutions()
                if graphicsFramePartial, let end = graphicsPending.firstIndex(of: 10) {
                    graphicsPending.removeSubrange((end + 1)..<graphicsPending.count)
                } else {
                    graphicsPending.removeAll()
                    graphicsFramePartial = false
                }
                graphicsReady = true
                sendGraphicsBackend()
                if graphicsReady, let line = desiredDisplayHDR { enqueueGraphicsControl(line) }
                if graphicsPending.isEmpty { flushGraphics() }
                continue
            }
            if line == "applied hdr" { continue }
            if line == "resolve" || line.hasPrefix("resolve ") { resolveGraphics(line); continue }
            let words = line.split(separator: " ")
            if words.count == 2, words[0] == "applied",
               let mode = GraphicsBackend(rawValue: String(words[1])) {
                guard graphicsReady, mode == graphicsDesired else {
                    report("BRINEWELL graphics ignoring stale ack: \(line)")
                    continue
                }
                graphicsApplied = mode
                report("BRINEWELL graphics applied \(mode.rawValue)")
                onGraphicsApplied?(mode)
            } else {
                report("BRINEWELL graphics \(line)")
            }
        }
    }

    func readPad(_ bytes: [UInt8]) {
        guard !closed else { return }
        for line in Self.lines(bytes, buffer: &padLine, overflow: &padOverflow) {
            guard !closed else { return }
            guard line == "hello" else { continue } // Rumble is intentionally unsupported.
            ready = false
            _ = input.resetScript()
            physicalPending = nil
            physicalResetPending = false
            guard send("create 0003 045e 028e 0114 304,305,307,308,310,311,314,315,316,317,318 0:-32768:32767:16:128,1:-32768:32767:16:128,2:0:255:0:0,3:-32768:32767:16:128,4:-32768:32767:16:128,5:0:255:0:0,16:-1:1:0:0,17:-1:1:0:0 Microsoft X-Box 360 pad") else {
                report("BRINEWELL pad error: cannot queue create")
                continue
            }
            guard !closed else { return }
            ready = true
            input.synchronize()
        }
    }

    private func readFIFO(_ bytes: [UInt8]) {
        let wasOverflow = fifoOverflow
        let commands = Self.lines(bytes, buffer: &fifoLine, overflow: &fifoOverflow)
        if !wasOverflow && fifoOverflow { reply("error command too long") }
        for command in commands {
            guard !closed else { return }
            control(command)
        }
    }

    private func send(_ line: String, releasing: Bool = false) -> Bool {
        guard !closed else { return false }
        let bytes = Array((line + "\n").utf8)
        let limit = releasing ? Self.maxPending : Self.maxPending - 1024
        guard pending.count + bytes.count <= limit else { return false }
        pending.append(contentsOf: bytes)
        flush()
        return true
    }

    func queuePhysical(_ state: GamepadState, reset: Bool) {
        guard ready, !closed else { return }
        if reset { physicalResetPending = true }
        physicalPending = state
        flush()
    }

    func setInputActive(_ active: Bool) {
        guard !closed else { return }
        physicalGamepad.setActive(active)
        clipboard.setActive(active)
    }

    private func flushClipboard() {
        while clipboardOffset < clipboardPending.count {
            let written = clipboardPending.withUnsafeBytes {
                Darwin.write(clipboardWrite, $0.baseAddress!.advanced(by: clipboardOffset),
                             $0.count - clipboardOffset)
            }
            if written > 0 {
                clipboardOffset += written
            } else if written < 0 && errno == EINTR {
                continue
            } else if written < 0 && errno == EAGAIN {
                if clipboardWriteSource != nil { return }
                if let duplicate = try? duplicateForSource(clipboardWrite) {
                    let source = DispatchSource.makeWriteSource(fileDescriptor: duplicate, queue: .main)
                    source.setEventHandler { [weak self] in
                        guard let self, !self.closed else { return }
                        self.flushClipboard()
                    }
                    source.setCancelHandler { Darwin.close(duplicate) }
                    clipboardWriteSource = source
                    source.resume()
                    return
                }
                break
            } else {
                break
            }
        }
        clipboardPending.removeAll()
        clipboardOffset = 0
        clipboardWriteSource?.cancel()
        clipboardWriteSource = nil
    }

    func flush() {
        guard !closed else { return }
        while !pending.isEmpty || physicalPending != nil || physicalResetPending {
            if pending.isEmpty, physicalResetPending {
                physicalResetPending = false
                if physicalPending == GamepadState() { physicalPending = nil }
                pending = Array((GamepadState().frame + "\n").utf8)
            } else if pending.isEmpty, let state = physicalPending {
                physicalPending = nil
                pending = Array((state.frame + "\n").utf8)
            }
            let n = padWriter?(pending) ?? pending.withUnsafeBytes {
                Darwin.write(padWrite, $0.baseAddress!, $0.count)
            }
            if n > 0 { pending.removeFirst(n) }
            else if n < 0 && errno == EINTR { continue }
            else { break }
        }
        if pending.isEmpty {
            writeSource?.cancel(); writeSource = nil
        } else if writeSource == nil {
            let duplicate: Int32
            do {
                duplicate = try duplicateForSource(padWrite)
            } catch {
                report("BRINEWELL pad error: \(error)")
                onShutdown?()
                return
            }
            let source = DispatchSource.makeWriteSource(fileDescriptor: duplicate, queue: .main)
            source.setEventHandler { [weak self] in
                guard let self, !self.closed else { return }
                self.flush()
            }
            source.setCancelHandler { Darwin.close(duplicate) }
            writeSource = source; source.resume()
        }
    }

    private func reply(_ text: String) { report("BRINEWELL control \(text)") }

    func control(_ command: String) {
        guard !closed else { return }
        if command.hasPrefix("frame-dump ") {
            let path = String(command.dropFirst("frame-dump ".count))
            guard path.hasPrefix("/"), !path.contains("\0") else { reply("error invalid frame-dump path"); return }
            guard let onFrameDump else { reply("error frame-dump: callback unavailable"); return }
            do {
                try onFrameDump(path)
                reply("ok frame-dump")
            } catch {
                reply("error frame-dump: \(error)")
            }
            return
        }
        if command.hasPrefix("graphics-backend ") {
            let name = String(command.dropFirst("graphics-backend ".count))
            guard let mode = GraphicsBackend(rawValue: name), let onGraphicsRequest else {
                reply("error invalid graphics backend"); return
            }
            do {
                try onGraphicsRequest(mode)
                reply("ok graphics-backend \(name) requested")
            } catch {
                reply("error \(error)")
            }
            return
        }
        if command == "uu install" || command == "uu status" {
            do {
                try sendUU(command == "uu install" ? .install : .status)
                reply("ok \(command) requested")
            } catch {
                reply("error \(error)")
            }
            return
        }
        let args = command.split(separator: " ").map(String.init)
        guard !args.isEmpty else { reply("error empty command"); return }
        if args == ["shutdown"] { onShutdown?(); reply("ok shutdown"); return }
        if args.count == 2, args[0] == "fullscreen", ["on", "off"].contains(args[1]) {
            guard let onFullScreen else { reply("error fullscreen: callback unavailable"); return }
            do {
                try onFullScreen(args[1] == "on")
                reply("ok fullscreen \(args[1]) requested")
            } catch {
                reply("error fullscreen: \(error)")
            }
            return
        }
        if args == ["pad", "state"] {
            reply("pad ready=\(ready) active=\(input.script != nil) physical=\(input.physical) selected=\(input.selected)")
            return
        }
        if args == ["pad", "neutral"] {
            guard ready else { reply("error pad not ready"); return }
            guard !input.physical else { reply("error physical pad active"); return }
            guard physicalPending == nil, !physicalResetPending else { reply("error physical release pending"); return }
            guard input.neutralScript() else { reply("error pad queue full"); return }
            reply("ok pad neutral"); return
        }
        guard args.count >= 3, args[0] == "pad",
              ["press", "dpad", "axis"].contains(args[1]) else { reply("error invalid command"); return }
        let durationIndex = args[1] == "axis" ? 4 : 3
        guard args.count == durationIndex || args.count == durationIndex + 1 else {
            reply("error invalid command"); return
        }
        let duration: Int
        if args.count == durationIndex + 1 {
            guard let value = Int(args[durationIndex]), (50...2000).contains(value) else { reply("error invalid duration"); return }
            duration = value
        } else { duration = 150 }
        let buttons = ["a": 304, "b": 305, "x": 307, "y": 308, "lb": 310, "rb": 311,
                       "back": 314, "start": 315, "guide": 316, "ls": 317, "rs": 318]
        let directions: [String: (Int, Int)] = ["up": (17, -1), "down": (17, 1), "left": (16, -1), "right": (16, 1)]
        let axes = ["lx": 0, "ly": 1, "rx": 3, "ry": 4]
        let type: Int, code: Int, value: Int
        if args[1] == "press", let button = buttons[args[2]] { type = 1; code = button; value = 1 }
        else if args[1] == "dpad", let direction = directions[args[2]] { type = 3; code = direction.0; value = direction.1 }
        else if args[1] == "axis", let axis = axes[args[2]],
                let axisValue = Int(args[3]), (-32768...32767).contains(axisValue) {
            type = 3; code = axis; value = axisValue
        }
        else { reply("error invalid pad action"); return }
        guard ready else { reply("error pad not ready"); return }
        guard !input.physical else { reply("error physical pad active"); return }
        guard physicalPending == nil, !physicalResetPending else { reply("error physical release pending"); return }
        guard input.script == nil else { reply("error pad action active"); return }
        guard let token = input.press(type: type, code: code, value: value) else {
            reply("error pad queue full"); return
        }
        let item = DispatchWorkItem { [weak self] in
            guard let self, !self.closed else { return }
            self.input.release(token)
        }
        release = item
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(duration), execute: item)
        reply("ok pad \(args[1])")
    }


    func close() {
        guard !closed, !closing else { return }
        closing = true
        // The main-queue read source may not have run before the VM exits.
        // Consume complete commands before removing the FIFO, without waiting
        // for a writer or allowing a shutdown callback to re-enter close().
        if fifoRead >= 0 {
            var buffer = [UInt8](repeating: 0, count: 4096)
            while true {
                let count = Darwin.read(fifoRead, &buffer, buffer.count)
                if count > 0 { readFIFO(Array(buffer[..<count])) }
                else if count < 0 && errno == EINTR { continue }
                else { break }
            }
        }
        stopTelemetry()
        physicalGamepad.stop()
        flush()
        closed = true
        ready = false
        uuReady = false
        graphicsReady = false
        resetGraphicsResolutions()
        graphicsWriteSource?.cancel()
        graphicsWriteSource = nil
        graphicsPending.removeAll()
        release?.cancel(); release = nil
        for source in sources { source.cancel() }
        sources.removeAll()
        writeSource?.cancel(); writeSource = nil
        clipboardWriteSource?.cancel(); clipboardWriteSource = nil
        clipboardPending.removeAll()
        pending.removeAll()
        physicalPending = nil
        physicalResetPending = false
        for fd in descriptors { Darwin.close(fd) }
        descriptors.removeAll()
        if ownsFIFO {
            unlink(runDirectory.appendingPathComponent("control.fifo").path)
            ownsFIFO = false
        }
    }
}
