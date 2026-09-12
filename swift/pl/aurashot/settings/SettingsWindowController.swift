import AppKit

/// The preferences window: hotkey recorders, save folder, auto-save
/// toggle and the filename pattern (with a live example preview).
///
/// AppKit stack-view form, no XIB — the app has no storyboard and the
/// form is small enough to keep in code.
@MainActor
final class SettingsWindowController: NSObject {
    static let shared = SettingsWindowController()

    private var window: NSWindow?
    private var pathLabel: NSTextField?
    private var autoSaveCheckbox: NSButton?
    private var patternField: NSTextField?
    private var patternPreview: NSTextField?

    func show() {
        if window == nil {
            buildWindow()
        }
        reloadValues()
        window?.center()
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: - Window construction

    private func buildWindow() {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 470, height: 250),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false,
        )
        window.title = "AuraShot 设置"
        window.isReleasedWhenClosed = false

        let settings = Settings.shared

        // Hotkey recorders.
        let captureRecorder = HotkeyRecorderView(combo: settings.captureCombo)
        captureRecorder.onChange = { combo in settings.captureCombo = combo }
        let ocrRecorder = HotkeyRecorderView(combo: settings.ocrCombo)
        ocrRecorder.onChange = { combo in settings.ocrCombo = combo }

        // Save folder row.
        let pathLabel = NSTextField(labelWithString: "")
        pathLabel.lineBreakMode = .byTruncatingMiddle
        pathLabel.font = .systemFont(ofSize: 12)
        pathLabel.textColor = .secondaryLabelColor
        pathLabel.setContentHuggingPriority(.defaultLow, for: .horizontal)
        self.pathLabel = pathLabel

        let chooseButton = NSButton(title: "选择…", target: self, action: #selector(chooseFolder))
        chooseButton.bezelStyle = .rounded

        let pathRow = NSStackView(views: [pathLabel, chooseButton])
        pathRow.orientation = .horizontal
        pathRow.alignment = .firstBaseline
        pathRow.spacing = 8

        // Auto-save toggle.
        let autoSave = NSButton(
            checkboxWithTitle: "自动保存到该目录（不再每次询问）",
            target: self,
            action: #selector(toggleAutoSave(_:)),
        )
        autoSaveCheckbox = autoSave

        // Filename pattern row + live preview.
        let pattern = NSTextField(string: "")
        pattern.placeholderString = Settings.defaultFilenamePattern
        pattern.target = self
        pattern.action = #selector(patternChanged(_:))
        pattern.setContentHuggingPriority(.defaultLow, for: .horizontal)
        patternField = pattern

        let preview = NSTextField(labelWithString: "")
        preview.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        preview.textColor = .tertiaryLabelColor
        patternPreview = preview

        // Form grid.
        let grid = NSGridView(views: [
            [label("截图快捷键"), captureRecorder],
            [label("OCR 快捷键"), ocrRecorder],
            [label("保存位置"), pathRow],
            [NSGridCell.emptyContentView, autoSave],
            [label("文件名规则"), pattern],
            [NSGridCell.emptyContentView, preview],
        ])
        grid.column(at: 0).xPlacement = .trailing
        grid.rowAlignment = .firstBaseline
        grid.rowSpacing = 12
        grid.columnSpacing = 10
        grid.translatesAutoresizingMaskIntoConstraints = false

        let content = NSView()
        content.addSubview(grid)
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            grid.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            grid.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
        ])
        window.contentView = content
        self.window = window
    }

    private func label(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.alignment = .right
        return label
    }

    // MARK: - Values

    private func reloadValues() {
        let settings = Settings.shared
        pathLabel?.stringValue = settings.saveDirectory.path
        autoSaveCheckbox?.state = settings.autoSave ? .on : .off
        patternField?.stringValue = settings.filenamePattern
        updatePatternPreview()
    }

    private func updatePatternPreview() {
        let pattern = patternField?.stringValue ?? Settings.shared.filenamePattern
        let example = Settings.renderFileName(
            pattern: pattern.isEmpty ? Settings.defaultFilenamePattern : pattern,
        )
        patternPreview?.stringValue = "示例：" + example
    }

    // MARK: - Actions

    @objc private func chooseFolder() {
        guard let window else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        panel.directoryURL = Settings.shared.saveDirectory
        panel.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            Settings.shared.saveDirectory = url
            self?.pathLabel?.stringValue = url.path
        }
    }

    @objc private func toggleAutoSave(_ sender: NSButton) {
        Settings.shared.autoSave = (sender.state == .on)
    }

    @objc private func patternChanged(_ sender: NSTextField) {
        let value = sender.stringValue.trimmingCharacters(in: .whitespaces)
        Settings.shared.filenamePattern = value.isEmpty ? Settings.defaultFilenamePattern : value
        updatePatternPreview()
    }
}
