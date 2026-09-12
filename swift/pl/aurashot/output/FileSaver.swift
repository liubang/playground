import AppKit

/// Save flow for the finished capture (design doc §6.6), driven by
/// Settings:
///   autoSave off → NSSavePanel, defaulting to the configured folder
///                  and a pattern-generated name.
///   autoSave on  → write straight to the folder, no panel; name
///                  collisions get a " 2", " 3"… suffix.
enum FileSaver {
    /// Encodes the PNG off the main thread (a full-screen Retina PNG
    /// takes a visible beat to encode), then routes to auto-save or
    /// the save panel back on the main actor.
    @MainActor
    static func save(_ image: CGImage) {
        DispatchQueue.global(qos: .userInitiated).async {
            guard let png = NSBitmapImageRep(cgImage: image)
                .representation(using: .png, properties: [:])
            else {
                NSLog("AuraShot: PNG encoding failed")
                Task { @MainActor in OcrHud.toast("PNG 编码失败") }
                return
            }
            Task { @MainActor in
                if Settings.shared.autoSave {
                    writeToDefaultFolder(png)
                } else {
                    presentSavePanel(png)
                }
            }
        }
    }

    // MARK: - Auto save

    @MainActor
    private static func writeToDefaultFolder(_ png: Data) {
        let url = uniqueFileURL()
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(),
                withIntermediateDirectories: true,
            )
            try png.write(to: url)
            NSLog("AuraShot: saved \(url.path)")
            OcrHud.toast("已保存：\(url.lastPathComponent)")
        } catch {
            NSLog("AuraShot: save failed: \(error.localizedDescription)")
            OcrHud.toast("保存失败：\(error.localizedDescription)")
        }
    }

    /// The configured folder + pattern file name, made unique.
    @MainActor
    private static func uniqueFileURL() -> URL {
        let folder = Settings.shared.saveDirectory
        let baseName = Settings.renderFileName(pattern: Settings.shared.filenamePattern)
        let url = folder.appendingPathComponent(baseName)
        guard FileManager.default.fileExists(atPath: url.path) else { return url }

        let stem = url.deletingPathExtension().lastPathComponent
        let ext = url.pathExtension
        for index in 2 ... 99 {
            let candidate = folder.appendingPathComponent("\(stem) \(index).\(ext)")
            if !FileManager.default.fileExists(atPath: candidate.path) {
                return candidate
            }
        }
        return url // give up: overwrite after 98 collisions in one second
    }

    // MARK: - Interactive save

    @MainActor
    private static func presentSavePanel(_ png: Data) {
        let panel = NSSavePanel()
        // The capture overlays live at screenSaver level and cover every
        // display — at the default level the panel would appear UNDER
        // them and look like a no-op. Float one level higher.
        panel.level = NSWindow.Level(
            rawValue: Int(CGWindowLevelForKey(.screenSaverWindow)) + 1,
        )
        panel.allowedContentTypes = [.png]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = Settings.renderFileName(
            pattern: Settings.shared.filenamePattern,
        )
        panel.directoryURL = Settings.shared.saveDirectory
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let url = panel.url else { return }

        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(),
                withIntermediateDirectories: true,
            )
            try png.write(to: url)
            OcrHud.toast("已保存：\(url.lastPathComponent)")
        } catch {
            NSApp.activate(ignoringOtherApps: true)
            NSAlert(error: error).runModal()
        }
    }
}
