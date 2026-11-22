import Foundation

enum AppLanguage: String {
    case system, english = "en", simplifiedChinese = "zh-Hans"

    private static let preferenceKey = "AppleLanguages"
    static let atLaunch = selected

    static var selected: Self {
        guard let domain = Bundle.main.bundleIdentifier,
              let languages = UserDefaults.standard.persistentDomain(forName: domain)?[preferenceKey] as? [String],
              let first = languages.first else { return .system }
        return Self(rawValue: first) ?? .system
    }

    func save() {
        // macOS reads this app-domain preference before loading localized resources.
        if self == .system {
            UserDefaults.standard.removeObject(forKey: Self.preferenceKey)
        } else {
            UserDefaults.standard.set([rawValue], forKey: Self.preferenceKey)
        }
    }
}
