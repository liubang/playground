import AppKit

/// Tiny transient HUD for progress/result toasts: a dark rounded panel
/// centered on the screen under the mouse, owned by no session,
/// dismissed by the caller (or a short auto-dismiss timer).
@MainActor
enum OcrHud {
    private static var panel: NSPanel?
    /// Bumped by every show/dismiss. A scheduled auto-dismiss captures
    /// the generation at schedule time and no-ops if a newer HUD has
    /// since appeared — otherwise the timer from a PREVIOUS message
    /// would kill the one currently on screen.
    private static var generation = 0

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
        label.lineBreakMode = .byTruncatingMiddle

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

        // Show on the screen the user is actually looking at.
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { $0.frame.contains(mouse) } ?? NSScreen.main
        if let screen {
            panel.setFrameOrigin(CGPoint(
                x: screen.frame.midX - panel.frame.width / 2,
                y: screen.frame.midY - panel.frame.height / 2 + screen.frame.height * 0.1,
            ))
        } else {
            panel.center()
        }
        panel.alphaValue = 0
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.12
            panel.animator().alphaValue = 1
        }
        self.panel = panel
    }

    /// Brief confirmation message ("已复制到剪贴板" style).
    static func toast(_ text: String) {
        show(text, spinner: false)
        dismiss(after: 1.4)
    }

    static func dismiss(after delay: TimeInterval = 0) {
        if delay > 0 {
            let scheduled = generation
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                guard scheduled == generation else { return }
                dismiss()
            }
            return
        }
        generation += 1
        panel?.orderOut(nil)
        panel = nil
    }
}
