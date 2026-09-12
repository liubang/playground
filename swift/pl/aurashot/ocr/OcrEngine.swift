import CoreGraphics

/// One recognized block of content. `rect` is always in image pixel
/// coordinates (CGImage space), device-independent — the output layer
/// converts to points via the screen's backingScaleFactor.
struct OcrBlock: Sendable {
    enum Kind: Sendable {
        case text
        case title
        case table
        case formula
        case figure
    }

    var rect: CGRect
    var text: String
    var kind: Kind
    var confidence: Float
}

/// The seam between the UI and any OCR implementation (see
/// docs/design.md §7). The UI programs only against this protocol from
/// M0 on; the PaddleOCR-VL engine (cpp/pl/mllm via an ObjC++ bridge)
/// slots in at M4 without touching call sites.
protocol OcrEngine: Sendable {
    /// Recognizes the whole image, returning blocks in reading order.
    func recognize(_ image: CGImage) async throws -> [OcrBlock]
}

/// M0–M3 placeholder: always empty. Replaced by PaddleOcrEngine at M4.
struct NullOcrEngine: OcrEngine {
    func recognize(_: CGImage) async throws -> [OcrBlock] { [] }
}
