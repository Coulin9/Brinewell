import Foundation
import XCTest
@testable import BrinewellCore

final class VMDisplayConfigurationTests: XCTestCase {
    private let host = HostCapabilities(logicalCPUCount: 8, physicalMemoryBytes: 16 * 1_073_741_824,
                                        architecture: "test", metalDeviceName: nil)

    func testLegacyConfigurationKeepsExistingDisplayMode() throws {
        let configuration = try JSONDecoder().decode(VMConfiguration.self, from: Data(#"{"cpuCount":2,"memoryGiB":4}"#.utf8))
        XCTAssertEqual(configuration.display.width, 1280)
        XCTAssertEqual(configuration.display.height, 800)
        XCTAssertEqual(configuration.display.refreshRate, 60)
    }

    func testSavedCustomDisplayModeSurvivesReload() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
        let configuration = VMConfiguration(cpuCount: 2, memoryGiB: 4,
                                             display: VMDisplayConfiguration(width: 2304, height: 1440, refreshRate: 100))
        try store.save(configuration, for: host)
        XCTAssertEqual(try store.load(for: host), configuration)
    }

    func testSupportedModesIncludeHighRefreshAnd4K() throws {
        for mode in [VMDisplayConfiguration(),
                     VMDisplayConfiguration(width: 1920, height: 1080, refreshRate: 240),
                     VMDisplayConfiguration(width: 2560, height: 1440, refreshRate: 144),
                     VMDisplayConfiguration(width: 3456, height: 2234, refreshRate: 120),
                     VMDisplayConfiguration(width: 3840, height: 2160, refreshRate: 240),
                     VMDisplayConfiguration(width: 5120, height: 2880, refreshRate: 144),
                     VMDisplayConfiguration(width: 7680, height: 4320, refreshRate: 120),
                     VMDisplayConfiguration(width: 8192, height: 8192, refreshRate: 60)] {
            XCTAssertNoThrow(try mode.validate())
        }
    }

    func testRejectsInvalidDimensionsAndRefreshBeforeArithmeticOrConversion() {
        for value in [Int.min, -1, 0, VMDisplayConfiguration.maximumDimension + 1, Int.max] {
            XCTAssertThrowsError(try VMDisplayConfiguration(width: value).validate())
            XCTAssertThrowsError(try VMDisplayConfiguration(height: value).validate())
        }
        for rate in [Int.min, -1, 0, Int.max] {
            XCTAssertThrowsError(try VMDisplayConfiguration(refreshRate: rate).validate())
        }
    }

    func testRejectsModesBeyondDisplayIDClockCapacity() {
        let mode = VMDisplayConfiguration(width: 8192, height: 8192, refreshRate: 10000)
        XCTAssertThrowsError(try mode.validate())
    }

    func testInvalidDisplayDoesNotOverwriteSavedConfiguration() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = ConfigurationStore(url: directory.appendingPathComponent("configuration.json"))
        var configuration = VMConfiguration(cpuCount: 2, memoryGiB: 4)
        try store.save(configuration, for: host)
        let original = try Data(contentsOf: store.url)
        configuration.display.refreshRate = 0
        XCTAssertThrowsError(try store.save(configuration, for: host))
        XCTAssertEqual(try Data(contentsOf: store.url), original)
    }
}
