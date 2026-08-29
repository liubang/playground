import AppKit
import SwiftUI

/// Renders an SF Symbol pre-tinted with `color` as a non-template image.
/// AppKit-bridged controls in the MenuBarExtra window (e.g. the settings
/// Menu) ignore SwiftUI's foregroundStyle and recolor template images
/// with the window's appearance; a pre-tinted bitmap bypasses that.
enum TintedSymbol {
    static func make(_ name: String, color: Color, pointSize: CGFloat = 13) -> NSImage {
        let config = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .regular)
        guard let symbol = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(config)
        else {
            return NSImage()
        }
        let nsColor = NSColor(color)
        let image = NSImage(size: symbol.size, flipped: false) { rect in
            symbol.draw(in: rect)
            nsColor.setFill()
            rect.fill(using: .sourceAtop)
            return true
        }
        image.isTemplate = false
        return image
    }
}
