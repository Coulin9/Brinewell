import Foundation
import XCTest
@testable import BrinewellVMRuntime

final class HostCPUInfoTests: XCTestCase {
    func testUsesHighestSupportedFrequencyAcrossCoreTiers() {
        let sample = Data("""
            {"cpu_tiers":[{"freq_mhz":1308}],"soc":{"cpu_tiers":[
              {"label":"S","freqs":[1308,4608]},
              {"label":"P","freqs":[1344,4380]},
              {"label":"E","freqs":[0,2400,25000]}]}}
            """.utf8)
        XCTAssertEqual(HostCPUInfo.maximumFrequencyMHz(in: sample), 4608)
    }

    func testMissingFrequencyTableDoesNotUseLiveFrequency() {
        for sample in [#"{"cpu_tiers":[{"freq_mhz":1308}]}"#,
                       #"{"soc":{"cpu_tiers":[{"freqs":[0]},{"freqs":[]}]}}"#] {
            XCTAssertNil(HostCPUInfo.maximumFrequencyMHz(in: Data(sample.utf8)))
        }
    }
}
