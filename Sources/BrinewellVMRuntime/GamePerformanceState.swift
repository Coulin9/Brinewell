import Foundation

/// Intervals between a game's Present submissions, never GPU execution time.
struct GameFrameSample: Equatable {
    struct Source: Hashable {
        let backend: String
        let processID: UInt32
        let streamID: UInt64
    }

    let source: Source
    let width: UInt32
    let height: UInt32
    let focused: Bool
    let intervalCount: UInt64
    let durationNS: UInt64
    let sequence: UInt64?
    let frameIntervalsNS: [UInt64]?

    var area: UInt64 { UInt64(width) * UInt64(height) }
    var framesPerSecond: Double { Double(intervalCount) * 1_000_000_000 / Double(durationNS) }
    var frameTimeMS: Double { Double(durationNS) / Double(intervalCount) / 1_000_000 }

    init?(line: String) {
        guard line.utf8.count <= 4096 else { return nil }
        let words = line.split(separator: " ", omittingEmptySubsequences: false)
        guard words.count >= 2, words[0] == "game-frame" else { return nil }
        let hasHistory = words[1] == "2"
        guard (hasHistory && words.count == 13) ||
                (words[1] == "1" && words.count == 10 && line.utf8.count <= 256),
              words[2] == "neptune" || words[2] == "vulkan",
              words[3...(hasHistory ? 11 : 9)].allSatisfy(Self.isUnsignedDecimal),
              let pid = UInt32(words[3]), pid > 0, pid <= UInt32(Int32.max),
              let stream = UInt64(words[4]), stream > 0,
              let width = UInt32(words[5]), (1...65535).contains(width),
              let height = UInt32(words[6]), (1...65535).contains(height),
              words[7] == "0" || words[7] == "1",
              let count = UInt64(words[8]), let duration = UInt64(words[9]) else { return nil }
        source = Source(backend: String(words[2]), processID: pid, streamID: stream)
        self.width = width
        self.height = height
        focused = words[7] == "1"
        intervalCount = count
        durationNS = duration
        if focused && (count == 0 || duration == 0) { return nil }
        if hasHistory {
            guard let sequence = UInt64(words[10]), sequence > 0,
                  words[11] == "0" || words[11] == "1" else { return nil }
            self.sequence = sequence
            if !focused && (count != 0 || duration != 0 || words[11] != "0") { return nil }
            if words[11] == "1" {
                guard focused, count <= 128 else { return nil }
                let values = words[12].split(separator: ",", omittingEmptySubsequences: false)
                guard values.count == Int(count) else { return nil }
                var intervals: [UInt64] = []
                intervals.reserveCapacity(values.count)
                var total: UInt64 = 0
                for value in values {
                    guard Self.isUnsignedDecimal(value), let interval = UInt64(value), interval > 0 else { return nil }
                    let (sum, overflow) = total.addingReportingOverflow(interval)
                    guard !overflow else { return nil }
                    total = sum
                    intervals.append(interval)
                }
                guard total == duration else { return nil }
                frameIntervalsNS = intervals
            } else {
                guard words[12] == "-" else { return nil }
                frameIntervalsNS = nil
            }
        } else {
            sequence = nil
            frameIntervalsNS = nil
        }
    }

    private static func isUnsignedDecimal(_ value: Substring) -> Bool {
        !value.isEmpty && value.utf8.allSatisfy { (48...57).contains($0) }
    }
}

struct GameFrameHistory: Equatable {
    static let maximumDurationNS: UInt64 = 10_000_000_000
    static let maximumSamples = 4096

    let frameTimesMS: [Double]
    let sampleCount: Int
    let durationMS: Double
    let minimumFrameTimeMS: Double
    let maximumFrameTimeMS: Double
    /// Reciprocal of the mean of the slowest ceil(N / 100) complete intervals.
    let onePercentLowFPS: Double?

    fileprivate init(intervals: [UInt64]) {
        precondition(!intervals.isEmpty)
        frameTimesMS = intervals.map { Double($0) / 1_000_000 }
        sampleCount = intervals.count
        durationMS = Double(intervals.reduce(0, +)) / 1_000_000
        minimumFrameTimeMS = Double(intervals.min()!) / 1_000_000
        maximumFrameTimeMS = Double(intervals.max()!) / 1_000_000
        if intervals.count >= 100 {
            let slowCount = (intervals.count + 99) / 100
            let slowDuration = intervals.sorted(by: >).prefix(slowCount).reduce(0, +)
            onePercentLowFPS = Double(slowCount) * 1_000_000_000 / Double(slowDuration)
        } else {
            onePercentLowFPS = nil
        }
    }
}

struct GamePerformanceSnapshot: Equatable {
    let sample: GameFrameSample
    let history: GameFrameHistory?
}

/// Select one recent focused swapchain. Independent streams are never summed.
struct GamePerformanceState {
    private struct Reading {
        let sample: GameFrameSample
        let receivedNS: UInt64
        let intervals: [UInt64]
        let history: GameFrameHistory?
    }

    private var readings: [GameFrameSample.Source: Reading] = [:]
    private var selected: GameFrameSample.Source?
    private(set) var enabled = false

    mutating func setEnabled(_ enabled: Bool) {
        guard self.enabled != enabled else { return }
        self.enabled = enabled
        readings.removeAll(keepingCapacity: true)
        selected = nil
    }

    mutating func receive(_ sample: GameFrameSample, at now: UInt64) {
        guard enabled else { return }
        expire(at: now)
        let previous = readings[sample.source]
        if let sequence = sample.sequence, let oldSequence = previous?.sample.sequence,
           sequence <= oldSequence { return }
        guard sample.focused else {
            readings.removeValue(forKey: sample.source)
            if selected == sample.source { selected = nil }
            return
        }
        if readings[sample.source] == nil, readings.count == 16,
           let oldest = readings.filter({ $0.key != selected }).min(by: { $0.value.receivedNS < $1.value.receivedNS }) {
            readings.removeValue(forKey: oldest.key)
        }
        var intervals: [UInt64] = []
        if let batch = sample.frameIntervalsNS {
            if let sequence = sample.sequence, let oldSequence = previous?.sample.sequence,
               oldSequence < UInt64.max, sequence == oldSequence + 1 {
                intervals = previous?.intervals ?? []
            }
            intervals.append(contentsOf: batch)
            // Keep whole intervals only. Missing batches and long pauses are never interpolated.
            var retained = 0
            var duration: UInt64 = 0
            for interval in intervals.reversed() {
                guard retained < GameFrameHistory.maximumSamples,
                      interval <= GameFrameHistory.maximumDurationNS - duration else { break }
                retained += 1
                duration += interval
            }
            intervals = Array(intervals.suffix(retained))
        }
        readings[sample.source] = Reading(
            sample: sample, receivedNS: now, intervals: intervals,
            history: intervals.isEmpty ? nil : GameFrameHistory(intervals: intervals))
    }

    mutating func current(at now: UInt64) -> GameFrameSample? {
        snapshot(at: now)?.sample
    }

    mutating func snapshot(at now: UInt64) -> GamePerformanceSnapshot? {
        guard enabled else { return nil }
        expire(at: now)
        var best = selected.flatMap { readings[$0] }
        for reading in readings.values where best == nil || reading.sample.area > best!.sample.area {
            best = reading
        }
        selected = best?.sample.source
        return best.map { GamePerformanceSnapshot(sample: $0.sample, history: $0.history) }
    }

    private mutating func expire(at now: UInt64) {
        readings = readings.filter {
            now >= $0.value.receivedNS && now - $0.value.receivedNS <= 2_000_000_000
        }
        if let selected, readings[selected] == nil { self.selected = nil }
    }
}
