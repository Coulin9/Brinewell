import Foundation

public struct VMSSHEndpoint: Equatable, Sendable {
    public static let defaultHost = "127.0.0.1"
    public static let defaultPort = 2222
    public static let defaultUsername = "steamos"

    public var host: String
    public var port: Int
    public var username: String

    public init(host: String = defaultHost, port: Int = defaultPort, username: String = defaultUsername) {
        self.host = host
        self.port = port
        self.username = username
    }

    public var command: String { "ssh -p \(port) \(username)@\(host)" }
}

public struct VMGuestSSHState: Codable, Equatable, Sendable {
    public var serviceLoad: String
    public var serviceActive: String
    public var serviceSub: String
    public var serviceUnitFile: String
    public var socketLoad: String
    public var socketActive: String
    public var socketSub: String
    public var socketUnitFile: String
    public var listeningOn22: Bool

    public init(serviceLoad: String, serviceActive: String, serviceSub: String, serviceUnitFile: String,
                socketLoad: String, socketActive: String, socketSub: String, socketUnitFile: String,
                listeningOn22: Bool) {
        self.serviceLoad = serviceLoad
        self.serviceActive = serviceActive
        self.serviceSub = serviceSub
        self.serviceUnitFile = serviceUnitFile
        self.socketLoad = socketLoad
        self.socketActive = socketActive
        self.socketSub = socketSub
        self.socketUnitFile = socketUnitFile
        self.listeningOn22 = listeningOn22
    }

    public var isServiceActive: Bool { serviceActive == "active" }
    public var isSocketActive: Bool { socketActive == "active" }
    public var isActive: Bool { isServiceActive || isSocketActive }
    public var isServiceEnabledOnBoot: Bool { serviceUnitFile == "enabled" }
    public var isSocketEnabledOnBoot: Bool { socketUnitFile == "enabled" }
    public var isEnabledOnBoot: Bool { isServiceEnabledOnBoot || isSocketEnabledOnBoot }
    public var isMasked: Bool {
        (serviceLoad == "masked" || serviceUnitFile == "masked" || serviceUnitFile == "masked-runtime")
            && !(isSocketActive || isSocketEnabledOnBoot)
    }
    public var isSocketOnly: Bool { isSocketActive && !isServiceActive }
    public var isRunningOnly: Bool { isActive && !isEnabledOnBoot }
    public var isBootOnly: Bool { !isActive && isEnabledOnBoot }
    public var isPort22UnavailableWhileActive: Bool { isActive && !listeningOn22 }
    public var shouldExposeHostForward: Bool { isActive && listeningOn22 }
}

public enum VMGuestPasswordStatus: String, Codable, Equatable, Sendable {
    case set, unset, locked, unknown
}

public struct VMGuestPasswordState: Codable, Equatable, Sendable {
    public var state: VMGuestPasswordStatus
    public var rev: Int

    public init(state: VMGuestPasswordStatus, rev: Int) {
        self.state = state
        self.rev = rev
    }
}

public struct VMAdminSnapshot: Codable, Equatable, Sendable {
    public var ssh: VMGuestSSHState
    public var password: VMGuestPasswordState

    public init(ssh: VMGuestSSHState, password: VMGuestPasswordState) {
        self.ssh = ssh
        self.password = password
    }
}

public enum VMAdminErrorCode: String, Codable, Equatable, Sendable, Error {
    case ok, invalidRequest, unknownCommand, unitMasked, unitNotFound, sshApplyFailed
    case sshPartiallyManaged, invalidPassword, accountMissing, accountLocked, passwordApplyFailed, internalError
    case notConnected, timeout, busy, disconnected, unknown
}

public enum VMSSHForwardState: Equatable, Sendable {
    case unpublished, publishing, published, unpublishing
    case conflict(String), error(String)
}

public enum VMPasswordValidationError: Equatable, Sendable {
    case empty, tooLong, unsupportedCharacter, mismatch
}

public enum VMPasswordValidation {
    public static func validate(password: String, confirmation: String) -> VMPasswordValidationError? {
        guard !password.isEmpty else { return .empty }
        guard password.utf8.count <= 256 else { return .tooLong }
        guard !password.unicodeScalars.contains(where: { [0, 10, 13, 58].contains($0.value) }) else {
            return .unsupportedCharacter
        }
        guard password.utf8.elementsEqual(confirmation.utf8) else { return .mismatch }
        return nil
    }
}
