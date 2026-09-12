import AppKit

// Generates resources/AppIcon.icns for AuraShot.
//
// Usage: swift tools/make-icon.swift [output.icns]
//
// The icon: the same Everforest palette as AuraBar's ring icon (its
// menu-bar sibling), but the glyph is a screenshot crop frame — four
// corner marks framing a crosshair, with a confetti dot under the
// bottom-right corner as the "clip has landed" accent.

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> NSColor {
    NSColor(
        red: CGFloat((hex >> 16) & 0xFF) / 255,
        green: CGFloat((hex >> 8) & 0xFF) / 255,
        blue: CGFloat(hex & 0xFF) / 255,
        alpha: alpha,
    )
}

let bgTop = color(0x2D353B) // Everforest bg1
let bgBottom = color(0x141B1E) // Everforest bg_dim
let teal = color(0x7FBBB3) // Everforest teal — primary accent
let aqua = color(0x83C092) // Everforest aqua
let green = color(0xA7C080) // Everforest green

/// One crop-mark corner: an L made of two round-capped strokes.
func drawCorner(
    at corner: NSPoint,
    dx: CGFloat,
    dy: CGFloat,
    arm: CGFloat,
    width: CGFloat,
) {
    teal.setStroke()
    let path = NSBezierPath()
    path.lineWidth = width
    path.lineCapStyle = .round
    path.move(to: NSPoint(x: corner.x + dx * arm, y: corner.y))
    path.line(to: corner)
    path.line(to: NSPoint(x: corner.x, y: corner.y + dy * arm))
    path.stroke()
}

func drawIcon(size: CGFloat) -> NSImage {
    NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
        // Background: macOS-style rounded rect with a vertical gradient.
        let bgPath = NSBezierPath(roundedRect: rect, xRadius: size * 0.2237, yRadius: size * 0.2237)
        NSGraphicsContext.saveGraphicsState()
        bgPath.addClip()
        NSGradient(colors: [bgTop, bgBottom])?.draw(in: rect, angle: -90)
        NSGraphicsContext.restoreGraphicsState()

        // Soft radial glow behind the crop frame.
        let glowRect = rect.insetBy(dx: size * 0.18, dy: size * 0.18)
        let glow = NSGradient(
            colors: [
                teal.withAlphaComponent(0.30),
                teal.withAlphaComponent(0.0),
            ],
        )
        glow?.draw(in: glowRect, relativeCenterPosition: .zero)

        // Crop frame: four L corners around the center.
        let half = size * 0.26 // half-side of the framed region
        let arm = size * 0.13 // length of each L arm
        let width = size * 0.055
        let cx = rect.midX
        let cy = rect.midY

        drawCorner(at: NSPoint(x: cx - half, y: cy + half), dx: 1, dy: -1, arm: arm, width: width)
        drawCorner(at: NSPoint(x: cx + half, y: cy + half), dx: -1, dy: -1, arm: arm, width: width)
        drawCorner(at: NSPoint(x: cx - half, y: cy - half), dx: 1, dy: 1, arm: arm, width: width)
        drawCorner(at: NSPoint(x: cx + half, y: cy - half), dx: -1, dy: 1, arm: arm, width: width)

        // Crosshair at the center: a plus and a dot, in mistier aqua.
        let chLen = size * 0.10
        aqua.withAlphaComponent(0.9).setStroke()
        let plus = NSBezierPath()
        plus.lineWidth = width * 0.75
        plus.lineCapStyle = .round
        plus.move(to: NSPoint(x: cx - chLen, y: cy))
        plus.line(to: NSPoint(x: cx + chLen, y: cy))
        plus.move(to: NSPoint(x: cx, y: cy - chLen))
        plus.line(to: NSPoint(x: cx, y: cy + chLen))
        plus.stroke()

        teal.setFill()
        NSBezierPath(ovalIn: NSRect(
            x: cx - width * 0.8, y: cy - width * 0.8,
            width: width * 1.6, height: width * 1.6,
        )).fill()

        // Confetti dot under the bottom-right corner — same accent-dot
        // language as the AuraBar ring's gap dot.
        let dotCenter = NSPoint(x: cx + half + width * 1.2, y: cy - half - width * 1.2)
        let dotSide = width * 1.35
        green.setFill()
        NSBezierPath(ovalIn: NSRect(
            x: dotCenter.x - dotSide / 2,
            y: dotCenter.y - dotSide / 2,
            width: dotSide, height: dotSide,
        )).fill()

        return true
    }
}

func pngData(size: Int) -> Data? {
    guard let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0,
    ) else { return nil }
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    drawIcon(size: CGFloat(size)).draw(in: NSRect(x: 0, y: 0, width: size, height: size))
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])
}

let output = CommandLine.arguments.count > 1
    ? CommandLine.arguments[1]
    : "resources/AppIcon.icns"
let iconset = NSTemporaryDirectory() + "aurashot.iconset"
try? FileManager.default.removeItem(atPath: iconset)
try FileManager.default.createDirectory(atPath: iconset, withIntermediateDirectories: true)

let variants: [(pixels: Int, name: String)] = [
    (16, "icon_16x16.png"), (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"), (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"), (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"), (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"), (1024, "icon_512x512@2x.png"),
]
for variant in variants {
    guard let data = pngData(size: variant.pixels) else {
        fatalError("failed to render \(variant.name)")
    }
    try data.write(to: URL(fileURLWithPath: iconset + "/" + variant.name))
}

let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset, "-o", output]
try iconutil.run()
iconutil.waitUntilExit()
try? FileManager.default.removeItem(atPath: iconset)

guard iconutil.terminationStatus == 0 else {
    fatalError("iconutil failed with exit code \(iconutil.terminationStatus)")
}

print("wrote \(output)")
