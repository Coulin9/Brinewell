import AppKit
import XCTest
@testable import BrinewellCore

final class HostDisplayCapabilitiesTests: XCTestCase {
    private func screen(width: Int = 1920, height: Int = 1080, refresh: Int = 60,
                        modes: [DisplayResolution] = [], rates: [Int] = []) -> HostDisplayCapabilities {
        HostDisplayCapabilities(id: 1, name: "Test Screen",
                                maximumResolution: DisplayResolution(width: width, height: height),
                                maximumRefreshRate: refresh, availableResolutions: modes, availableRefreshRates: rates)
    }

    func testDropdownCombinesCommonAndScreenModesWithoutExceedingScreen() {
        let native = DisplayResolution(width: 1500, height: 1000)
        let capabilities = screen(modes: [native, native, DisplayResolution(width: 3840, height: 2160)])
        XCTAssertTrue(capabilities.resolutions.contains(DisplayResolution(width: 1280, height: 800)))
        XCTAssertTrue(capabilities.resolutions.contains(native))
        XCTAssertEqual(capabilities.resolutions.count, Set(capabilities.resolutions).count)
        XCTAssertTrue(capabilities.resolutions.allSatisfy { $0.width <= 1920 && $0.height <= 1080 })
    }

    func testDropdownAlsoRespectsVirtualDisplayDimensionLimit() {
        let capabilities = screen(width: 32768, height: 32768)
        XCTAssertFalse(capabilities.resolutions.contains(capabilities.maximumResolution))
        XCTAssertTrue(capabilities.resolutions.contains(DisplayResolution(width: 3840, height: 2160)))
        XCTAssertTrue(capabilities.resolutions.allSatisfy {
            $0.width <= VMDisplayConfiguration.maximumDimension && $0.height <= VMDisplayConfiguration.maximumDimension
        })
    }

    func testRefreshDropdownCombinesNativeRatesWithoutLegacyHighResolutionCap() {
        let capabilities = screen(width: 3840, height: 2160, refresh: 120, rates: [85, 240])
        let hdRates = capabilities.refreshRates(for: DisplayResolution(width: 1920, height: 1080))
        XCTAssertTrue(hdRates.contains(85))
        XCTAssertTrue(hdRates.contains(120))
        XCTAssertFalse(hdRates.contains(144))
        let uhdRates = capabilities.refreshRates(for: DisplayResolution(width: 3840, height: 2160))
        XCTAssertTrue(uhdRates.contains(60))
        XCTAssertTrue(uhdRates.contains(90))
        XCTAssertTrue(uhdRates.contains(120))
        XCTAssertTrue(uhdRates.allSatisfy { $0 <= 120 })
    }

    func testOldOrManuallyEditedConfigurationCannotExceedEitherScreenDimensionOrRefresh() {
        let capabilities = screen()
        for mode in [VMDisplayConfiguration(width: 2560, height: 1080),
                     VMDisplayConfiguration(width: 1280, height: 1200),
                     VMDisplayConfiguration(refreshRate: 90)] {
            XCTAssertThrowsError(try capabilities.validate(mode))
        }
        XCTAssertNoThrow(try capabilities.validate(VMDisplayConfiguration(width: 1920, height: 1080)))
    }

    func testMovingToSmallerScreenPreservesAspectAndCapsResolutionAndRefresh() throws {
        let mode = try XCTUnwrap(screen().preferredMode(for: VMDisplayConfiguration(width: 3840, height: 2160, refreshRate: 120)))
        XCTAssertEqual(mode, VMDisplayConfiguration(width: 1920, height: 1080, refreshRate: 60))
        let unchanged = VMDisplayConfiguration(width: 1280, height: 800, refreshRate: 40)
        XCTAssertEqual(screen().preferredMode(for: unchanged), unchanged)
    }

    func testIncreasingResolutionPreservesSupportedHighRefreshRate() throws {
        let capabilities = screen(width: 3840, height: 2160, refresh: 120)
        let mode = try XCTUnwrap(capabilities.preferredMode(for: VMDisplayConfiguration(width: 3840, height: 2160, refreshRate: 120)))
        XCTAssertEqual(mode.refreshRate, 120)
        XCTAssertNoThrow(try capabilities.validate(mode))
    }

    func testNativeAndExternalHighResolutionScreensExposeTheirMaximumRefresh() {
        for (width, height, refresh) in [(3456, 2234, 120), (3840, 2160, 240), (5120, 2880, 144), (7680, 4320, 120)] {
            let capabilities = screen(width: width, height: height, refresh: refresh)
            let resolution = DisplayResolution(width: width, height: height)
            XCTAssertTrue(capabilities.resolutions.contains(resolution))
            XCTAssertTrue(capabilities.refreshRates(for: resolution).contains(refresh))
            XCTAssertFalse(capabilities.refreshRates(for: resolution).contains { $0 > refresh })
        }
    }

    func testDetectsActualHostScreenAndItsModes() throws {
        guard let capabilities = HostDisplayCapabilities.detect(), let screen = NSScreen.main else {
            throw XCTSkip("No host display is available")
        }
        XCTAssertEqual(capabilities.id, HostDisplayCapabilities.screenID(screen))
        XCTAssertGreaterThanOrEqual(capabilities.maximumRefreshRate, screen.maximumFramesPerSecond)
        XCTAssertFalse(capabilities.resolutions.isEmpty)
        XCTAssertTrue(capabilities.resolutions.allSatisfy {
            $0.width <= capabilities.maximumResolution.width && $0.height <= capabilities.maximumResolution.height
        })
    }
}
