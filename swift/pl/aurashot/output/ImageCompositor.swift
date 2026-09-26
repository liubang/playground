import AppKit

/// Bitmap-context factory shared by the output stage (ImageCompositor,
/// FrameDecorator).
///
/// The context inherits the SOURCE image's color space (Display P3 on
/// wide-gamut screens) instead of flattening to device RGB: re-rendering
/// a P3 screenshot into an sRGB-ish space visibly clips saturated
/// colors. AppKit drawing still works — NSGraphicsContext wraps the
/// CGContext with flipped: false, the same y-up semantics the old
/// NSBitmapImageRep contexts had, so every draw call behaves as before.
enum BitmapContext {
    static func make(width: Int, height: Int, colorSpace: CGColorSpace?) -> CGContext? {
        guard width > 0, height > 0 else { return nil }
        let space = colorSpace
            ?? CGColorSpace(name: CGColorSpace.sRGB)
            ?? CGColorSpaceCreateDeviceRGB()
        return CGContext(
            data: nil,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: 0,
            space: space,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue,
        )
    }
}

/// Renders the final output bitmap: the cropped snapshot plus all
/// annotations (design doc §6.5 export path).
///
/// `base` is the snapshot ALREADY cropped to `crop` (a pixel rect
/// inside the source image). Annotations carry view-local (screen)
/// point coordinates; the bitmap context maps them with the crop's
/// EXACT pixel origin and the display scale, so fractional selections
/// and outward-rounded crop edges can't desynchronize the two — the
/// annotations land sub-pixel-exactly where the user saw them.
enum ImageCompositor {
    /// - Parameters:
    ///   - base: snapshot cropped to `crop` (pixel space).
    ///   - crop: the pixel rect that was cropped out of the source image.
    ///   - imageHeight: pixel height of the FULL source image (needed
    ///     because `crop` is y-down while annotations are y-up).
    ///   - scale: points → pixels multiplier of the source display.
    static func composite(
        base: CGImage,
        crop: CGRect,
        imageHeight: Int,
        scale: CGFloat,
        annotations: [Annotation],
    ) -> CGImage? {
        let pixelW = base.width
        let pixelH = base.height
        guard pixelW > 0, pixelH > 0, scale > 0 else { return nil }
        guard let cgContext = BitmapContext.make(
            width: pixelW,
            height: pixelH,
            colorSpace: base.colorSpace,
        ) else { return nil }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: cgContext, flipped: false)

        // Base image 1:1 in pixel space.
        NSImage(cgImage: base, size: CGSize(width: pixelW, height: pixelH))
            .draw(in: CGRect(x: 0, y: 0, width: pixelW, height: pixelH))

        // Annotations: a view point p maps to pixel
        //   (p.x * scale - crop.minX,  p.y * scale - y0)
        // with y0 = imageHeight - crop.maxY (y-axis flip between the
        // y-down crop rect and the y-up bitmap/view space). Post-
        // multiplied CTM (scale first, then translate) implements
        // exactly that.
        if !annotations.isEmpty {
            let y0 = CGFloat(imageHeight) - crop.maxY
            cgContext.scaleBy(x: scale, y: scale)
            cgContext.translateBy(x: -crop.minX / scale, y: -y0 / scale)
            for annotation in annotations {
                annotation.draw()
            }
        }

        NSGraphicsContext.restoreGraphicsState()
        return cgContext.makeImage()
    }
}
