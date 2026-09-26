#!/usr/bin/env bash
# Copyright (c) 2026 The Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# generate_icon.sh — regenerate resources/AppIcon.icns for Loom (Swift).
#
# The mark: Loom's classic diamond ring (the glyph from
# go/pl/loom/internal/server/web/static/favicon.svg), refined — an
# aqua-gradient diamond ring lifted off the dark Everforest squircle.
# Detailing: a radial top sheen on the background, a diagonal (-70°)
# aqua gradient on the ring, a real drop shadow, and a hairline rim
# highlight — all three telling the same top-left light story. The ring
# occupies 68% of the canvas (optical compensation: point-contact
# diamonds read smaller than circles at the same extent). The inner
# cutout is an even-odd hole, so the background shows through exactly.
# Small sizes (<128px) skip the shadow/rim and brighten the ring so the
# mark stays crisp in the Dock. Rendered at every iconset size via a
# throwaway Swift/CoreGraphics program, then packed with iconutil(1).
# Re-run after changing the artwork and commit the resulting AppIcon.icns.
set -euo pipefail
cd "$(dirname "$0")"

SWIFT_SRC="$(mktemp /tmp/loom_swift_icon_XXXXXX.swift)"
cleanup() {
    rm -f "${SWIFT_SRC}"
    rm -rf AppIcon.iconset
}
trap cleanup EXIT

cat >"${SWIFT_SRC}" <<'EOF'
import AppKit

// Palette (Everforest).
let bgTop = NSColor(calibratedRed: 0x47 / 255, green: 0x55 / 255, blue: 0x60 / 255, alpha: 1)
let bgBottom = NSColor(calibratedRed: 0x1c / 255, green: 0x21 / 255, blue: 0x24 / 255, alpha: 1)
let aquaTop = NSColor(calibratedRed: 0x9d / 255, green: 0xd8 / 255, blue: 0xcd / 255, alpha: 1)
let aquaBottom = NSColor(calibratedRed: 0x6b / 255, green: 0xa8 / 255, blue: 0xa2 / 255, alpha: 1)

// smallTuning: below 128px the drop shadow and rim highlight only blur
// into dirty edges — skip them and let the brighter ring carry the mark.
func drawIcon(size: Int, smallTuning: Bool) -> NSImage {
    let s = CGFloat(size)
    let image = NSImage(size: NSMakeSize(s, s))
    image.lockFocus()

    // Squircle background: linear base gradient + a faint radial sheen
    // near the top, so the plate reads as lit volume, not a flat fill.
    let canvas = NSRect(x: 0, y: 0, width: s, height: s)
    let radius = s * 0.2237
    let bg = NSBezierPath(roundedRect: canvas, xRadius: radius, yRadius: radius)
    NSGradient(colors: [bgTop, bgBottom])!.draw(in: bg, angle: -90)
    NSGradient(colors: [
        NSColor.white.withAlphaComponent(0.07),
        NSColor.white.withAlphaComponent(0.0),
    ])!.draw(in: bg, relativeCenterPosition: NSPoint(x: 0, y: 0.42))

    // Diamond ring (favicon geometry) on a 32×32 design grid (y-down),
    // occupying 68% of the canvas — optical compensation, since a
    // point-contact diamond reads smaller than the Apple grid assumes.
    let extent = s * 0.68
    let k = extent / 32.0
    let ox = (s - extent) / 2.0
    let oy = (s - extent) / 2.0
    func pt(_ x: CGFloat, _ y: CGFloat) -> NSPoint { NSMakePoint(ox + x * k, oy + (32 - y) * k) }

    // The ring as ONE even-odd path: the inner diamond is a true hole,
    // never painted, so the background gradient shows through exactly
    // (no refilled patch, no seam) and the shadow hugs the ring only.
    let ring = NSBezierPath()
    ring.move(to: pt(16, 2)); ring.line(to: pt(30, 16)); ring.line(to: pt(16, 30)); ring.line(to: pt(2, 16))
    ring.close()
    ring.move(to: pt(16, 8)); ring.line(to: pt(24, 16)); ring.line(to: pt(16, 24)); ring.line(to: pt(8, 16))
    ring.close()
    ring.windingRule = .evenOdd

    // Diagonal (-70°) gradient + a drop shadow with real presence —
    // sheen, gradient and shadow all tell the same top-left light story.
    NSGraphicsContext.saveGraphicsState()
    if !smallTuning {
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.45)
        shadow.shadowBlurRadius = s * 0.02
        shadow.shadowOffset = NSSize(width: 0, height: -s * 0.008)
        shadow.set()
    }
    NSGradient(colors: [aquaTop, aquaBottom])!.draw(in: ring, angle: -70)
    NSGraphicsContext.restoreGraphicsState()

    // Hairline rim highlight along the ring edges, faking a bevel that
    // catches the light. Skipped at small sizes where it would smear.
    if !smallTuning {
        let rim = NSBezierPath()
        rim.move(to: pt(16, 2)); rim.line(to: pt(30, 16)); rim.line(to: pt(16, 30)); rim.line(to: pt(2, 16))
        rim.close()
        rim.move(to: pt(16, 8)); rim.line(to: pt(24, 16)); rim.line(to: pt(16, 24)); rim.line(to: pt(8, 16))
        rim.close()
        rim.lineWidth = s * 0.0035
        NSColor.white.withAlphaComponent(0.22).setStroke()
        rim.stroke()
    }

    image.unlockFocus()
    return image
}

let specs: [(String, Int)] = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]
for (name, px) in specs {
    let img = drawIcon(size: px, smallTuning: px < 128)
    guard let tiff = img.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:])
    else {
        fatalError("failed to render \(name)")
    }
    try png.write(to: URL(fileURLWithPath: "AppIcon.iconset/\(name)"))
}
// Also drop a 1024px preview next to the icns for review.
try drawIcon(size: 1024, smallTuning: false).tiffRepresentation
    .flatMap { NSBitmapImageRep(data: $0) }?
    .representation(using: .png, properties: [:])?
    .write(to: URL(fileURLWithPath: "AppIcon.preview.png"))
EOF

rm -rf AppIcon.iconset
mkdir -p AppIcon.iconset
swift "${SWIFT_SRC}"
iconutil -c icns AppIcon.iconset -o AppIcon.icns
echo "wrote $(pwd)/AppIcon.icns (+ AppIcon.preview.png)"
