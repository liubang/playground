import AppKit

/// Output framing: the capture floats on a transparent canvas with
/// a soft drop shadow and a 1px hairline border, so a copied/saved
/// shot reads as a bordered "card" instead of a raw crop (design doc
/// §6.6 output cosmetics).
///
/// All geometry is specified in POINTS and multiplied by `scale`
/// (pixels per point, ≈ the display's backing factor) so Retina and
/// 1x captures get the same visual weight. The transparent margin is
/// what makes the shadow paste correctly onto any background.
enum FrameDecorator {
    /// Transparent margin around the capture, in points. Must
    /// comfortably contain the shadow blur plus its offset.
    static let padding: CGFloat = 40
    static let shadowBlur: CGFloat = 20
    static let shadowOffsetY: CGFloat = -6
    static let shadowAlpha: CGFloat = 0.55

    static func apply(to image: CGImage, scale: CGFloat) -> CGImage? {
        let scale = max(scale, 0.5)
        let pad = (padding * scale).rounded()
        let outW = image.width + Int(pad) * 2
        let outH = image.height + Int(pad) * 2
        guard outW > 0, outH > 0,
              let rep = NSBitmapImageRep(
                  bitmapDataPlanes: nil,
                  pixelsWide: outW,
                  pixelsHigh: outH,
                  bitsPerSample: 8,
                  samplesPerPixel: 4,
                  hasAlpha: true,
                  isPlanar: false,
                  colorSpaceName: .deviceRGB,
                  bytesPerRow: 0,
                  bitsPerPixel: 0,
              ) else { return nil }

        let contentRect = CGRect(
            x: pad, y: pad,
            width: CGFloat(image.width), height: CGFloat(image.height),
        )

        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

        // An opaque fill under an NSShadow paints the drop shadow into
        // the transparent margin. The save/restore keeps the shadow
        // attribute from leaking into the capture/border passes below.
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(shadowAlpha)
        shadow.shadowBlurRadius = shadowBlur * scale
        shadow.shadowOffset = NSSize(width: 0, height: shadowOffsetY * scale)
        shadow.set()
        NSColor.white.setFill()
        NSBezierPath(rect: contentRect).fill()
        NSGraphicsContext.restoreGraphicsState()

        // The capture itself, 1:1 in pixel space (same NSImage path as
        // ImageCompositor, which keeps the orientation intact).
        NSImage(cgImage: image, size: contentRect.size).draw(in: contentRect)

        // Hairline border hugging the capture's outermost pixels.
        NSColor.black.withAlphaComponent(0.15).setStroke()
        let border = NSBezierPath(rect: contentRect)
        border.lineWidth = max(1, scale)
        border.stroke()

        NSGraphicsContext.restoreGraphicsState()
        return rep.cgImage
    }
}
