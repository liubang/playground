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

# generate_icon.sh — regenerate macos/AppIcon.icns.
#
# Renders the Loom mark (the diamond glyph from
# internal/server/web/static/favicon.svg) at every iconset size via a
# throwaway Swift/CoreGraphics program, then packs the iconset with
# iconutil(1). Re-run this after changing the artwork and commit the
# resulting AppIcon.icns.
set -euo pipefail
cd "$(dirname "$0")"

SWIFT_SRC="$(mktemp /tmp/loom_icon_XXXXXX.swift)"
cleanup() {
  rm -f "${SWIFT_SRC}"
  rm -rf AppIcon.iconset
}
trap cleanup EXIT

cat > "${SWIFT_SRC}" <<'EOF'
import AppKit

// macOS Big Sur+ style icon: rounded-rectangle (squircle) background with
// the Loom diamond mark as a ring, sized to ~66% of the canvas per the
// Apple icon grid.
func drawMark(size: Int) -> NSImage {
    let s = CGFloat(size)
    let image = NSImage(size: NSMakeSize(s, s))
    image.lockFocus()

    // Squircle background with a subtle vertical gradient.
    let rect = NSRect(x: 0, y: 0, width: s, height: s)
    let radius = s * 0.2237
    let bg = NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius)
    let gradient = NSGradient(colors: [
        NSColor(calibratedRed: 0x2b / 255.0, green: 0x35 / 255.0, blue: 0x39 / 255.0, alpha: 1),
        NSColor(calibratedRed: 0x1e / 255.0, green: 0x23 / 255.0, blue: 0x26 / 255.0, alpha: 1),
    ])!
    gradient.draw(in: bg, angle: -90)

    // Diamond mark (same geometry as favicon.svg) centered on the canvas.
    let markExtent = s * 0.66
    let k = markExtent / 32.0
    let ox = (s - markExtent) / 2.0
    let oy = (s - markExtent) / 2.0
    // SVG is y-down; AppKit is y-up.
    func pt(_ x: CGFloat, _ y: CGFloat) -> NSPoint { NSMakePoint(ox + x * k, oy + (32 - y) * k) }

    let outer = NSBezierPath()
    outer.move(to: pt(16, 2))
    outer.line(to: pt(30, 16))
    outer.line(to: pt(16, 30))
    outer.line(to: pt(2, 16))
    outer.close()
    NSColor(calibratedRed: 0x7f / 255.0, green: 0xbb / 255.0, blue: 0xb3 / 255.0, alpha: 1).setFill()
    outer.fill()

    let inner = NSBezierPath()
    inner.move(to: pt(16, 8))
    inner.line(to: pt(24, 16))
    inner.line(to: pt(16, 24))
    inner.line(to: pt(8, 16))
    inner.close()
    NSColor(calibratedRed: 0x1e / 255.0, green: 0x23 / 255.0, blue: 0x26 / 255.0, alpha: 1).setFill()
    inner.fill()

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
    let img = drawMark(size: px)
    guard let tiff = img.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:])
    else {
        fatalError("failed to render \(name)")
    }
    try png.write(to: URL(fileURLWithPath: "AppIcon.iconset/\(name)"))
}
EOF

rm -rf AppIcon.iconset
mkdir -p AppIcon.iconset
swift "${SWIFT_SRC}"
iconutil -c icns AppIcon.iconset -o AppIcon.icns
echo "wrote $(pwd)/AppIcon.icns"
