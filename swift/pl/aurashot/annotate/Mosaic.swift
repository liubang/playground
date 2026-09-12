import AppKit

/// Pixellated-patch rendering for the mosaic annotation tool.
enum Mosaic {
    /// CIContext creation is comparatively expensive (it sets up the
    /// Metal pipeline); one shared instance serves every patch bake.
    private static let context = CIContext(options: [.useSoftwareRenderer: false])

    /// Renders a pixellated copy of the snapshot region under
    /// `viewRect` (view-local points) at the display's pixel
    /// resolution. Captured once at annotation commit time, then the
    /// patch is a plain NSImage — no re-filtering during redraw/export.
    static func patch(
        snapshot: CGImage,
        viewRect: CGRect,
        display: DisplayContext,
        scale: CGFloat = 1.0,
    ) -> NSImage? {
        let crop = display.cropRectPixels(forSelectionPoints: viewRect)
        guard crop.width >= 1, crop.height >= 1,
              let cropped = snapshot.cropping(to: crop) else { return nil }

        let input = CIImage(cgImage: cropped)
        guard let filter = CIFilter(name: "CIPixellate") else { return nil }
        filter.setValue(input, forKey: kCIInputImageKey)
        // Blocks scale with the region: always coarse enough to
        // anonymize, never degenerate for tiny rects. `scale` is the
        // palette's intensity knob.
        let block = min(max(crop.width, crop.height) / 24.0, 32) * scale
        filter.setValue(max(block, 6), forKey: kCIInputScaleKey)
        guard let output = filter.outputImage else { return nil }

        // CIPixellate's output extent can drift; crop back to the input.
        guard let cgImage = context.createCGImage(output, from: input.extent) else { return nil }
        return NSImage(cgImage: cgImage, size: viewRect.size)
    }
}
