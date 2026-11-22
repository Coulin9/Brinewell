// Camera-area frame/menu integration adapted from UTM's VMDisplayWindow.swift.
// Copyright 2026 Turing Software, LLC. SPDX-License-Identifier: Apache-2.0
// See runtime/licenses/UTM-Apache-2.0.txt for the license and upstream source.
import AppKit
import Darwin
import ObjectiveC

/// Optional camera-area extension of AppKit full screen. Undocumented interfaces
/// are isolated here and checked before installation; failure leaves native Spaces
/// and the system's safe-area layout intact. No methods on NSWindow are replaced.
@MainActor
enum NativeFullScreenSupport {
    private typealias FrameMethod = @convention(c) @Sendable (NSWindow, Selector) -> NSRect
    private typealias FrameBlock = @convention(block) (NSWindow) -> NSRect
    private static let tileSelector = NSSelectorFromString("_tileFrameForFullScreen")
    private static var tileFrame: FrameMethod?
    private static let server = WindowServer()

    static let isAvailable: Bool = {
        let frameSelector = NSSelectorFromString("_frameForFullScreenMode")
        let encoding = "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"
        guard server != nil,
              let frame = class_getInstanceMethod(NSWindow.self, frameSelector),
              let tile = class_getInstanceMethod(NSWindow.self, tileSelector),
              let frameTypes = method_getTypeEncoding(frame), String(cString: frameTypes) == encoding,
              let tileTypes = method_getTypeEncoding(tile), String(cString: tileTypes) == encoding else { return false }
        let original = unsafeBitCast(method_getImplementation(frame), to: FrameMethod.self)
        let tileFunction = unsafeBitCast(method_getImplementation(tile), to: FrameMethod.self)
        let block: FrameBlock = { window in
            MainActor.assumeIsolated {
                let normal = original(window, frameSelector)
                guard let screen = window.screen, screen.safeAreaInsets.top > 0,
                      tileFunction(window, tileSelector) == screen.frame else { return normal }
                return screen.frame
            }
        }
        let implementation = imp_implementationWithBlock(block)
        guard class_addMethod(VMWindow.self, frameSelector, implementation, frameTypes) else {
            imp_removeBlock(implementation)
            return false
        }
        tileFrame = tileFunction
        return true
    }()

    static func occupiesWholeScreen(_ window: NSWindow) -> Bool {
        guard let screen = window.screen else { return false }
        return tileFrame?(window, tileSelector) == screen.frame
    }

    static func fullScreenSpaceID(for window: NSWindow) -> UInt64? {
        server?.fullScreenSpaceID(window.windowNumber)
    }

    static func setMenuBarAlpha(_ alpha: Float, in space: UInt64) -> Bool {
        server?.setMenuBarAlpha(alpha, space: space) ?? false
    }

    private struct WindowServer {
        private typealias Connection = @convention(c) () -> Int32
        private typealias Spaces = @convention(c) (Int32, Int32, CFArray) -> Unmanaged<CFArray>?
        private typealias SpaceType = @convention(c) (Int32, UInt64) -> Int32
        private typealias Transaction = @convention(c) (Int32) -> Unmanaged<CFTypeRef>?
        private typealias Alpha = @convention(c) (CFTypeRef, UInt64, Float) -> Void
        private typealias Commit = @convention(c) (CFTypeRef, Int32) -> Int32

        private let connection: Connection
        private let spaces: Spaces
        private let spaceType: SpaceType
        private let transaction: Transaction
        private let alpha: Alpha
        private let commit: Commit

        init?() {
            guard let library = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY) else { return nil }
            func resolve<T>(_ name: String, as type: T.Type) -> T? {
                guard let symbol = dlsym(library, name) else { return nil }
                return unsafeBitCast(symbol, to: type)
            }
            guard let connection = resolve("SLSMainConnectionID", as: Connection.self),
                  let spaces = resolve("SLSCopySpacesForWindows", as: Spaces.self),
                  let spaceType = resolve("SLSSpaceGetType", as: SpaceType.self),
                  let transaction = resolve("SLSTransactionCreate", as: Transaction.self),
                  let alpha = resolve("SLSTransactionSetMenuBarSystemOverrideAlpha", as: Alpha.self),
                  let commit = resolve("SLSTransactionCommit", as: Commit.self) else {
                dlclose(library)
                return nil
            }
            // Keep SkyLight loaded while these function pointers are in use.
            self.connection = connection
            self.spaces = spaces
            self.spaceType = spaceType
            self.transaction = transaction
            self.alpha = alpha
            self.commit = commit
        }

        func fullScreenSpaceID(_ windowNumber: Int) -> UInt64? {
            let windows = [NSNumber(value: windowNumber)] as CFArray
            guard let result = spaces(connection(), 7, windows)?.takeRetainedValue() as? [NSNumber],
                  result.count == 1 else { return nil }
            let id = result[0].uint64Value
            return spaceType(connection(), id) == 4 ? id : nil
        }

        func setMenuBarAlpha(_ value: Float, space: UInt64) -> Bool {
            guard let update = transaction(connection())?.takeRetainedValue() else { return false }
            alpha(update, space, value)
            return commit(update, 1) == 0
        }
    }
}
