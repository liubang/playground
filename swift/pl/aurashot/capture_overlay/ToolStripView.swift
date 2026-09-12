import AppKit

/// The floating toolbar shown below the selection once it is placed:
/// white capsule, gray glyphs, thin separators, a red ✗ for cancel
/// and a copy icon for done.
final class ToolStripView: NSView {
    enum Action {
        case tool(AnnotationTool)
        case undo
        case clearAll
        case ocr
        case cancel
        case save
        case done // copy + close
    }

    var onAction: ((Action) -> Void)?
    private(set) var activeTool: AnnotationTool?

    /// Same frame blue as the selection chrome.
    private static let selectionBlue = NSColor(red: 0.04, green: 0.52, blue: 1.0, alpha: 1)

    private enum Item {
        case button(symbol: String, tip: String, action: Action)
        case separator
    }

    private let items: [Item] = [
        .button(symbol: "square", tip: "矩形", action: .tool(.rectangle)),
        .button(symbol: "circle", tip: "椭圆", action: .tool(.ellipse)),
        .button(symbol: "line.diagonal", tip: "直线", action: .tool(.line)),
        .button(symbol: "arrow.up.left", tip: "箭头", action: .tool(.arrow)),
        .button(symbol: "pencil", tip: "手绘", action: .tool(.freehand)),
        .button(symbol: "square.grid.3x3", tip: "马赛克", action: .tool(.mosaic)),
        // NB: NOT SF Symbols' "character" — that glyph is locale
        // aware and renders as 字 on Chinese systems. "A" is special
        // cased in buildButtons() to a hand-drawn Latin letter.
        .button(symbol: "A", tip: "文字", action: .tool(.text)),
        .button(symbol: "1.circle.fill", tip: "序号", action: .tool(.step)),
        .separator,
        .button(symbol: "arrow.uturn.backward", tip: "撤销 (⌘Z)", action: .undo),
        .button(symbol: "trash", tip: "清空标注", action: .clearAll),
        .separator,
        .button(symbol: "xmark", tip: "取消 (Esc)", action: .cancel),
        .button(symbol: "text.viewfinder", tip: "识别文字 (OCR)", action: .ocr),
        .button(symbol: "square.and.arrow.down", tip: "保存 (⌘S)", action: .save),
        .button(symbol: "doc.on.doc", tip: "复制到剪贴板 (⏎)", action: .done),
    ]

    private let buttonSize = CGSize(width: 34, height: 34)
    private let spacing: CGFloat = 6
    private let padding: CGFloat = 16
    private let stripHeight: CGFloat = 46
    private var buttons: [Int: NSButton] = [:]
    private var hoveredButton: NSButton?

    override var intrinsicContentSize: CGSize {
        var width = padding * 2
        for item in items {
            switch item {
            case .button: width += buttonSize.width + spacing
            case .separator: width += 10 + spacing
            }
        }
        return CGSize(width: width - spacing, height: stripHeight)
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        // Stadium pill: fully rounded ends, no border, soft shadow.
        layer?.cornerRadius = stripHeight / 2
        layer?.backgroundColor = NSColor(white: 0.99, alpha: 0.98).cgColor
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.28)
        shadow.shadowOffset = NSSize(width: 0, height: -2)
        shadow.shadowBlurRadius = 12
        self.shadow = shadow
        buildButtons()
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    private func buildButtons() {
        var x = padding
        let y = (stripHeight - buttonSize.height) / 2
        for (index, item) in items.enumerated() {
            switch item {
            case let .button(symbol, tip, _):
                let button = NSButton(
                    frame: CGRect(origin: CGPoint(x: x, y: y), size: buttonSize),
                )
                button.isBordered = false
                button.wantsLayer = true
                button.layer?.cornerRadius = 8
                if symbol == "A" {
                    button.image = Self.letterAImage()
                } else {
                    button.image = NSImage(
                        systemSymbolName: symbol,
                        accessibilityDescription: tip,
                    )?.withSymbolConfiguration(.init(pointSize: 19, weight: .medium))
                }
                button.contentTintColor = NSColor(white: 0.22, alpha: 1)
                button.toolTip = tip
                button.target = self
                button.action = #selector(buttonClicked(_:))
                button.tag = index
                addSubview(button)
                buttons[index] = button
                x += buttonSize.width + spacing
            case .separator:
                // A short, rounded tick — not a hairline: much better
                // visual weight next to the medium-weight glyphs.
                let line = NSView(frame: CGRect(
                    x: x + (10 - 2.5) / 2, y: (stripHeight - 16) / 2,
                    width: 2.5, height: 16,
                ))
                line.wantsLayer = true
                line.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.18).cgColor
                line.layer?.cornerRadius = 1.25
                addSubview(line)
                x += 10 + spacing
            }
        }
        updateButtonStyles()
    }

    /// A hand-drawn Latin "A" in the same optical size/thinness as
    /// the SF Symbol glyphs around it. Rendered as a template image
    /// so the button's contentTintColor drives idle/hover/active
    /// coloring exactly like the symbol-based icons.
    private static func letterAImage() -> NSImage {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 19, weight: .regular),
            .foregroundColor: NSColor.black,
        ]
        let text = "A"
        let size = text.size(withAttributes: attrs)
        let image = NSImage(size: size, flipped: false) { rect in
            text.draw(at: rect.origin, withAttributes: attrs)
            return true
        }
        image.isTemplate = true
        return image
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for (_, button) in buttons {
            button.trackingAreas.forEach { button.removeTrackingArea($0) }
            button.addTrackingArea(NSTrackingArea(
                rect: button.bounds,
                options: [.mouseEnteredAndExited, .activeInKeyWindow],
                owner: self,
                userInfo: ["button": button],
            ))
        }
    }

    override func mouseEntered(with event: NSEvent) {
        hoveredButton = event.trackingArea?.userInfo?["button"] as? NSButton
        updateButtonStyles()
    }

    override func mouseExited(with _: NSEvent) {
        hoveredButton = nil
        updateButtonStyles()
    }

    @objc private func buttonClicked(_ sender: NSButton) {
        guard sender.tag < items.count, case let .button(_, _, action) = items[sender.tag] else { return }
        if case let .tool(tool) = action {
            setActiveTool(activeTool == tool ? nil : tool)
        }
        onAction?(action)
    }

    func setActiveTool(_ tool: AnnotationTool?) {
        activeTool = tool
        updateButtonStyles()
    }

    private func updateButtonStyles() {
        for (index, button) in buttons {
            guard case let .button(_, _, action) = items[index] else { continue }

            // Accent color: red cancel; everything else stays neutral.
            var tint = NSColor(white: 0.22, alpha: 1)
            if case .cancel = action { tint = NSColor(red: 0.85, green: 0.16, blue: 0.18, alpha: 1) }

            var active = false
            if case let .tool(t) = action, t == activeTool { active = true }

            if active {
                button.layer?.backgroundColor = Self.selectionBlue.withAlphaComponent(0.14).cgColor
                button.contentTintColor = Self.selectionBlue
            } else if button == hoveredButton {
                button.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.06).cgColor
                button.contentTintColor = tint
            } else {
                button.layer?.backgroundColor = nil
                button.contentTintColor = tint
            }
        }
    }
}
