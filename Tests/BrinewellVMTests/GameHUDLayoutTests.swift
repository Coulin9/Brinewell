import AppKit
import BrinewellCore
import XCTest
@testable import BrinewellVMRuntime

@MainActor
final class GameHUDLayoutTests: XCTestCase {
    private func presentation(level: GameHUDLevel = .full, headline: String = "60.0 FPS") -> GameHUDPresentation {
        GameHUDPresentation(level: level, headline: headline,
                            rows: level == .fps ? [] : (0..<20).map { .init(title: "指标 \($0)", value: "—") },
                            frameTimes: [], cores: level == .full ? Array(repeating: "P0.0  25%  4.10G  62°C", count: 18) : [])
    }

    private func container(size: NSSize) -> NSView {
        let container = NSView(frame: NSRect(origin: .zero, size: size))
        container.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            container.widthAnchor.constraint(equalToConstant: size.width),
            container.heightAnchor.constraint(equalToConstant: size.height),
        ])
        return container
    }

    private func place(_ hud: GamePerformanceHUD, in container: NSView, position: GameHUDPosition) {
        container.addSubview(hud)
        let left = position == .topLeft || position == .bottomLeft
        let top = position == .topLeft || position == .topRight
        NSLayoutConstraint.activate([
            hud.widthAnchor.constraint(lessThanOrEqualTo: container.widthAnchor, constant: -32),
            hud.heightAnchor.constraint(lessThanOrEqualTo: container.heightAnchor, constant: -32),
            left ? hud.leadingAnchor.constraint(equalTo: container.leadingAnchor, constant: 16)
                 : hud.trailingAnchor.constraint(equalTo: container.trailingAnchor, constant: -16),
            top ? hud.topAnchor.constraint(equalTo: container.topAnchor, constant: 16)
                : hud.bottomAnchor.constraint(equalTo: container.bottomAnchor, constant: -16),
        ])
    }

    private func checkFrame(_ hud: GamePerformanceHUD, in container: NSView, position: GameHUDPosition,
                            file: StaticString = #filePath, line: UInt = #line) {
        container.layoutSubtreeIfNeeded()
        let frame = hud.frame
        let natural = hud.intrinsicContentSize
        XCTAssertGreaterThan(frame.width, 0, file: file, line: line)
        XCTAssertGreaterThan(frame.height, 0, file: file, line: line)
        let scale = min(1, (container.bounds.width - 32) / natural.width,
                        (container.bounds.height - 32) / natural.height)
        // These windowless AppKit views align their resulting frames to whole points.
        // Check the exact rounded fit, not an impossible subpixel aspect ratio.
        XCTAssertEqual(frame.width, (natural.width * scale).rounded(), accuracy: 0.01, file: file, line: line)
        XCTAssertEqual(frame.height, (natural.height * scale).rounded(), accuracy: 0.01, file: file, line: line)
        XCTAssertLessThanOrEqual(frame.width, container.bounds.width - 32 + 0.01, file: file, line: line)
        XCTAssertLessThanOrEqual(frame.height, container.bounds.height - 32 + 0.01, file: file, line: line)
        let left = position == .topLeft || position == .bottomLeft
        let top = position == .topLeft || position == .topRight
        XCTAssertEqual(left ? frame.minX : container.bounds.maxX - frame.maxX, 16, accuracy: 0.01, file: file, line: line)
        XCTAssertEqual(top ? container.bounds.maxY - frame.maxY : frame.minY, 16, accuracy: 0.01, file: file, line: line)
    }

    func testCompressedCardsKeepTheirRealFrameAtAllFourCorners() {
        for size in [NSSize(width: 640, height: 400), NSSize(width: 320, height: 800)] {
            for position in GameHUDPosition.allCases {
                let container = container(size: size)
                let hud = GamePerformanceHUD(frame: .zero)
                hud.update(presentation())
                place(hud, in: container, position: position)
                checkFrame(hud, in: container, position: position)
            }
        }
    }

    func testChangingLevelsReplacesAspectRatioWithoutMovingTheSelectedCorner() {
        let container = container(size: NSSize(width: 640, height: 400))
        let hud = GamePerformanceHUD(frame: .zero)
        place(hud, in: container, position: .bottomRight)
        for level in [GameHUDLevel.full, .fps, .detailed, .full] {
            hud.update(presentation(level: level))
            checkFrame(hud, in: container, position: .bottomRight)
            XCTAssertEqual(hud.constraints.filter { $0.isActive && $0.firstAttribute == .width && $0.secondAttribute == .height }.count, 1)
        }
    }

    func testNewReadingsDoNotRebuildUnchangedLayoutConstraints() throws {
        let hud = GamePerformanceHUD(frame: .zero)
        hud.update(presentation())
        let original = try XCTUnwrap(hud.constraints.first { $0.firstAttribute == .width && $0.secondAttribute == .height })
        hud.update(presentation(headline: "59.9 FPS"))
        XCTAssertTrue(original.isActive)
        XCTAssertTrue(hud.constraints.contains { $0 === original })
    }
}
