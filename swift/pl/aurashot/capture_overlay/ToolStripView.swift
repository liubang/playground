import AppKit

/// The floating toolbar shown below the selection once it is placed —
/// Xnip parity styling: white capsule, gray glyphs, thin separators,
/// a red ✗ for cancel and a green ✓ for done.
final class ToolStripView: NSView {
    enum Action {
        case tool(AnnotationTool)
        case undo
        case clearAll
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
        .button(symbol: "rectangle", tip: "矩形", action: .tool(.rectangle)),
        .button(symbol: "oval", tip: "椭圆", action: .tool(.ellipse)),
        .button(symbol: "line.diagonal", tip: "直线", action: .tool(.line)),
        .button(symbol: "arrow.up.right", tip: "箭头", action: .tool(.arrow)),
        .button(symbol: "pencil", tip: "手绘", action: .tool(.freehand)),
        .button(symbol: "square.grid.3x3", tip: "马赛克", action: .tool(.mosaic)),
        .button(symbol: "character", tip: "文字", action: .tool(.text)),
        .button(symbol: "1.circle", tip: "序号", action: .tool(.step)),
        .separator,
        .button(symbol: "arrow.uturn.backward", tip: "撤销 (⌘Z)", action: .undo),
        .button(symbol: "trash", tip: "清空标注", action: .clearAll),
        .separator,
        .button(symbol: "square.and.arrow.down", tip: "保存 (⌘S)", action: .save),
        .button(symbol: "xmark", tip: "取消 (Esc)", action: .cancel),
        .button(symbol: "checkmark", tip: "完成，复制到剪贴板 (⏎)", action: .done),
    ]

    private let buttonSize = CGSize(width: 32, height: 30)
    private let spacing: CGFloat = 4
    private let padding: CGFloat = 12
    private let stripHeight: CGFloat = 40
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
                button.layer?.cornerRadius = 6
                button.image = NSImage(
                    systemSymbolName: symbol,
                    accessibilityDescription: tip,
                )?.withSymbolConfiguration(.init(pointSize: 15, weight: .regular))
                button.contentTintColor = NSColor(white: 0.22, alpha: 1)
                button.toolTip = tip
                button.target = self
                button.action = #selector(buttonClicked(_:))
                button.tag = index
                addSubview(button)
                buttons[index] = button
                x += buttonSize.width + spacing
            case .separator:
                let line = NSBox(frame: CGRect(
                    x: x + 4.5, y: 11, width: 1, height: stripHeight - 22,
                ))
                line.boxType = .separator
                line.fillColor = NSColor.black.withAlphaComponent(0.10)
                line.borderColor = .clear
                addSubview(line)
                x += 10 + spacing
            }
        }
        updateButtonStyles()
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

            // Xnip accents: red cancel, green done.
            var tint = NSColor(white: 0.22, alpha: 1)
            if case .cancel = action { tint = NSColor(red: 0.85, green: 0.16, blue: 0.18, alpha: 1) }
            if case .done = action { tint = NSColor(red: 0.25, green: 0.72, blue: 0.27, alpha: 1) }

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
