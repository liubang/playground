import AppKit

// Generates resources/AppIcon.icns: an Everforest-styled aurora ring.
//
// Usage: swift tools/make-icon.swift [output.icns]
//
// The icon: Everforest Dark Hard gradient background with a soft center
// glow, and a thick "aura" ring in a teal→aqua→green gradient — the same
// donut-gauge language the app uses for its battery / CPU / memory menu
// bar glyphs. A gap at the bottom-right with an accent dot keeps it from
// looking like a plain circle.

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

func drawIcon(size: CGFloat) -> NSImage {
    NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
        // Background: macOS-style rounded rect with a vertical gradient.
        let bgPath = NSBezierPath(roundedRect: rect, xRadius: size * 0.2237, yRadius: size * 0.2237)
        NSGraphicsContext.saveGraphicsState()
        bgPath.addClip()
        NSGradient(colors: [bgTop, bgBottom])?.draw(in: rect, angle: -90)
        NSGraphicsContext.restoreGraphicsState()

        // Soft radial glow behind the ring, like light bleeding out of
        // the aura.
        let glowRect = rect.insetBy(dx: size * 0.18, dy: size * 0.18)
        let glow = NSGradient(
            colors: [
                teal.withAlphaComponent(0.35),
                teal.withAlphaComponent(0.0),
            ],
        )
        glow?.draw(in: glowRect, relativeCenterPosition: .zero)

        // The aura ring. Angles are measured counterclockwise from the
        // +x axis: sweeping 300° counterclockwise from -20° to 320°
        // leaves a 60° gap on the right, between -20° and +40°. A short
        // aqua overlay near the trailing end fakes a gradient without
        // clip-path tricks (an open arc's implicit clip region leaks).
        let center = NSPoint(x: rect.midX, y: rect.midY)
        let ringRadius = size * 0.32
        let ringWidth = size * 0.105

        func arc(_ start: CGFloat, _ end: CGFloat) -> NSBezierPath {
            let path = NSBezierPath()
            path.appendArc(withCenter: center, radius: ringRadius, startAngle: start, endAngle: end, clockwise: false)
            path.lineWidth = ringWidth
            path.lineCapStyle = .round
            return path
        }

        teal.setStroke()
        arc(-20, 320).stroke()
        // A second teal pass over the bottom half with a softer alpha
        // fakes a subtle gradient without an overlay stroke's cap
        // artifact. The ring reads teal up top, mistier at the bottom.
        aqua.withAlphaComponent(0.35).setStroke()
        arc(160, 300).stroke()

        // Accent dot at the gap's upper end (40° position).
        let dotAngle = CGFloat(40) * .pi / 180
        let dotCenter = NSPoint(
            x: center.x + ringRadius * cos(dotAngle),
            y: center.y + ringRadius * sin(dotAngle),
        )
        let dotSide = ringWidth * 1.25
        let dotRect = NSRect(
            x: dotCenter.x - dotSide / 2,
            y: dotCenter.y - dotSide / 2,
            width: dotSide,
            height: dotSide,
        )
        green.setFill()
        NSBezierPath(ovalIn: dotRect).fill()

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
let iconset = NSTemporaryDirectory() + "aurabar.iconset"
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
