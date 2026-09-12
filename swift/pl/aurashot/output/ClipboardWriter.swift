import AppKit

/// Writes the finished capture to the general pasteboard (design
/// doc §6.6).
///
/// PNG and TIFF are provided LAZILY through an NSPasteboardItem data
/// provider: encoding a multi-megapixel TIFF eagerly on the main
/// thread stalls the UI for a visible beat, and most paste targets
/// only ever ask for one flavor. The provider encodes on demand, off
/// the critical path, when the paste target actually requests a type.
enum ClipboardWriter {
    static func write(_ image: CGImage) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        let item = NSPasteboardItem()
        item.setDataProvider(ImageDataProvider(image: image), forTypes: [.png, .tiff])
        pasteboard.writeObjects([item])
    }

    /// Plain-text flavor, for OCR results.
    static func writeText(_ text: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }
}

/// Encodes the capture into the requested pasteboard flavor on demand.
/// The pasteboard retains the item, the item retains the provider, so
/// the CGImage stays alive as long as the clipboard entry does.
private final class ImageDataProvider: NSObject, NSPasteboardItemDataProvider {
    private let image: CGImage

    init(image: CGImage) {
        self.image = image
    }

    func pasteboard(
        _: NSPasteboard?,
        item: NSPasteboardItem,
        provideDataForType type: NSPasteboard.PasteboardType,
    ) {
        let bitmap = NSBitmapImageRep(cgImage: image)
        let format: NSBitmapImageRep.FileType = type == .png ? .png : .tiff
        if let data = bitmap.representation(using: format, properties: [:]) {
            item.setData(data, forType: type)
        }
    }
}
