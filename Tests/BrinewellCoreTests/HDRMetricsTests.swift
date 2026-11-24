import XCTest
@testable import BrinewellCore

final class HDRMetricsTests: XCTestCase {
    func testBuiltInBrightnessAndHeadroom() {
        let bright = HostDisplayCapabilities.computeHDRMetrics(maxPotentialEDR: 8, currentEDR: 1.2, referenceEDR: 1, isBuiltin: true, builtinCurrentSDRNits: 480, builtinRealtimePeakNits: 1600)
        XCTAssertEqual(bright.maxLuminanceNits, 1600)
        XCTAssertEqual(bright.sdrReferenceNits, 480)
        let dim = HostDisplayCapabilities.computeHDRMetrics(maxPotentialEDR: 8, currentEDR: 8, referenceEDR: 1, isBuiltin: true, builtinCurrentSDRNits: 200, builtinRealtimePeakNits: 1600)
        XCTAssertEqual(dim.maxLuminanceNits, 1600)
        XCTAssertEqual(dim.maxFrameAverageLuminanceNits, 1000)
        let throttled = HostDisplayCapabilities.computeHDRMetrics(maxPotentialEDR: 8, currentEDR: 1.2, referenceEDR: 1, isBuiltin: true, builtinCurrentSDRNits: 480, builtinRealtimePeakNits: 1200)
        XCTAssertEqual(throttled.maxLuminanceNits, 1200)
        XCTAssertEqual(throttled.maxFrameAverageLuminanceNits, 1000)
    }

    func testReferenceExternalAndSDR() {
        let reference = HostDisplayCapabilities.computeHDRMetrics(maxPotentialEDR: 8, currentEDR: 1.2, referenceEDR: 10, isBuiltin: true, builtinCurrentSDRNits: nil, builtinRealtimePeakNits: nil)
        XCTAssertEqual(reference.maxLuminanceNits, 1000)
        XCTAssertEqual(reference.sdrReferenceNits, 100)
        let external = HostDisplayCapabilities.computeHDRMetrics(maxPotentialEDR: 8, currentEDR: 2, referenceEDR: 1, isBuiltin: false, builtinCurrentSDRNits: nil, builtinRealtimePeakNits: nil)
        XCTAssertTrue(external.hdrSupported)
        XCTAssertEqual(external.maxLuminanceNits, 800)
        XCTAssertEqual(external.minLuminance10000ths, 50)
        let sdr = HostDisplayCapabilities.computeHDRMetrics(maxPotentialEDR: 1.05, currentEDR: 1, referenceEDR: 1, isBuiltin: false, builtinCurrentSDRNits: nil, builtinRealtimePeakNits: nil)
        XCTAssertFalse(sdr.hdrSupported)
        XCTAssertEqual(sdr.maxLuminanceNits, 0)
        XCTAssertEqual(sdr.sdrReferenceNits, 0)
    }
}
