import Darwin
import Foundation
import OSLog

/// Moves an AppleDeck installation's data to the Brinewell locations on the
/// first launch after the rename. Must run before the configuration is loaded
/// so relocated paths are already in place, and before `AppLanguage.atLaunch`
/// is read so an imported language preference applies to this launch.
public enum LegacyInstallationMigration {
    public struct Report: Sendable, Equatable {
        public enum Status: String, Sendable {
            /// Nothing to do: no legacy directory and no pending rewrite.
            case nothingToDo
            /// The legacy directory moved, or a pending rewrite finished.
            case migrated
            /// Both an AppleDeck and a Brinewell directory exist; nothing was touched.
            case conflict
            /// The legacy path exists but is a file or symlink; nothing was touched.
            case pathOccupied
            /// The directory move failed; data is still at the legacy path.
            case notMoved
            /// The data is at the new path but the configuration file could
            /// not be read, decoded, or rewritten; it was left untouched.
            case movedButConfigNeedsRepair
        }
        public let status: Status
        public let legacySupportURL: URL
        public let supportURL: URL
        /// The legacy directory was renamed to the new path during this run.
        /// False when only the new directory exists (native install or an
        /// interrupted move resumed on a later launch).
        public let movedData: Bool
        /// The stored configuration had paths under the legacy root relocated.
        public let rewroteConfiguration: Bool
        /// The stored configuration's contents are not a decodable
        /// configuration (invalid JSON or missing/invalid fields); it was
        /// left untouched.
        public let invalidConfiguration: Bool
        /// The configuration file could not be read at all (permission or
        /// I/O error); it was left untouched.
        public let unreadableConfiguration: Bool
        public let cachesMoved: Bool
        /// OS error detail for `.notMoved`/`.movedButConfigNeedsRepair`; safe
        /// to show, never holds config data.
        public let failureDescription: String?
        /// Hard failures the launch flow must surface instead of acting fresh:
        /// the user's data did not move, or it moved with a configuration the
        /// app cannot trust, so automatic startup and onboarding stay off.
        public var needsAttention: Bool {
            status == .conflict || status == .pathOccupied || status == .notMoved
                || status == .movedButConfigNeedsRepair
        }
    }

    public static let legacyDirectoryName = "AppleDeck"
    public static let directoryName = "Brinewell"
    public static let configurationFileName = "configuration.json"
    public static let legacyDefaultsDomain = "org.appledeck.AppleDeck"
    public static let defaultsImportedKey = "LegacyAppleDeckDefaultsImported"
    private static let languagesKey = "AppleLanguages"
    private static let logger = Logger(subsystem: "org.brinewell.Brinewell", category: "migration")

    /// Migration using the real per-user Library locations and defaults.
    @discardableResult
    public static func migrateIfNeeded() -> Report {
        let files = FileManager.default
        let support = (try? files.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                      appropriateFor: nil, create: false))
            ?? files.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support", isDirectory: true)
        let caches = (try? files.url(for: .cachesDirectory, in: .userDomainMask,
                                     appropriateFor: nil, create: false))
            ?? files.homeDirectoryForCurrentUser.appendingPathComponent("Library/Caches", isDirectory: true)
        return migrateIfNeeded(applicationSupport: support, caches: caches,
                               defaults: .standard,
                               legacyDefaults: UserDefaults(suiteName: legacyDefaultsDomain),
                               legacyDefaultsDomain: legacyDefaultsDomain,
                               newDefaultsDomain: Bundle.main.bundleIdentifier)
    }

    /// `applicationSupport` and `caches` are the parent roots holding the
    /// `AppleDeck`/`Brinewell` directories, so tests can point at temp dirs.
    /// The defaults domains scope the AppleLanguages check to each app's own
    /// persistent domain; `AppleLanguages` also exists in NSGlobalDomain, so a
    /// plain key lookup would always find the system language list.
    @discardableResult
    public static func migrateIfNeeded(applicationSupport: URL, caches: URL,
                                       defaults: UserDefaults, legacyDefaults: UserDefaults?,
                                       legacyDefaultsDomain: String? = nil,
                                       newDefaultsDomain: String? = nil) -> Report {
        importLanguage(defaults: defaults, domain: newDefaultsDomain ?? Bundle.main.bundleIdentifier,
                       legacyDefaults: legacyDefaults,
                       legacyDomain: legacyDefaultsDomain ?? Self.legacyDefaultsDomain)
        let cachesMoved = moveCachesIfNeeded(caches: caches)
        var report = moveSupportIfNeeded(applicationSupport: applicationSupport)
        return Report(status: report.status, legacySupportURL: report.legacySupportURL,
                      supportURL: report.supportURL, movedData: report.movedData,
                      rewroteConfiguration: report.rewroteConfiguration,
                      invalidConfiguration: report.invalidConfiguration,
                      unreadableConfiguration: report.unreadableConfiguration,
                      cachesMoved: cachesMoved, failureDescription: report.failureDescription)
    }

    /// Copies only the language preference. The one-time flag is set once the
    /// decision is made — even when nothing is copied — so a later "follow
    /// system" choice is never overridden by a re-import. Both sides are read
    /// through their persistent domains: `AppleLanguages` lives in
    /// NSGlobalDomain too, so `object(forKey:)` would always find the system
    /// list even when the legacy app never stored a preference.
    private static func importLanguage(defaults: UserDefaults, domain: String?,
                                       legacyDefaults: UserDefaults?, legacyDomain: String?) {
        guard !defaults.bool(forKey: defaultsImportedKey) else { return }
        let existing = domain.flatMap { defaults.persistentDomain(forName: $0) }?[languagesKey]
        if existing == nil, let legacyDomain,
           let languages = legacyDefaults?.persistentDomain(forName: legacyDomain)?[languagesKey] {
            defaults.set(languages, forKey: languagesKey)
        }
        defaults.set(true, forKey: defaultsImportedKey)
    }

    /// Caches are rebuildable, so a failed move is ignored.
    private static func moveCachesIfNeeded(caches: URL) -> Bool {
        let old = caches.appendingPathComponent(legacyDirectoryName, isDirectory: true)
        let new = caches.appendingPathComponent(directoryName, isDirectory: true)
        guard isRealDirectory(old), !exists(new) else { return false }
        return rename(old.path, new.path) == 0
    }

    private static func moveSupportIfNeeded(applicationSupport: URL) -> Report {
        let old = applicationSupport.appendingPathComponent(legacyDirectoryName, isDirectory: true)
        let new = applicationSupport.appendingPathComponent(directoryName, isDirectory: true)
        let canonicalOld = canonical(old)
        let canonicalNew = canonical(new)
        let oldIsRealDirectory = isRealDirectory(old)
        func report(_ status: Report.Status, rewrote: Bool = false, invalid: Bool = false,
                    unreadable: Bool = false, failure: String? = nil) -> Report {
            Report(status: status, legacySupportURL: old, supportURL: new,
                   movedData: oldIsRealDirectory, rewroteConfiguration: rewrote,
                   invalidConfiguration: invalid, unreadableConfiguration: unreadable,
                   cachesMoved: false, failureDescription: failure)
        }

        // A file or symlink at the legacy path is reported separately from a
        // real directory colliding with an existing new directory.
        if exists(old) && !oldIsRealDirectory {
            logger.error("legacy path occupied at \(old.path, privacy: .public)")
            return report(.pathOccupied)
        }
        if oldIsRealDirectory && exists(new) {
            logger.error("migration conflict at \(old.path, privacy: .public)")
            return report(.conflict)
        }
        if oldIsRealDirectory {
            guard rename(old.path, new.path) == 0 else {
                let code = errno
                logger.error("rename failed: \(code)")
                return report(.notMoved, failure: String(cString: strerror(code)))
            }
            logger.notice("moved \(old.path, privacy: .public) to \(new.path, privacy: .public)")
        } else if !exists(new) {
            return report(.nothingToDo)
        }
        // Reaching here with `new` present but `old` absent means the move was
        // already done — possibly interrupted before the path rewrite below,
        // which therefore runs idempotently on every launch until clean.
        let configurationURL = new.appendingPathComponent(configurationFileName)
        if !exists(configurationURL) { return report(oldIsRealDirectory ? .migrated : .nothingToDo) }
        let dictionary: [String: Any]
        switch readConfiguration(at: configurationURL) {
        case .unreadable(let detail):
            return report(.movedButConfigNeedsRepair, unreadable: true, failure: detail)
        case .invalid:
            // The file stayed untouched; it must not be treated as a clean
            // migration or this launch would onboard over the user's data.
            return report(.movedButConfigNeedsRepair, invalid: true)
        case .dictionary(let parsed):
            dictionary = parsed
        }
        let rewrote: Bool
        do {
            rewrote = try rewriteConfiguration(dictionary, at: configurationURL,
                                               from: canonicalOld, to: canonicalNew)
        } catch let failure as UndecodableCandidate {
            logger.error("rewritten configuration does not decode: \(failure.underlying.localizedDescription, privacy: .public)")
            return report(.movedButConfigNeedsRepair, invalid: true,
                          failure: failure.underlying.localizedDescription)
        } catch {
            logger.error("configuration rewrite failed: \(error.localizedDescription, privacy: .public)")
            return report(.movedButConfigNeedsRepair, failure: error.localizedDescription)
        }
        // Valid JSON is not necessarily a loadable configuration (for example
        // `{}` lacks cpuCount/memoryGiB). Decode with the real decoder —
        // without host-resource settings validation — so a file the app
        // cannot load is never reported as a clean migration.
        do {
            _ = try ConfigurationStore(url: configurationURL).decode()
        } catch {
            logger.error("stored configuration does not decode: \(error.localizedDescription, privacy: .public)")
            return report(.movedButConfigNeedsRepair, invalid: true,
                          failure: error.localizedDescription)
        }
        return report(rewrote || oldIsRealDirectory ? .migrated : .nothingToDo,
                      rewrote: rewrote)
    }

    /// The rewritten candidate failed the real decoder; thrown before the
    /// candidate replaces the user's file so the original stays untouched.
    private struct UndecodableCandidate: Error {
        let underlying: Error
    }

    /// Absolute paths under the old root are re-anchored to the new root;
    /// everything else (unknown keys, external paths) is preserved as-is.
    /// The rewritten candidate is validated with the real decoder while it is
    /// still a temporary file: a result the app could not load never replaces
    /// the user's configuration.
    private static func rewriteConfiguration(_ dictionary: [String: Any], at url: URL,
                                             from canonicalOld: String, to canonicalNew: String) throws -> Bool {
        var changed = false
        let updated = relocate(dictionary, from: canonicalOld, to: canonicalNew, changed: &changed)
        guard changed else { return false }
        let data = try JSONSerialization.data(withJSONObject: updated, options: [.prettyPrinted, .sortedKeys])
        let temporary = url.deletingLastPathComponent()
            .appendingPathComponent(".\(UUID().uuidString).json")
        try writePrivateConfiguration(data, to: temporary)
        defer { try? FileManager.default.removeItem(at: temporary) }
        do {
            _ = try ConfigurationStore(url: temporary).decode()
        } catch {
            throw UndecodableCandidate(underlying: error)
        }
        guard rename(temporary.path, url.path) == 0 else { throw CocoaError(.fileWriteUnknown) }
        return true
    }

    private static func relocate(_ value: Any, from canonicalOld: String, to canonicalNew: String,
                                 changed: inout Bool) -> Any {
        if let dictionary = value as? [String: Any] {
            return dictionary.mapValues { relocate($0, from: canonicalOld, to: canonicalNew, changed: &changed) }
        }
        if let array = value as? [Any] {
            return array.map { relocate($0, from: canonicalOld, to: canonicalNew, changed: &changed) }
        }
        if let string = value as? String, let relative = suffixInside(string, root: canonicalOld) {
            changed = true
            return relative.isEmpty ? canonicalNew : canonicalNew + "/" + relative
        }
        return value
    }

    /// Returns `string`'s path relative to `root`, `""` when equal, or nil when
    /// outside. Comparing resolved path components (not string prefixes) keeps
    /// `/var/...` vs `/private/var/...` equivalent and rejects lookalikes.
    private static func suffixInside(_ string: String, root: String) -> String? {
        guard string.hasPrefix("/") else { return nil }
        let resolved = URL(fileURLWithPath: string).resolvingSymlinksInPath().path
        let rootComponents = URL(fileURLWithPath: root).pathComponents
        let components = URL(fileURLWithPath: resolved).pathComponents
        guard components.count >= rootComponents.count,
              Array(components.prefix(rootComponents.count)) == rootComponents else { return nil }
        return components.dropFirst(rootComponents.count).joined(separator: "/")
    }

    private enum StoredConfiguration {
        case dictionary([String: Any])
        /// Reading failed (permission or I/O error); the payload is the error description.
        case unreadable(String)
        /// The file is not a JSON dictionary.
        case invalid
    }

    private static func readConfiguration(at url: URL) -> StoredConfiguration {
        do {
            let data = try Data(contentsOf: url)
            guard let object = try? JSONSerialization.jsonObject(with: data),
                  let dictionary = object as? [String: Any] else {
                return .invalid
            }
            return .dictionary(dictionary)
        } catch {
            return .unreadable(error.localizedDescription)
        }
    }

    private static func canonical(_ url: URL) -> String {
        URL(fileURLWithPath: url.path).resolvingSymlinksInPath().path
    }

    /// `fileExists(atPath:)` follows symlinks, so a dangling link looks absent
    /// and an entry that must block migration would be treated as nothing.
    /// `lstat` reports the entry itself.
    private static func exists(_ url: URL) -> Bool {
        var info = stat()
        return lstat(url.path, &info) == 0
    }

    /// A symlinked or plain-file legacy directory is not migrated silently.
    /// `lstat` keeps a symlink — even one pointing at a real directory — on
    /// the occupied path instead of renaming the link itself.
    private static func isRealDirectory(_ url: URL) -> Bool {
        var info = stat()
        return lstat(url.path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR
    }
}
