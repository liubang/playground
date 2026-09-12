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
    private var borderShadowCheckbox: NSButton?
    private var patternField: NSTextField?
    private var patternPreview: NSTextField?
    private var ocrCliLabel: NSTextField?
    private var ocrModelLabel: NSTextField?
    private var ocrStatusLabel: NSTextField?

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

        // Output framing (border + drop shadow) toggle.
        let borderShadow = NSButton(
            checkboxWithTitle: "截图带边框和阴影",
            target: self,
            action: #selector(toggleBorderShadow(_:)),
        )
        borderShadowCheckbox = borderShadow

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

        // OCR section: engine binary + model directory + live status.
        let ocrCliLabel = NSTextField(labelWithString: "")
        ocrCliLabel.lineBreakMode = .byTruncatingMiddle
        ocrCliLabel.font = .systemFont(ofSize: 12)
        ocrCliLabel.textColor = .secondaryLabelColor
        self.ocrCliLabel = ocrCliLabel
        let ocrCliButton = NSButton(title: "选择…", target: self, action: #selector(chooseOcrCli))
        ocrCliButton.bezelStyle = .rounded
        let ocrCliRow = NSStackView(views: [ocrCliLabel, ocrCliButton])
        ocrCliRow.orientation = .horizontal
        ocrCliRow.alignment = .firstBaseline
        ocrCliRow.spacing = 8

        let ocrModelLabel = NSTextField(labelWithString: "")
        ocrModelLabel.lineBreakMode = .byTruncatingMiddle
        ocrModelLabel.font = .systemFont(ofSize: 12)
        ocrModelLabel.textColor = .secondaryLabelColor
        self.ocrModelLabel = ocrModelLabel
        let ocrModelButton = NSButton(title: "选择…", target: self, action: #selector(chooseOcrModelDir))
        ocrModelButton.bezelStyle = .rounded
        let ocrModelRow = NSStackView(views: [ocrModelLabel, ocrModelButton])
        ocrModelRow.orientation = .horizontal
        ocrModelRow.alignment = .firstBaseline
        ocrModelRow.spacing = 8

        let status = NSTextField(labelWithString: "")
        status.font = .systemFont(ofSize: 11)
        ocrStatusLabel = status

        // Form grid.
        let grid = NSGridView(views: [
            [label("截图快捷键"), captureRecorder],
            [label("OCR 快捷键"), ocrRecorder],
            [label("保存位置"), pathRow],
            [NSGridCell.emptyContentView, autoSave],
            [NSGridCell.emptyContentView, borderShadow],
            [label("文件名规则"), pattern],
            [NSGridCell.emptyContentView, preview],
            [label("OCR 引擎"), ocrCliRow],
            [label("OCR 模型目录"), ocrModelRow],
            [NSGridCell.emptyContentView, status],
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
        // Rows were added over time; size the window to the grid.
        let fitting = grid.fittingSize
        window.setContentSize(NSSize(width: max(470, fitting.width + 40),
                                     height: fitting.height + 44))
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
        borderShadowCheckbox?.state = settings.borderShadow ? .on : .off
        patternField?.stringValue = settings.filenamePattern
        updatePatternPreview()
        reloadOcrStatus()
    }

    private func reloadOcrStatus() {
        let settings = Settings.shared
        ocrCliLabel?.stringValue = settings.ocrCliPath.isEmpty
            ? "自动（~/Library/Application Support/AuraShot/mllm_cli）"
            : settings.ocrCliPath
        ocrModelLabel?.stringValue = settings.ocrModelDir.path
        do {
            _ = try MllmOcrEngine.resolveCLI()
            _ = try MllmOcrEngine.resolveModels()
            ocrStatusLabel?.stringValue = "✅ OCR 就绪"
            ocrStatusLabel?.textColor = .systemGreen
        } catch {
            ocrStatusLabel?.stringValue = "⚠️ " + error.localizedDescription
            ocrStatusLabel?.textColor = .systemOrange
        }
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

    @objc private func toggleBorderShadow(_ sender: NSButton) {
        Settings.shared.borderShadow = (sender.state == .on)
    }

    @objc private func chooseOcrCli() {
        guard let window else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.treatsFilePackagesAsDirectories = false
        panel.directoryURL = URL(fileURLWithPath: NSHomeDirectory())
        panel.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            Settings.shared.ocrCliPath = url.path
            self?.reloadOcrStatus()
        }
    }

    @objc private func chooseOcrModelDir() {
        guard let window else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = Settings.shared.ocrModelDir
        panel.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            Settings.shared.ocrModelDir = url
            self?.reloadOcrStatus()
        }
    }

    @objc private func patternChanged(_ sender: NSTextField) {
        let value = sender.stringValue.trimmingCharacters(in: .whitespaces)
        Settings.shared.filenamePattern = value.isEmpty ? Settings.defaultFilenamePattern : value
        updatePatternPreview()
    }
}
