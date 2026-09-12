import AppKit

/// Writes the finished capture to the general pasteboard (design
/// doc §6.6): PNG and TIFF representations together, so paste targets
/// that only accept one flavor (Finder, Preview, IMs) all work.
enum ClipboardWriter {
    static func write(_ image: CGImage) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()

        let bitmap = NSBitmapImageRep(cgImage: image)
        let item = NSPasteboardItem()
        if let png = bitmap.representation(using: .png, properties: [:]) {
            item.setData(png, forType: .png)
        }
        if let tiff = bitmap.representation(using: .tiff, properties: [:]) {
            item.setData(tiff, forType: .tiff)
        }
        pasteboard.writeObjects([item])
    }
}
