import AppKit

/// A shortcut recorder field: click to arm, press the new combination
/// (at least one of ⌃⌥⇧⌘ required), Esc cancels. Looks like a text
/// field but is a plain custom view — recorder fields don't want IME
/// or text editing semantics.
final class HotkeyRecorderView: NSView {
    var combo: KeyCombo {
        didSet { needsDisplay = true }
    }

    var onChange: ((KeyCombo) -> Void)?

    private var recording = false {
        didSet { needsDisplay = true }
    }

    init(combo: KeyCombo) {
        self.combo = combo
        super.init(frame: NSRect(x: 0, y: 0, width: 160, height: 24))
        translatesAutoresizingMaskIntoConstraints = false
        widthAnchor.constraint(equalToConstant: 160).isActive = true
        heightAnchor.constraint(equalToConstant: 24).isActive = true
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var acceptsFirstResponder: Bool {
        true
    }

    override func mouseDown(with _: NSEvent) {
        recording = true
        window?.makeFirstResponder(self)
        // Unregister the live Carbon hotkeys while recording: pressing
        // the current combo into the field must be CAPTURED, not fire
        // a screenshot session out from under the settings window.
        HotKeyManager.shared.suspend()
    }

    override func keyDown(with event: NSEvent) {
        guard recording else { super.keyDown(with: event); return }
        if event.keyCode == Carbon.KeyCode.escape {
            cancelRecording()
            return
        }
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let mask = KeyCombo.carbonModifiers(from: flags)
        guard mask != 0, let characters = event.charactersIgnoringModifiers, !characters.isEmpty else {
            // Modifier-only presses or dead keys: keep waiting.
            NSSound.beep()
            return
        }
        var newCombo = KeyCombo(
            keyCode: UInt32(event.keyCode),
            carbonModifiers: mask,
            display: KeyCombo.displayString(carbonModifiers: mask, keyCharacter: characters),
        )
        newCombo.display = newCombo.display.trimmingCharacters(in: .whitespaces)
        combo = newCombo
        onChange?(newCombo)
        cancelRecording()
    }

    override func flagsChanged(with _: NSEvent) {
        // Swallow modifier presses so they don't beep while recording.
    }

    private func cancelRecording() {
        recording = false
        window?.makeFirstResponder(nil)
        HotKeyManager.shared.resume()
    }

    override func resignFirstResponder() -> Bool {
        if recording {
            recording = false
            HotKeyManager.shared.resume()
        }
        return super.resignFirstResponder()
    }

    override func draw(_: NSRect) {
        let path = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: 5, yRadius: 5)
        (recording ? NSColor.controlAccentColor.withAlphaComponent(0.12) : NSColor.textBackgroundColor)
            .setFill()
        path.fill()
        (recording ? NSColor.controlAccentColor : NSColor.separatorColor).setStroke()
        path.lineWidth = recording ? 1.5 : 0.5
        path.stroke()

        let text = recording ? "按下新的快捷键…" : combo.display
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 13),
            .foregroundColor: recording
                ? NSColor.secondaryLabelColor
                : NSColor.labelColor,
        ]
        let size = text.size(withAttributes: attributes)
        text.draw(
            at: CGPoint(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2),
            withAttributes: attributes,
        )
    }
}
