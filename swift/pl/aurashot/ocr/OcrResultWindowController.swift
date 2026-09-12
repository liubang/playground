import AppKit

/// Shows OCR output in a small panel instead of silently landing on
/// the clipboard: the user can review, trim, and re-copy. Text is
/// editable; the panel is reused across runs.
@MainActor
final class OcrResultWindowController: NSObject {
    static let shared = OcrResultWindowController()

    private var panel: NSPanel?
    private var textView: NSTextView?
    private var countLabel: NSTextField?

    func show(text: String) {
        if panel == nil {
            buildPanel()
        }
        textView?.string = text
        countLabel?.stringValue = "\(text.count) 字"

        let mouse = NSEvent.mouseLocation
        if let screen = NSScreen.screens.first(where: { $0.frame.contains(mouse) }) ?? NSScreen.main,
           let panel
        {
            let size = panel.frame.size
            panel.setFrameOrigin(CGPoint(
                x: min(max(mouse.x - size.width / 2, screen.frame.minX + 20),
                       screen.frame.maxX - size.width - 20),
                y: min(max(mouse.y - size.height / 2, screen.frame.minY + 20),
                       screen.frame.maxY - size.height - 20),
            ))
        }
        panel?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func buildPanel() {
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 560, height: 420),
            styleMask: [.titled, .closable, .resizable, .utilityWindow],
            backing: .buffered,
            defer: false,
        )
        panel.title = "OCR 识别结果"
        panel.isReleasedWhenClosed = false
        panel.minSize = NSSize(width: 360, height: 240)

        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.borderType = .noBorder
        scroll.translatesAutoresizingMaskIntoConstraints = false

        let textView = NSTextView()
        textView.isEditable = true
        textView.isSelectable = true
        textView.font = .systemFont(ofSize: 13)
        textView.textContainerInset = NSSize(width: 8, height: 8)
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.textContainer?.widthTracksTextView = true
        textView.autoresizingMask = [.width]
        scroll.documentView = textView
        self.textView = textView

        let countLabel = NSTextField(labelWithString: "")
        countLabel.font = .systemFont(ofSize: 11)
        countLabel.textColor = .secondaryLabelColor
        self.countLabel = countLabel

        let copyButton = NSButton(title: "复制全部", target: self, action: #selector(copyAll))
        copyButton.bezelStyle = .rounded
        copyButton.keyEquivalent = "\r"
        let closeButton = NSButton(title: "关闭", target: self, action: #selector(closePanel))
        closeButton.bezelStyle = .rounded
        closeButton.keyEquivalent = "\u{1b}" // Esc

        let bar = NSStackView(views: [countLabel, NSView(), copyButton, closeButton])
        bar.orientation = .horizontal
        bar.alignment = .centerY
        bar.spacing = 8
        bar.translatesAutoresizingMaskIntoConstraints = false

        let content = NSView()
        content.addSubview(scroll)
        content.addSubview(bar)
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: content.topAnchor),
            scroll.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bar.topAnchor, constant: -8),

            bar.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 12),
            bar.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -12),
            bar.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -10),
            bar.heightAnchor.constraint(equalToConstant: 28),
        ])
        panel.contentView = content
        self.panel = panel
    }

    @objc private func copyAll() {
        guard let text = textView?.string, !text.isEmpty else { return }
        ClipboardWriter.writeText(text)
        OcrHud.toast("已复制识别结果（\(text.count) 字）")
    }

    @objc private func closePanel() {
        panel?.close()
    }
}
