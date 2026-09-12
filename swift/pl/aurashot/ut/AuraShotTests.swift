import AppKit
@testable import AuraShot
import XCTest

/// Pure-geometry tests: the crop math from design doc §6.2 (outward
/// rounding, y-flip, image-bounds clamping) without needing NSScreen.
final class CropGeometryTests: XCTestCase {
    func testOutwardRoundingOnFractionalSelection() {
        let crop = DisplayContext.cropRectPixels(
            selectionPoints: CGRect(x: 10.3, y: 20.6, width: 100.2, height: 50.4),
            scale: 2,
            imageWidth: 2000,
            imageHeight: 1000,
        )
        // x0 = floor(20.6) = 20, x1 = ceil(221.0) = 221 → width 201
        // y0 = floor(41.2) = 41, y1 = ceil(142.0) = 142 → height 101,
        // and the y axis flips: cropY = imageHeight - y1.
        XCTAssertEqual(crop, CGRect(x: 20, y: 858, width: 201, height: 101))
    }

    func testCropClampedToImageBounds() {
        let crop = DisplayContext.cropRectPixels(
            selectionPoints: CGRect(x: -50, y: -50, width: 200, height: 200),
            scale: 1,
            imageWidth: 500,
            imageHeight: 500,
        )
        // y0 = -50, y1 = 150 → cropY = 500 - 150 = 350; x clamped at 0.
        XCTAssertEqual(crop, CGRect(x: 0, y: 350, width: 150, height: 150))
    }

    func testIntegralSelectionIsExact() {
        let crop = DisplayContext.cropRectPixels(
            selectionPoints: CGRect(x: 100, y: 100, width: 200, height: 150),
            scale: 2,
            imageWidth: 4000,
            imageHeight: 3000,
        )
        XCTAssertEqual(crop, CGRect(x: 200, y: 2500, width: 400, height: 300))
    }
}

/// Annotations must ride along when the selection moves or resizes.
final class AnnotationTransformTests: XCTestCase {
    func testTranslated() {
        let annotation = Annotation(
            tool: .rectangle,
            start: CGPoint(x: 10, y: 10),
            end: CGPoint(x: 110, y: 60),
        )
        let moved = annotation.translated(by: CGPoint(x: 5, y: -8))
        XCTAssertEqual(moved.start, CGPoint(x: 15, y: 2))
        XCTAssertEqual(moved.end, CGPoint(x: 115, y: 52))
    }

    func testTranslatedFreehandMovesEveryPoint() {
        var annotation = Annotation(tool: .freehand, start: .zero, end: .zero)
        annotation.points = [CGPoint(x: 1, y: 1), CGPoint(x: 5, y: 9)]
        let moved = annotation.translated(by: CGPoint(x: 10, y: 10))
        XCTAssertEqual(moved.points, [CGPoint(x: 11, y: 11), CGPoint(x: 15, y: 19)])
    }

    func testMappedScalesIntoNewRect() {
        let annotation = Annotation(
            tool: .rectangle,
            start: CGPoint(x: 100, y: 100),
            end: CGPoint(x: 200, y: 200),
        )
        // Source rect doubles in size and shifts right by 100.
        let mapped = annotation.mapped(
            from: CGRect(x: 100, y: 100, width: 100, height: 100),
            to: CGRect(x: 200, y: 100, width: 200, height: 200),
        )
        XCTAssertEqual(mapped.start, CGPoint(x: 200, y: 100))
        XCTAssertEqual(mapped.end, CGPoint(x: 400, y: 300))
    }

    func testMappedWithDegenerateSourceIsIdentity() {
        let annotation = Annotation(
            tool: .arrow,
            start: CGPoint(x: 1, y: 2),
            end: CGPoint(x: 3, y: 4),
        )
        let mapped = annotation.mapped(from: .zero, to: CGRect(x: 0, y: 0, width: 10, height: 10))
        XCTAssertEqual(mapped.start, annotation.start)
        XCTAssertEqual(mapped.end, annotation.end)
    }

    func testMosaicScaleRoundTripsThroughTransforms() {
        var annotation = Annotation(tool: .mosaic, start: .zero, end: CGPoint(x: 10, y: 10))
        annotation.mosaicScale = 1.5
        let moved = annotation.translated(by: CGPoint(x: 1, y: 1))
            .mapped(from: CGRect(x: 0, y: 0, width: 100, height: 100),
                    to: CGRect(x: 0, y: 0, width: 200, height: 200))
        XCTAssertEqual(moved.mosaicScale, 1.5)
    }
}

final class KeyComboTests: XCTestCase {
    func testDisplayStringOrdersModifiers() {
        let display = KeyCombo.displayString(
            carbonModifiers: Carbon.Modifier.cmd | Carbon.Modifier.shift,
            keyCharacter: "x",
        )
        XCTAssertEqual(display, "⇧⌘X")
    }

    func testDisplayStringFullModifierSet() {
        let display = KeyCombo.displayString(
            carbonModifiers: Carbon.Modifier.control | Carbon.Modifier.option
                | Carbon.Modifier.shift | Carbon.Modifier.cmd,
            keyCharacter: "p",
        )
        XCTAssertEqual(display, "⌃⌥⇧⌘P")
    }

    func testHotkeyConflictRequiresSameKeyAndModifiers() {
        let a = KeyCombo(keyCode: 7, carbonModifiers: Carbon.Modifier.cmd, display: "")
        let same = KeyCombo(keyCode: 7, carbonModifiers: Carbon.Modifier.cmd, display: "different")
        let otherKey = KeyCombo(keyCode: 8, carbonModifiers: Carbon.Modifier.cmd, display: "")
        let otherMods = KeyCombo(keyCode: 7, carbonModifiers: Carbon.Modifier.cmd | Carbon.Modifier.shift, display: "")
        XCTAssertTrue(Settings.hotkeysConflict(a, same))
        XCTAssertFalse(Settings.hotkeysConflict(a, otherKey))
        XCTAssertFalse(Settings.hotkeysConflict(a, otherMods))
    }
}

final class FilenamePatternTests: XCTestCase {
    func testDefaultPatternRenders() {
        let name = Settings.renderFileName(
            pattern: Settings.defaultFilenamePattern,
            date: Date(timeIntervalSince1970: 1_700_000_000),
        )
        XCTAssertTrue(name.hasPrefix("AuraShot-"))
        XCTAssertTrue(name.hasSuffix(".png"))
        XCTAssertEqual(name.count, "AuraShot-20231114-221313.png".count)
    }

    func testUnsafeCharactersAreSanitized() {
        let name = Settings.renderFileName(pattern: "yyyy/MM/dd HH:mm:ss")
        XCTAssertFalse(name.contains("/"))
        XCTAssertFalse(name.contains(":"))
        XCTAssertTrue(name.hasSuffix(".png"))
    }

    func testEmptyPatternFallsBack() {
        let name = Settings.renderFileName(pattern: "''")
        XCTAssertFalse(name.isEmpty)
        XCTAssertTrue(name.hasSuffix(".png"))
    }
}
