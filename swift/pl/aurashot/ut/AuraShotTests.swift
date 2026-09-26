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

/// CG↔NS coordinate conversions (design doc §6.2: "单元测试覆盖（含
/// 主屏在左/右/上、负坐标排列）"). The pure primaryHeight-taking
/// overloads keep NSScreen out of the test process.
final class CoordinateSpaceTests: XCTestCase {
    private let primary: CGFloat = 1440

    func testPointRoundTrip() {
        let cg = CGPoint(x: 500, y: 300)
        let ns = CoordinateSpace.pointToNS(cg, primaryHeight: primary)
        XCTAssertEqual(ns, NSPoint(x: 500, y: 1140))
        XCTAssertEqual(CoordinateSpace.pointToCG(ns, primaryHeight: primary), cg)
    }

    func testNegativeCoordinatesForLeftSecondaryDisplay() {
        // A display left of the primary has negative x in BOTH spaces.
        let cg = CGPoint(x: -1200, y: 100)
        let ns = CoordinateSpace.pointToNS(cg, primaryHeight: primary)
        XCTAssertEqual(ns, NSPoint(x: -1200, y: 1340))
        XCTAssertEqual(CoordinateSpace.pointToCG(ns, primaryHeight: primary), cg)
    }

    func testRectFlipUsesMaxY() {
        // CG rect (y-down) spans y 50...150 → NS (y-up) minY = 1440-150.
        let cg = CGRect(x: 100, y: 50, width: 200, height: 100)
        let ns = CoordinateSpace.rectToNS(cg, primaryHeight: primary)
        XCTAssertEqual(ns, NSRect(x: 100, y: 1290, width: 200, height: 100))
        XCTAssertEqual(CoordinateSpace.rectToCG(ns, primaryHeight: primary), cg)
    }

    func testRectAbovePrimary() {
        // A display stacked ABOVE the primary has negative CG y and an
        // NS minY starting at primaryHeight.
        let cg = CGRect(x: 0, y: -1080, width: 1920, height: 1080)
        let ns = CoordinateSpace.rectToNS(cg, primaryHeight: primary)
        XCTAssertEqual(ns, NSRect(x: 0, y: 1440, width: 1920, height: 1080))
        XCTAssertEqual(CoordinateSpace.rectToCG(ns, primaryHeight: primary), cg)
    }
}

// MARK: - Bitmap helpers for the output-stage tests

/// A solid-color CGImage in the given color space (Quartz y-up).
private func makeSolidImage(
    width: Int,
    height: Int,
    r: CGFloat,
    g: CGFloat,
    b: CGFloat,
    space: CGColorSpace,
) -> CGImage {
    let context = CGContext(
        data: nil, width: width, height: height,
        bitsPerComponent: 8, bytesPerRow: 0, space: space,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue,
    )!
    // NB: setFillColor(_ components:) silently no-ops here (the fresh
    // context's default fill color space isn't RGB); an explicit CGColor
    // in the context's own space is the reliable form.
    context.setFillColor(CGColor(colorSpace: space, components: [r, g, b, 1])!)
    context.fill(CGRect(x: 0, y: 0, width: width, height: height))
    return context.makeImage()!
}

/// Reads a pixel at QUARTZ (y-up) coordinates, tolerating no layout
/// surprises: bitmap memory is top-row-first, so memory row = h-1-y.
private func pixel(of image: CGImage, x: Int, y: Int) -> (r: Int, g: Int, b: Int, a: Int) {
    let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
    let context = CGContext(
        data: nil, width: image.width, height: image.height,
        bitsPerComponent: 8, bytesPerRow: 0, space: space,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue,
    )!
    context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
    let data = context.data!.assumingMemoryBound(to: UInt8.self)
    let offset = ((image.height - 1 - y) * image.width + x) * 4
    return (Int(data[offset]), Int(data[offset + 1]), Int(data[offset + 2]), Int(data[offset + 3]))
}

private let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!

private func isReddish(_ p: (r: Int, g: Int, b: Int, a: Int)) -> Bool {
    p.r > 180 && p.g < 140 && p.b < 140
}

private func isBluish(_ p: (r: Int, g: Int, b: Int, a: Int)) -> Bool {
    p.b > 200 && p.r < 80
}

/// Output-stage geometry: annotations must land where the user saw
/// them (crop offset + y-flip), and the output must keep the source
/// image's color space (Display P3 screenshots must not flatten to
/// device RGB).
final class ImageCompositorTests: XCTestCase {
    private func blueBase(_ size: Int = 8) -> CGImage {
        makeSolidImage(width: size, height: size, r: 0, g: 0, b: 1, space: sRGB)
    }

    func testStrokeLandsAtExpectedQuartzPosition() {
        let base = blueBase()
        let annotation = Annotation(
            tool: .rectangle,
            start: CGPoint(x: 1, y: 1),
            end: CGPoint(x: 7, y: 7),
        )
        let output = ImageCompositor.composite(
            base: base,
            crop: CGRect(x: 0, y: 0, width: 8, height: 8),
            imageHeight: 8,
            scale: 1,
            annotations: [annotation],
        )
        XCTAssertNotNil(output)
        // Left-edge midpoint is on the stroke…
        XCTAssertTrue(isReddish(pixel(of: output!, x: 1, y: 4)))
        // …and the interior is untouched base. (The 2.5pt stroke
        // legitimately spills onto the image's outermost pixels from
        // the rect's corners, so no corner assertions here.)
        XCTAssertTrue(isBluish(pixel(of: output!, x: 4, y: 4)))
        XCTAssertTrue(isBluish(pixel(of: output!, x: 3, y: 3)))
    }

    func testHorizontalLineLocksYFlip() {
        let base = blueBase()
        // A horizontal line near the TOP of y-up space (y = 7): a
        // y-flip bug would mirror it to y = 0/1.
        let annotation = Annotation(
            tool: .line,
            start: CGPoint(x: 1, y: 7),
            end: CGPoint(x: 7, y: 7),
        )
        let output = ImageCompositor.composite(
            base: base,
            crop: CGRect(x: 0, y: 0, width: 8, height: 8),
            imageHeight: 8,
            scale: 1,
            annotations: [annotation],
        )!
        XCTAssertTrue(isReddish(pixel(of: output, x: 4, y: 7)))
        XCTAssertTrue(isBluish(pixel(of: output, x: 4, y: 1)))
    }

    func testCropOffsetAndY0Translation() {
        // Base is the 8×8 crop TAKEN FROM pixel rect (4,4,8,8) of a
        // 16×16 source. An annotation at view (5,5)-(11,11) must land
        // on output pixels (1,1)-(7,7): translate(-crop.minX, -y0)
        // with y0 = imageHeight - crop.maxY = 16 - 12 = 4.
        let base = blueBase()
        let annotation = Annotation(
            tool: .rectangle,
            start: CGPoint(x: 5, y: 5),
            end: CGPoint(x: 11, y: 11),
        )
        let output = ImageCompositor.composite(
            base: base,
            crop: CGRect(x: 4, y: 4, width: 8, height: 8),
            imageHeight: 16,
            scale: 1,
            annotations: [annotation],
        )!
        XCTAssertTrue(isReddish(pixel(of: output, x: 1, y: 4)))
        XCTAssertTrue(isBluish(pixel(of: output, x: 4, y: 4)))
        XCTAssertTrue(isBluish(pixel(of: output, x: 3, y: 3)))
    }

    func testScaleMultipliesAnnotationCoordinates() {
        // scale 2: view point (2,2)-(6,6) → pixels (4,4)-(12,12); the
        // 2.5pt stroke scales to 5px with the CTM. (The rect must be
        // big enough that a clear interior survives the scaled stroke.)
        let base = makeSolidImage(width: 16, height: 16, r: 0, g: 0, b: 1, space: sRGB)
        let annotation = Annotation(
            tool: .rectangle,
            start: CGPoint(x: 2, y: 2),
            end: CGPoint(x: 6, y: 6),
        )
        let output = ImageCompositor.composite(
            base: base,
            crop: CGRect(x: 0, y: 0, width: 16, height: 16),
            imageHeight: 16,
            scale: 2,
            annotations: [annotation],
        )!
        XCTAssertTrue(isReddish(pixel(of: output, x: 4, y: 8)))
        XCTAssertTrue(isBluish(pixel(of: output, x: 8, y: 8)))
    }

    func testOutputPreservesSourceColorSpace() {
        let p3 = CGColorSpace(name: CGColorSpace.displayP3)!
        let base = makeSolidImage(width: 8, height: 8, r: 1, g: 0, b: 0, space: p3)
        let output = ImageCompositor.composite(
            base: base,
            crop: CGRect(x: 0, y: 0, width: 8, height: 8),
            imageHeight: 8,
            scale: 1,
            annotations: [],
        )!
        XCTAssertEqual(output.colorSpace, base.colorSpace)
    }
}

final class FrameDecoratorTests: XCTestCase {
    func testPaddingTransparencyAndCenterPixel() {
        let base = makeSolidImage(width: 8, height: 8, r: 0, g: 0, b: 1, space: sRGB)
        let output = FrameDecorator.apply(to: base, scale: 1)!
        // pad = 40pt × scale 1 on each side.
        XCTAssertEqual(output.width, 8 + 80)
        XCTAssertEqual(output.height, 8 + 80)
        // Far corner stays transparent (shadow must not reach it).
        XCTAssertLessThan(pixel(of: output, x: 0, y: 0).a, 5)
        // The capture sits centered, unmodified.
        XCTAssertTrue(isBluish(pixel(of: output, x: 44, y: 44)))
    }

    func testScaleScalesPadding() {
        let base = makeSolidImage(width: 8, height: 8, r: 0, g: 0, b: 1, space: sRGB)
        let output = FrameDecorator.apply(to: base, scale: 2)!
        XCTAssertEqual(output.width, 8 + 160)
        XCTAssertEqual(output.height, 8 + 160)
        XCTAssertTrue(isBluish(pixel(of: output, x: 84, y: 84)))
    }

    func testOutputPreservesSourceColorSpace() {
        let p3 = CGColorSpace(name: CGColorSpace.displayP3)!
        let base = makeSolidImage(width: 8, height: 8, r: 1, g: 0, b: 0, space: p3)
        let output = FrameDecorator.apply(to: base, scale: 1)!
        XCTAssertEqual(output.colorSpace, base.colorSpace)
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
