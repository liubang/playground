import AppKit

/// Renders the final output bitmap: the cropped snapshot plus all
/// annotations (design doc §6.5 export path).
///
/// `base` is the snapshot ALREADY cropped to the selection's pixel
/// rect — its pixel dimensions are the ground truth. Annotations carry
/// view-local (screen) point coordinates; the bitmap context maps them
/// with the EFFECTIVE scale (pixels / points) so fractional selections
/// can't desynchronize the two, and their draw() calls land exactly
/// where the user saw them, at full pixel resolution.
enum ImageCompositor {
    static func composite(
        base: CGImage,
        selectionPoints: CGRect,
        annotations: [Annotation],
    ) -> CGImage? {
        let pixelW = base.width
        let pixelH = base.height
        guard pixelW > 0, pixelH > 0,
              selectionPoints.width > 0, selectionPoints.height > 0 else { return nil }
        guard let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: pixelW,
            pixelsHigh: pixelH,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0,
        ) else { return nil }

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

        // Base image 1:1 in pixel space.
        NSImage(cgImage: base, size: CGSize(width: pixelW, height: pixelH))
            .draw(in: CGRect(x: 0, y: 0, width: pixelW, height: pixelH))

        // Annotations: view points → selection-local → pixels, using
        // the effective per-axis scale. Post-multiplied CTM (scale
        // first, then translate) gives p → (p - origin) * scale.
        if !annotations.isEmpty, let context = NSGraphicsContext.current?.cgContext {
            context.scaleBy(
                x: CGFloat(pixelW) / selectionPoints.width,
                y: CGFloat(pixelH) / selectionPoints.height,
            )
            context.translateBy(x: -selectionPoints.minX, y: -selectionPoints.minY)
            for annotation in annotations {
                annotation.draw()
            }
        }

        NSGraphicsContext.restoreGraphicsState()
        return rep.cgImage
    }
}
