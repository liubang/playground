import AppKit

/// Tiny transient HUD for OCR progress/result: a dark rounded panel
/// centered on the main screen, owned by no session, dismissed by the
/// caller (or a short auto-dismiss timer).
@MainActor
enum OcrHud {
    private static var panel: NSPanel?

    static func show(_ text: String, spinner: Bool) {
        dismiss()
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 240, height: 64),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false,
        )
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.isReleasedWhenClosed = false

        let background = NSView()
        background.wantsLayer = true
        background.layer?.cornerRadius = 14
        background.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.75).cgColor

        let label = NSTextField(labelWithString: text)
        label.textColor = .white
        label.font = .systemFont(ofSize: 13, weight: .medium)
        label.alignment = .center
        label.lineBreakMode = .byTruncatingTail

        let stack = NSStackView()
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        if spinner {
            let indicator = NSProgressIndicator()
            indicator.style = .spinning
            indicator.controlSize = .small
            indicator.startAnimation(nil)
            stack.addArrangedSubview(indicator)
        }
        stack.addArrangedSubview(label)

        background.addSubview(stack)
        stack.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: background.centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: background.centerYAnchor),
            stack.widthAnchor.constraint(lessThanOrEqualToConstant: 220),
        ])
        panel.contentView = background

        if let screen = NSScreen.main {
            panel.center()
            panel.setFrameOrigin(CGPoint(
                x: screen.frame.midX - panel.frame.width / 2,
                y: screen.frame.midY - panel.frame.height / 2 + screen.frame.height * 0.1,
            ))
        }
        panel.orderFrontRegardless()
        self.panel = panel
    }

    static func dismiss(after delay: TimeInterval = 0) {
        if delay > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { dismiss() }
            return
        }
        panel?.orderOut(nil)
        panel = nil
    }
}
