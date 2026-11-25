import Foundation

public struct PCGamingWikiReleaseDate: Sendable, Equatable, Comparable {
    public let year: Int
    public let month: Int
    public let day: Int

    public init(year: Int, month: Int, day: Int) {
        self.year = year
        self.month = month
        self.day = day
    }

    public static func < (lhs: Self, rhs: Self) -> Bool {
        (lhs.year, lhs.month, lhs.day) < (rhs.year, rhs.month, rhs.day)
    }
}

public struct PCGamingWikiCandidate: Sendable, Equatable {
    public let pageID: Int
    public let title: String
    public let appIDs: Set<UInt32>
    public let windowsReleaseDate: PCGamingWikiReleaseDate?
    public let direct3DMajors: Set<Int>
    public let hasVulkan: Bool
    public let hasOpenGL: Bool

    public init(pageID: Int, title: String, appIDs: Set<UInt32>, windowsReleaseDate: PCGamingWikiReleaseDate?,
                direct3DMajors: Set<Int>, hasVulkan: Bool, hasOpenGL: Bool) {
        self.pageID = pageID
        self.title = title
        self.appIDs = appIDs
        self.windowsReleaseDate = windowsReleaseDate
        self.direct3DMajors = direct3DMajors
        self.hasVulkan = hasVulkan
        self.hasOpenGL = hasOpenGL
    }
}

public enum PCGamingWikiParser {
    private struct Template {
        let name: String
        let parameters: [String: String]
        let positional: [String]
    }

    private static func normalized(_ value: String) -> String {
        value.replacingOccurrences(of: "_", with: " ").lowercased()
            .split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    // Scan each byte at most a constant number of times, including malformed tags.
    private static func masked(_ text: String, isExpired: (() -> Bool)?) -> String? {
        let bytes = Array(text.utf8)
        var output: [UInt8] = []
        output.reserveCapacity(bytes.count)
        var i = 0, steps = 0
        let commentOpen = Array("<!--".utf8), commentClose = Array("-->".utf8)
        let refOpen = Array("<ref".utf8), nowikiOpen = Array("<nowiki".utf8)
        let ref = Array("ref".utf8), nowiki = Array("nowiki".utf8)
        func expired() -> Bool {
            steps += 1
            return steps % 4096 == 0 && (isExpired?() ?? false)
        }
        func matches(_ pattern: [UInt8], at index: Int) -> Bool {
            guard index + pattern.count <= bytes.count else { return false }
            for offset in pattern.indices {
                let byte = bytes[index + offset]
                let lower = (65...90).contains(byte) ? byte + 32 : byte
                if lower != pattern[offset] { return false }
            }
            return true
        }
        func wordByte(_ byte: UInt8) -> Bool {
            (48...57).contains(byte) || (65...90).contains(byte) ||
                (97...122).contains(byte) || byte == 95 || byte >= 128
        }
        while i < bytes.count {
            if expired() { return nil }
            if matches(commentOpen, at: i) {
                i += 4
                while i < bytes.count && !matches(commentClose, at: i) {
                    if expired() { return nil }; i += 1
                }
                if i < bytes.count { i += 3 }
                output.append(32)
                continue
            }
            let tag: [UInt8]?
            if matches(refOpen, at: i) { tag = ref }
            else if matches(nowikiOpen, at: i) { tag = nowiki }
            else { tag = nil }
            if let tag, i + tag.count + 1 < bytes.count,
               !wordByte(bytes[i + tag.count + 1]) {
                let start = i
                i += tag.count + 1
                while i < bytes.count && bytes[i] != 62 {
                    if expired() { return nil }; i += 1
                }
                guard i < bytes.count else {
                    output.append(contentsOf: bytes[start...])
                    break
                }
                let selfClosing = i > start && bytes[i - 1] == 47
                i += 1
                if !selfClosing {
                    let closing = [UInt8]("</".utf8) + tag
                    while i < bytes.count {
                        if expired() { return nil }
                        if matches(closing, at: i) {
                            i += closing.count
                            while i < bytes.count && [9, 10, 11, 12, 13, 32].contains(bytes[i]) {
                                if expired() { return nil }; i += 1
                            }
                            if i < bytes.count && bytes[i] == 62 { i += 1; break }
                        } else { i += 1 }
                    }
                }
                output.append(32)
                continue
            }
            output.append(bytes[i]); i += 1
        }
        return String(decoding: output, as: UTF8.self)
    }

    // Split only at the outer level, respecting nested templates and wiki links.
    private static func split(_ text: String, separator: Character, maxSplits: Int = .max,
                              isExpired: (() -> Bool)? = nil) -> [String]? {
        let chars = Array(text)
        var parts: [String] = []
        var start = 0, i = 0, braces = 0, links = 0
        while i < chars.count {
            if i % 4096 < 2 && (isExpired?() ?? false) { return nil }
            if i + 1 < chars.count {
                let pair = String(chars[i...i+1])
                if pair == "{{" { braces += 1; i += 2; continue }
                if pair == "}}", braces > 0 { braces -= 1; i += 2; continue }
                if pair == "[[" { links += 1; i += 2; continue }
                if pair == "]]", links > 0 { links -= 1; i += 2; continue }
            }
            if chars[i] == separator && braces == 0 && links == 0 && parts.count < maxSplits {
                parts.append(String(chars[start..<i])); start = i + 1
            }
            i += 1
        }
        parts.append(String(chars[start..<chars.count]))
        return parts
    }

    private static func templates(_ text: String, isExpired: (() -> Bool)? = nil) -> [Template]? {
        let chars = Array(text)
        // Spans are recorded in opening order; nested annotation templates are opaque.
        let annotations: Set<String> = ["note", "ref", "refcheck", "refurl", "cn", "citation needed"]
        var spans: [(start: Int, end: Int?)] = []
        var stack: [(span: Int?, opaque: Bool)] = []
        var depth = 0, i = 0
        while i + 1 < chars.count {
            if i % 4096 < 2 && (isExpired?() ?? false) { return nil }
            if chars[i] == "{" && chars[i + 1] == "{" {
                depth += 1
                if depth <= 64 {
                    var nameEnd = i + 2
                    while nameEnd < chars.count && chars[nameEnd] != "|" && chars[nameEnd] != "{" && chars[nameEnd] != "}" {
                        if nameEnd % 4096 == 0 && (isExpired?() ?? false) { return nil }
                        nameEnd += 1
                    }
                    let name = normalized(String(chars[(i + 2)..<nameEnd]))
                    let opaque = (stack.last?.opaque ?? false) || annotations.contains(name)
                    var span: Int?
                    if !opaque && ["infobox game", "infobox game/row/date", "api"].contains(name) {
                        span = spans.count
                        spans.append((i + 2, nil))
                    }
                    stack.append((span, opaque))
                }
                i += 2
            } else if chars[i] == "}" && chars[i + 1] == "}" && depth > 0 {
                if depth <= 64, let entry = stack.popLast(), let span = entry.span {
                    spans[span].end = i
                }
                depth -= 1
                i += 2
            } else {
                i += 1
            }
        }
        var result: [Template] = []
        for span in spans {
            if isExpired?() ?? false { return nil }
            guard let end = span.end else { continue }
            guard let parts = split(String(chars[span.start..<end]), separator: "|", isExpired: isExpired) else { return nil }
            var parameters: [String: String] = [:], positional: [String] = []
            for part in parts.dropFirst() {
                guard let pair = split(part, separator: "=", maxSplits: 1, isExpired: isExpired) else { return nil }
                if pair.count == 2 { parameters[normalized(pair[0])] = pair[1] }
                else { positional.append(part.trimmingCharacters(in: .whitespacesAndNewlines)) }
            }
            result.append(Template(name: normalized(parts[0]), parameters: parameters, positional: positional))
        }
        return result
    }

    private static func withoutTemplates(_ value: String) -> String {
        let chars = Array(value)
        var result = "", depth = 0, i = 0
        while i < chars.count {
            if i + 1 < chars.count {
                let pair = String(chars[i...i+1])
                if pair == "{{" { depth += 1; i += 2; continue }
                if pair == "}}", depth > 0 { depth -= 1; if depth == 0 { result += " " }; i += 2; continue }
            }
            if depth == 0 { result.append(chars[i]) }
            i += 1
        }
        return result
    }

    private static func date(_ raw: String) -> PCGamingWikiReleaseDate? {
        let value = withoutTemplates(raw).trimmingCharacters(in: .whitespacesAndNewlines)
        let months = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"]
        let words = value.lowercased().replacingOccurrences(of: ",", with: " ").split(whereSeparator: { $0.isWhitespace }).map(String.init)
        var year: Int?, month = 0, day = 0
        let iso = value.split(separator: "-", omittingEmptySubsequences: false)
        if iso.count == 3, iso[0].count == 4, iso[1].count == 2, iso[2].count == 2,
           iso.allSatisfy({ $0.utf8.allSatisfy { (48...57).contains($0) } }),
           let y = Int(iso[0]), let m = Int(iso[1]), let d = Int(iso[2]),
           (1...12).contains(m), (1...31).contains(d) { year = y; month = m; day = d }
        else if words.count == 1, words[0].count == 4 { year = Int(words[0]) }
        else if (words.count == 2 || words.count == 3), let index = months.firstIndex(of: words[0]), words.last?.count == 4 {
            year = Int(words.last!); month = index + 1
            if words.count == 3 { guard let d = Int(words[1]), (1...31).contains(d) else { return nil }; day = d }
        }
        guard let year, year > 0, (0...12).contains(month), (0...31).contains(day), month != 0 || day == 0 else { return nil }
        if day > 0 {
            var calendar = Calendar(identifier: .gregorian); calendar.timeZone = TimeZone(secondsFromGMT: 0)!
            var components = DateComponents(); components.year = year; components.month = month; components.day = day
            guard let date = calendar.date(from: components), calendar.component(.day, from: date) == day else { return nil }
        }
        return PCGamingWikiReleaseDate(year: year, month: month, day: day)
    }

    public static func parse(pageID: Int, title: String, wikitext: String, isExpired: (() -> Bool)? = nil) -> PCGamingWikiCandidate {
        let empty = PCGamingWikiCandidate(pageID: pageID, title: title, appIDs: [], windowsReleaseDate: nil, direct3DMajors: [], hasVulkan: false, hasOpenGL: false)
        guard !(isExpired?() ?? false), let text = masked(wikitext, isExpired: isExpired),
              let all = templates(text, isExpired: isExpired), !(isExpired?() ?? false) else { return empty }
        var ids = Set<UInt32>(), dates: [PCGamingWikiReleaseDate] = [], majors = Set<Int>()
        var vulkan = false, openGL = false
        for template in all where template.name == "infobox game" {
            if let raw = template.parameters["steam appid"] {
                for token in withoutTemplates(raw).split(whereSeparator: { $0.isWhitespace || $0 == "," }) {
                    if let id = UInt32(token), id > 0, String(id) == token { ids.insert(id) }
                }
            }
            if let releases = template.parameters["release dates"] {
                guard let rows = templates(releases, isExpired: isExpired) else { return empty }
                for row in rows where row.name == "infobox game/row/date" && row.positional.count >= 2 {
                    let platform = normalized(row.positional[0])
                    if (platform == "windows" || platform == "microsoft windows"), let parsed = date(row.positional[1]) { dates.append(parsed) }
                }
            }
        }
        for template in all where template.name == "api" {
            if let raw = template.parameters["direct3d versions"] {
                let value = withoutTemplates(raw)
                let regex = try! NSRegularExpression(pattern: "(?i)(?<![a-z0-9.])(?:(?:dx|d3d|direct3d|directx)\\s*)?([1-9]|1[0-2])(?:\\.[0-9]+)?(?:ex|c)?(?![a-z0-9.])")
                let range = NSRange(value.startIndex..., in: value)
                for match in regex.matches(in: value, range: range) {
                    if let r = Range(match.range(at: 1), in: value), let major = Int(value[r]) { majors.insert(major) }
                }
            }
            func present(_ key: String) -> Bool {
                let value = normalized(withoutTemplates(template.parameters[key] ?? ""))
                return !value.isEmpty && !["false", "no", "none", "unknown", "n/a"].contains(value)
            }
            vulkan = vulkan || present("vulkan versions")
            openGL = openGL || present("opengl versions")
        }
        return PCGamingWikiCandidate(pageID: pageID, title: title, appIDs: ids, windowsReleaseDate: dates.min(), direct3DMajors: majors, hasVulkan: vulkan, hasOpenGL: openGL)
    }

    public static func recommendedBackend(forDirect3DMajors majors: Set<Int>) -> GraphicsBackend {
        if majors.contains(12) || majors.contains(11) { return .neptune }
        if majors.contains(10) { return .mullion }
        return .venus
    }

    public static func selectBestCandidate(_ candidates: [PCGamingWikiCandidate], for appID: UInt32) -> PCGamingWikiCandidate? {
        func tier(_ candidate: PCGamingWikiCandidate) -> Int {
            switch recommendedBackend(forDirect3DMajors: candidate.direct3DMajors) {
            case .neptune: return 2
            case .mullion: return 1
            case .venus, .automatic: return 0
            }
        }
        return candidates.filter { $0.appIDs.contains(appID) }.sorted { lhs, rhs in
            if lhs.windowsReleaseDate != rhs.windowsReleaseDate {
                guard let l = lhs.windowsReleaseDate else { return false }
                guard let r = rhs.windowsReleaseDate else { return true }
                return l > r
            }
            if tier(lhs) != tier(rhs) { return tier(lhs) > tier(rhs) }
            return lhs.pageID < rhs.pageID
        }.first
    }
}
