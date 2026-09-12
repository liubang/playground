import AppKit
import CoreGraphics

// Generates resources/AppIcon.icns: an Everforest-styled aurora ring.
//
// Usage: swift tools/make-icon.swift [output.icns]
//
// The icon: Everforest Dark Hard gradient background with a soft center
// glow, and a thick "aura" ring in a teal→aqua gradient — the same
// donut-gauge language the app uses for its battery / CPU / memory menu
// bar glyphs. A gap at the bottom-right with an accent dot at the upper
// right keeps it from looking like a plain circle.

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

func lerp(_ a: CGFloat, _ b: CGFloat, _ t: CGFloat) -> CGFloat {
    a + (b - a) * t
}

func mix(_ c1: NSColor, _ c2: NSColor, _ t: CGFloat) -> NSColor {
    let a = c1.usingColorSpace(.deviceRGB)!
    let b = c2.usingColorSpace(.deviceRGB)!
    return NSColor(
        red: lerp(a.redComponent, b.redComponent, t),
        green: lerp(a.greenComponent, b.greenComponent, t),
        blue: lerp(a.blueComponent, b.blueComponent, t),
        alpha: 1,
    )
}

// Ring geometry: sweeping 340° counterclockwise from -20° to 320° leaves
// a 20° gap at the lower right, between 320° and 340°. The accent dot
// sits at 40°, mirroring the gap across the +x axis.
let arcStart: CGFloat = -20
let arcEnd: CGFloat = 320
let dotAngleDeg: CGFloat = 40

func drawIcon(size: CGFloat) -> NSImage {
    NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
        let small = size <= 32

        // Background: macOS-style rounded rect with a vertical gradient.
        let bgPath = NSBezierPath(roundedRect: rect, xRadius: size * 0.2237, yRadius: size * 0.2237)
        NSGraphicsContext.saveGraphicsState()
        bgPath.addClip()
        NSGradient(colors: [bgTop, bgBottom])?.draw(in: rect, angle: -90)
        NSGraphicsContext.restoreGraphicsState()

        // Soft radial glow behind the ring, like light bleeding out of
        // the aura. CGContext.drawRadialGradient (unlike NSGradient)
        // paints nothing past the end circle, so no faint square seam
        // shows up at the bounds of the drawing rect. Skipped at small
        // sizes, where it only muddies the center.
        if !small, let ctx = NSGraphicsContext.current?.cgContext {
            let glowCenter = CGPoint(x: rect.midX, y: rect.midY)
            let glowRadius = size * 0.32
            let glowColors = [
                teal.withAlphaComponent(0.35).cgColor,
                teal.withAlphaComponent(0).cgColor,
            ] as CFArray
            if let gradient = CGGradient(
                colorsSpace: CGColorSpaceCreateDeviceRGB(),
                colors: glowColors,
                locations: [0, 1],
            ) {
                ctx.saveGState()
                bgPath.addClip()
                ctx.drawRadialGradient(
                    gradient,
                    startCenter: glowCenter, startRadius: 0,
                    endCenter: glowCenter, endRadius: glowRadius,
                    options: [],
                )
                ctx.restoreGState()
            }
        }

        let center = NSPoint(x: rect.midX, y: rect.midY)
        let ringRadius = size * 0.335
        let ringWidth = size * (small ? 0.12 : 0.11)

        if small {
            // Single-color ring: at 16px the gradient is unreadable and
            // the per-segment strokes alias into noise.
            let path = NSBezierPath()
            path.appendArc(
                withCenter: center, radius: ringRadius,
                startAngle: arcStart, endAngle: arcEnd, clockwise: false,
            )
            path.lineWidth = ringWidth
            path.lineCapStyle = .round
            teal.setStroke()
            path.stroke()
        } else {
            // Real gradient ring: stroke the arc in short segments,
            // blending teal → aqua from the top (90°) down to the bottom
            // (270°) with a sinusoidal mapping (continuous all the way
            // around). Segments overlap by half a step under round caps
            // so no hairline cracks appear between them. An earlier
            // version faked this with a 35%-alpha overlay arc whose
            // round cap left a visible dome seam where it started.
            let segments = 180
            let step = (arcEnd - arcStart) / CGFloat(segments)
            for i in 0 ..< segments {
                let a0 = arcStart + step * CGFloat(i)
                let a1 = a0 + step * 1.5 // overlap into the next segment
                let mid = (a0 + a1) / 2
                let t = (1 - cos((mid - 90) * .pi / 180)) / 2
                let path = NSBezierPath()
                path.appendArc(
                    withCenter: center, radius: ringRadius,
                    startAngle: a0, endAngle: a1, clockwise: false,
                )
                path.lineWidth = ringWidth
                path.lineCapStyle = .round
                mix(teal, aqua, t).setStroke()
                path.stroke()
            }
        }

        // Accent dot at 40° (upper right).
        let dotAngle = dotAngleDeg * .pi / 180
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
