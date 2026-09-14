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
    private var grid: NSGridView?
    private var pathLabel: NSTextField?
    private var autoSaveCheckbox: NSButton?
    private var borderShadowCheckbox: NSButton?
    private var patternField: NSTextField?
    private var patternPreview: NSTextField?
    private var ocrEngineLabel: NSTextField?
    private var ocrModelLabel: NSTextField?
    private var ocrServerStatusLabel: NSTextField?
    private var ocrServerRestartButton: NSButton?
    private var hotkeyWarningLabel: NSTextField?
    private var hotkeyWarningRow: NSGridRow?

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

        // Hotkey recorders. A combo equal to the OTHER action's combo
        // would fail Carbon registration (or shadow it), so it's
        // rejected with an inline warning instead of being saved.
        let captureRecorder = HotkeyRecorderView(combo: settings.captureCombo)
        captureRecorder.onChange = { [weak self, weak captureRecorder] combo in
            if Settings.hotkeysConflict(combo, settings.ocrCombo) {
                NSSound.beep()
                captureRecorder?.combo = settings.captureCombo
                self?.showHotkeyConflict("截图快捷键与 OCR 快捷键相同")
                return
            }
            self?.showHotkeyConflict(nil)
            settings.captureCombo = combo
        }
        let ocrRecorder = HotkeyRecorderView(combo: settings.ocrCombo)
        ocrRecorder.onChange = { [weak self, weak ocrRecorder] combo in
            if Settings.hotkeysConflict(combo, settings.captureCombo) {
                NSSound.beep()
                ocrRecorder?.combo = settings.ocrCombo
                self?.showHotkeyConflict("OCR 快捷键与截图快捷键相同")
                return
            }
            self?.showHotkeyConflict(nil)
            settings.ocrCombo = combo
        }

        // Inline hotkey conflict warning. The whole grid ROW is hidden
        // (not just the label) so it leaves no gap in the layout.
        let hotkeyWarning = NSTextField(labelWithString: "")
        hotkeyWarning.font = .systemFont(ofSize: 11)
        hotkeyWarning.textColor = .systemRed
        hotkeyWarningLabel = hotkeyWarning

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
        let ocrEngineLabel = NSTextField(labelWithString: "")
        ocrEngineLabel.lineBreakMode = .byTruncatingMiddle
        ocrEngineLabel.font = .systemFont(ofSize: 12)
        ocrEngineLabel.textColor = .secondaryLabelColor
        self.ocrEngineLabel = ocrEngineLabel
        let ocrEngineButton = NSButton(title: "选择…", target: self, action: #selector(chooseOcrEngine))
        ocrEngineButton.bezelStyle = .rounded
        let ocrEngineRow = NSStackView(views: [ocrEngineLabel, ocrEngineButton])
        ocrEngineRow.orientation = .horizontal
        ocrEngineRow.alignment = .firstBaseline
        ocrEngineRow.spacing = 8

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

        // OCR server row: runtime status + manual restart.
        let serverStatus = NSTextField(labelWithString: "")
        serverStatus.lineBreakMode = .byTruncatingMiddle
        serverStatus.font = .systemFont(ofSize: 12)
        serverStatus.textColor = .secondaryLabelColor
        serverStatus.setContentHuggingPriority(.defaultLow, for: .horizontal)
        ocrServerStatusLabel = serverStatus
        let restartButton = NSButton(title: "重启", target: self, action: #selector(restartOcrServer))
        restartButton.bezelStyle = .rounded
        ocrServerRestartButton = restartButton
        let serverRow = NSStackView(views: [serverStatus, restartButton])
        serverRow.orientation = .horizontal
        serverRow.alignment = .firstBaseline
        serverRow.spacing = 8

        // Form grid, grouped into three sections (截图 / 存储 / OCR).
        // Section headers sit in the content column; the label column
        // stays empty for those rows.
        let grid = NSGridView()
        grid.rowAlignment = .firstBaseline
        grid.rowSpacing = 12
        grid.columnSpacing = 10
        grid.translatesAutoresizingMaskIntoConstraints = false

        @discardableResult
        func addRow(_ views: [NSView]) -> Int {
            grid.addRow(with: views)
            return grid.numberOfRows - 1
        }

        /// Rows whose content should span the full content-column width
        /// (path/pattern/server rows); everything else is leading-aligned
        /// at its natural size (hotkey recorders, checkboxes, hints).
        func addFillRow(_ views: [NSView]) {
            let row = addRow(views)
            grid.cell(atColumnIndex: 1, rowIndex: row).xPlacement = .fill
        }

        func addSection(_ title: String, topPadding: CGFloat) {
            let row = addRow([NSGridCell.emptyContentView, sectionHeader(title)])
            grid.row(at: row).topPadding = topPadding
        }

        addSection("截图", topPadding: 0)
        addRow([label("截图快捷键"), captureRecorder])
        addRow([label("OCR 快捷键"), ocrRecorder])
        hotkeyWarningRow = grid.row(at: addRow([NSGridCell.emptyContentView, hotkeyWarning]))
        hotkeyWarningRow?.isHidden = true
        addRow([NSGridCell.emptyContentView, borderShadow])

        addSection("存储", topPadding: 10)
        addFillRow([label("保存位置"), pathRow])
        addRow([NSGridCell.emptyContentView, autoSave])
        addFillRow([label("文件名规则"), pattern])
        addRow([NSGridCell.emptyContentView, preview])

        addSection("OCR", topPadding: 10)
        addFillRow([label("OCR 引擎"), ocrEngineRow])
        addFillRow([label("OCR 模型目录"), ocrModelRow])
        addFillRow([label("OCR 服务"), serverRow])

        grid.column(at: 0).xPlacement = .trailing
        grid.column(at: 1).xPlacement = .leading
        self.grid = grid

        let content = NSView()
        content.addSubview(grid)
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            grid.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            grid.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            grid.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -20),
        ])
        window.contentView = content
        // Rows were added over time; size the window to the grid.
        let fitting = grid.fittingSize
        window.setContentSize(NSSize(width: max(470, fitting.width + 40),
                                     height: fitting.height + 40))
        self.window = window
    }

    private func label(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.alignment = .right
        return label
    }

    private func sectionHeader(_ text: String) -> NSTextField {
        let header = NSTextField(labelWithString: text)
        header.font = .boldSystemFont(ofSize: 13)
        return header
    }

    // MARK: - Values

    private func reloadValues() {
        let settings = Settings.shared
        pathLabel?.stringValue = settings.saveDirectory.path
        autoSaveCheckbox?.state = settings.autoSave ? .on : .off
        borderShadowCheckbox?.state = settings.borderShadow ? .on : .off
        patternField?.stringValue = settings.filenamePattern
        updatePatternPreview()
        updateOcrStatus()
        // Pick up crashed children / external servers asynchronously.
        Task {
            await MllmServerClient.shared.refresh()
            await MainActor.run { [weak self] in
                self?.updateOcrStatus()
            }
        }
    }

    /// Single source of truth for the OCR rows: engine/model paths plus
    /// one status line in the 服务 row, colored by health (green =
    /// running, orange = degraded/booting, red = failed).
    private func updateOcrStatus() {
        let settings = Settings.shared
        ocrEngineLabel?.stringValue = settings.ocrEnginePath.isEmpty
            ? "自动（应用内置 mllm_server）"
            : settings.ocrEnginePath
        ocrModelLabel?.stringValue = settings.ocrModelDir.path

        // Static resolvability check, shown when nothing is running yet.
        var standbyIssue: String?
        do {
            _ = try MllmOcrEngine.resolveModels()
            if MllmOcrEngine.resolveServer() == nil {
                _ = try MllmOcrEngine.resolveCLI()
                standbyIssue = "未找到 mllm_server，回退一次性 CLI（每次识别较慢）"
            }
        } catch {
            standbyIssue = error.localizedDescription
        }

        let status = MllmServerClient.shared.status
        let text: String
        let color: NSColor
        switch status.phase {
        case .running:
            text = "运行中（本应用启动，端口 8310）"
            color = .systemGreen
        case .booting:
            text = "启动中（模型加载可能需要几十秒）…"
            color = .systemOrange
        case .failed:
            text = "启动失败：\(status.detail)"
            color = .systemRed
        case .stopped:
            if status.healthyExternal {
                text = "运行中（外部进程，端口 8310；重启按钮仅控制本应用启动的进程）"
                color = .systemGreen
            } else if let standbyIssue {
                text = standbyIssue
                color = .systemOrange
            } else if status.detail.isEmpty {
                text = "就绪，未运行（首次 OCR 时自动启动）"
                color = .secondaryLabelColor
            } else {
                text = "未运行（\(status.detail)）"
                color = .systemOrange
            }
        }
        ocrServerStatusLabel?.stringValue = text
        ocrServerStatusLabel?.textColor = color
    }

    private func showHotkeyConflict(_ message: String?) {
        hotkeyWarningLabel?.stringValue = message ?? ""
        hotkeyWarningRow?.isHidden = (message == nil)
        resizeToFitContent()
    }

    /// The warning row appears/disappears after the window is built, so
    /// the initial content size would clip it — re-fit, keeping the top
    /// edge of the window in place.
    private func resizeToFitContent() {
        guard let window, let grid else { return }
        let fitting = grid.fittingSize
        let size = NSSize(width: max(470, fitting.width + 40), height: fitting.height + 40)
        var frame = window.frame
        frame.origin.y -= size.height - frame.height
        frame.size = size
        window.setFrame(frame, display: true, animate: true)
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

    @objc private func chooseOcrEngine() {
        guard let window else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.treatsFilePackagesAsDirectories = false
        panel.directoryURL = URL(fileURLWithPath: NSHomeDirectory())
        panel.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK, let url = panel.url else { return }
            Settings.shared.ocrEnginePath = url.path
            self?.updateOcrStatus()
        }
    }

    @objc private func restartOcrServer() {
        guard let binary = MllmOcrEngine.resolveServer() else {
            ocrServerStatusLabel?.stringValue = "未找到 mllm_server 二进制"
            ocrServerStatusLabel?.textColor = .systemOrange
            return
        }
        let model: String
        let mmproj: String
        do {
            (model, mmproj) = try MllmOcrEngine.resolveModels()
        } catch {
            ocrServerStatusLabel?.stringValue = error.localizedDescription
            ocrServerStatusLabel?.textColor = .systemOrange
            return
        }
        ocrServerRestartButton?.isEnabled = false
        ocrServerStatusLabel?.stringValue = "启动中（模型加载可能需要几十秒）…"
        ocrServerStatusLabel?.textColor = .systemOrange
        Task {
            _ = await MllmServerClient.shared.restart(binary: binary, model: model, mmproj: mmproj)
            await MainActor.run { [weak self] in
                self?.ocrServerRestartButton?.isEnabled = true
                self?.updateOcrStatus()
            }
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
            self?.updateOcrStatus()
        }
    }

    @objc private func patternChanged(_ sender: NSTextField) {
        let value = sender.stringValue.trimmingCharacters(in: .whitespaces)
        Settings.shared.filenamePattern = value.isEmpty ? Settings.defaultFilenamePattern : value
        updatePatternPreview()
    }
}
