import AppKit
import CoreGraphics
import IOKit

public struct HostDisplayCapabilities: Equatable, Sendable {
    public let id: UInt32
    public let name: String
    public let maximumResolution: DisplayResolution
    public let maximumRefreshRate: Int
    public let resolutions: [DisplayResolution]
    public let hdrSupported: Bool
    public let maxPotentialEDR: CGFloat
    public let currentEDR: CGFloat
    public let sdrReferenceNits: UInt16
    public let maxLuminanceNits: UInt16
    public let maxFrameAverageLuminanceNits: UInt16
    public let minLuminance10000ths: UInt16
    private let nativeRefreshRates: [Int]

    public init(id: UInt32, name: String, maximumResolution: DisplayResolution, maximumRefreshRate: Int,
                availableResolutions: [DisplayResolution], availableRefreshRates: [Int], maxPotentialEDR: CGFloat = 1.0,
                currentEDR: CGFloat = 1.0, referenceEDR: CGFloat = 1.0, isBuiltin: Bool = false,
                builtinCurrentSDRNits: Double? = nil, builtinRealtimePeakNits: Double? = nil) {
        self.maxPotentialEDR = maxPotentialEDR
        self.currentEDR = currentEDR
        let metrics = Self.computeHDRMetrics(maxPotentialEDR: maxPotentialEDR, currentEDR: currentEDR,
            referenceEDR: referenceEDR, isBuiltin: isBuiltin, builtinCurrentSDRNits: builtinCurrentSDRNits,
            builtinRealtimePeakNits: builtinRealtimePeakNits)
        hdrSupported = metrics.hdrSupported
        maxLuminanceNits = metrics.maxLuminanceNits
        maxFrameAverageLuminanceNits = metrics.maxFrameAverageLuminanceNits
        minLuminance10000ths = metrics.minLuminance10000ths
        sdrReferenceNits = metrics.sdrReferenceNits
        self.id = id
        self.name = name
        self.maximumResolution = maximumResolution
        self.maximumRefreshRate = maximumRefreshRate
        nativeRefreshRates = availableRefreshRates
        let candidates = Self.commonResolutions + availableResolutions + [maximumResolution]
        resolutions = Set(candidates).filter {
            (1...min(VMDisplayConfiguration.maximumDimension, maximumResolution.width)).contains($0.width)
                && (1...min(VMDisplayConfiguration.maximumDimension, maximumResolution.height)).contains($0.height)
        }.sorted {
            let leftArea = $0.width * $0.height, rightArea = $1.width * $1.height
            return leftArea == rightArea ? $0.width < $1.width : leftArea < rightArea
        }
    }

    public static func computeHDRMetrics(maxPotentialEDR: CGFloat, currentEDR: CGFloat, referenceEDR: CGFloat,
        isBuiltin: Bool, builtinCurrentSDRNits: Double?, builtinRealtimePeakNits: Double?) ->
        (hdrSupported: Bool, maxLuminanceNits: UInt16, maxFrameAverageLuminanceNits: UInt16,
         minLuminance10000ths: UInt16, sdrReferenceNits: UInt16) {
        guard maxPotentialEDR > 1.05 else { return (false, 0, 0, 0, 0) }
        let sdr: Double, peak: Double
        if referenceEDR > 1.05 {
            sdr = 100
            peak = Double(referenceEDR) * 100
        } else if isBuiltin {
            peak = builtinRealtimePeakNits.flatMap { (200...10000).contains($0) ? $0 : nil }
                ?? min(1600, max(400, Double(maxPotentialEDR) * 100))
            sdr = builtinCurrentSDRNits.flatMap { (10...1000).contains($0) ? $0 : nil } ?? 203
        } else {
            sdr = 203
            peak = min(4000, max(400, max(Double(maxPotentialEDR) * 100, Double(currentEDR) * 203)))
        }
        let maxNits = UInt16(min(Double(UInt16.max), peak.rounded()))
        return (true, maxNits, min(maxNits, 1000), 50, UInt16(sdr.rounded()))
    }

    private static func builtinBrightness(displayID: UInt32) -> (Double?, Double?) {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMobileFramebuffer"), &iterator) == KERN_SUCCESS else { return (nil, nil) }
        defer { IOObjectRelease(iterator) }
        var fallback: (Double?, Double?)?
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            var properties: Unmanaged<CFMutableDictionary>?
            guard IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) == KERN_SUCCESS,
                  let values = properties?.takeRetainedValue() as? [String: Any] else { continue }
            let product = (values["DisplayAttributes"] as? [String: Any])?["ProductAttributes"] as? [String: Any]
            func fixed(_ key: String) -> Double? { (values[key] as? NSNumber).map { $0.doubleValue / 65536.0 } }
            let peak = ["RTPLCBLNitsCap", "limit_max_physical_brightness"]
                .compactMap { fixed($0) }.first { (200...10000).contains($0) }
            let brightness = (fixed("IOMFBBrightnessLevel"), peak)
            if let vendor = product?["LegacyManufacturerID"] as? NSNumber,
               vendor.uint32Value == CGDisplayVendorNumber(displayID) { return brightness }
            if fallback == nil, (values["IONameMatched"] as? String)?.hasPrefix("disp0") == true { fallback = brightness }
        }
        return fallback ?? (nil, nil)
    }

    public static func screenID(_ screen: NSScreen) -> UInt32? {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
    }

    public static func detect(displayID: UInt32? = nil) -> HostDisplayCapabilities? {
        let screen = displayID.flatMap { id in NSScreen.screens.first { screenID($0) == id } } ?? NSScreen.main
        guard let screen, let id = screenID(screen) else { return nil }
        let modes = (CGDisplayCopyAllDisplayModes(id, nil) as? [CGDisplayMode] ?? [])
            .filter { $0.isUsableForDesktopGUI() }
        let native = modes.filter { $0.ioFlags & UInt32(kDisplayModeNativeFlag) != 0 }
        guard let largest = (native.isEmpty ? modes : native).max(by: {
            $0.pixelWidth * $0.pixelHeight < $1.pixelWidth * $1.pixelHeight
        }) else { return nil }
        let maximum = DisplayResolution(width: largest.pixelWidth, height: largest.pixelHeight)
        let rates = modes.map { Int($0.refreshRate.rounded()) }.filter { $0 > 0 }
        let isBuiltin = CGDisplayIsBuiltin(id) != 0
        let brightness = isBuiltin ? builtinBrightness(displayID: id) : (nil, nil)
        return HostDisplayCapabilities(
            id: id, name: screen.localizedName, maximumResolution: maximum,
            maximumRefreshRate: max(screen.maximumFramesPerSecond, rates.max() ?? 0),
            availableResolutions: modes.flatMap {
                [DisplayResolution(width: $0.width, height: $0.height),
                 DisplayResolution(width: $0.pixelWidth, height: $0.pixelHeight)]
            }, availableRefreshRates: rates, maxPotentialEDR: screen.maximumPotentialExtendedDynamicRangeColorComponentValue,
            currentEDR: screen.maximumExtendedDynamicRangeColorComponentValue,
            referenceEDR: screen.maximumReferenceExtendedDynamicRangeColorComponentValue, isBuiltin: isBuiltin,
            builtinCurrentSDRNits: brightness.0, builtinRealtimePeakNits: brightness.1
        )
    }

    public func refreshRates(for resolution: DisplayResolution) -> [Int] {
        let rates = Self.commonRefreshRates + nativeRefreshRates + [maximumRefreshRate]
        return Set(rates).filter { rate in
            guard rate > 0, rate <= maximumRefreshRate else { return false }
            return (try? VMDisplayConfiguration(width: resolution.width, height: resolution.height, refreshRate: rate).validate()) != nil
        }.sorted()
    }

    public func validate(_ mode: VMDisplayConfiguration) throws {
        try mode.validate()
        guard mode.width <= maximumResolution.width, mode.height <= maximumResolution.height else {
            throw DisplayError(String(localized: "Resolution cannot exceed \(name)’s maximum of \(maximumResolution.label)."))
        }
        guard mode.refreshRate <= maximumRefreshRate else {
            throw DisplayError(String(localized: "Refresh rate cannot exceed \(name)’s maximum of \(maximumRefreshRate) Hz."))
        }
    }

    /// Keeps the requested aspect ratio where possible when a screen or its limits change.
    public func preferredMode(for requested: VMDisplayConfiguration) -> VMDisplayConfiguration? {
        let aspect = Double(requested.width) / Double(max(1, requested.height))
        let requestedArea = Double(requested.width) * Double(requested.height)
        guard let resolution = resolutions.min(by: { left, right in
            let leftAspect = abs(Double(left.width) / Double(left.height) - aspect)
            let rightAspect = abs(Double(right.width) / Double(right.height) - aspect)
            if abs(leftAspect - rightAspect) > 0.001 { return leftAspect < rightAspect }
            return abs(Double(left.width * left.height) - requestedArea) < abs(Double(right.width * right.height) - requestedArea)
        }) else { return nil }
        let rates = refreshRates(for: resolution)
        guard let rate = rates.last(where: { $0 <= requested.refreshRate }) ?? rates.first else { return nil }
        return VMDisplayConfiguration(width: resolution.width, height: resolution.height, refreshRate: rate)
    }

    private struct DisplayError: LocalizedError {
        let message: String
        init(_ message: String) { self.message = message }
        var errorDescription: String? { message }
    }

    private static let commonResolutions: [DisplayResolution] = [
        (800, 600), (1024, 768), (1280, 720), (1280, 800), (1280, 1024), (1366, 768),
        (1440, 900), (1600, 900), (1600, 1000), (1680, 1050), (1920, 1080), (1920, 1200),
        (2048, 1152), (2560, 1080), (2560, 1440), (2560, 1600), (2880, 1800), (3200, 1800),
        (3440, 1440), (3840, 1600), (3840, 2160),
    ].map { DisplayResolution(width: $0.0, height: $0.1) }

    private static let commonRefreshRates = [24, 25, 30, 40, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240]
}
