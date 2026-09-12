import AppKit
import Foundation

enum OcrError: LocalizedError {
    case cliMissing(String)
    case modelMissing(String)
    case processFailed(String)
    case emptyResult

    var errorDescription: String? {
        switch self {
        case let .cliMissing(detail):
            "找不到 OCR 引擎程序（mllm_cli）。\(detail)"
        case let .modelMissing(detail):
            "找不到 PaddleOCR-VL 模型文件。\(detail)"
        case let .processFailed(detail):
            "识别失败：\(detail)"
        case .emptyResult:
            "没有识别出文字"
        }
    }
}

/// OCR via the mllm CLI (cpp/pl/mllm) as a one-shot child process.
///
/// Why a process and not a library link: the inference code is C++20
/// with a Metal backend, and this repo builds C++ debug+ASan by
/// default — embedding it would drag all of that into the app. A CLI
/// built with --config=release runs at full speed and upgrades
/// independently. The binary is discovered (see resolveCLI) rather
/// than bundled.
final class MllmOcrEngine: OcrEngine, @unchecked Sendable {
    private let lock = NSLock()
    private var busy = false

    func recognize(_ image: CGImage) async throws -> [OcrBlock] {
        lock.lock()
        if busy {
            lock.unlock()
            throw OcrError.processFailed("上一次识别还在进行中")
        }
        busy = true
        lock.unlock()
        defer {
            lock.lock()
            busy = false
            lock.unlock()
        }

        let cli = try Self.resolveCLI()
        let (model, mmproj) = try Self.resolveModels()
        let imageURL = try Self.writeTempPNG(image)
        defer { try? FileManager.default.removeItem(at: imageURL) }

        let text = try await Self.runCLI(cli: cli, model: model, mmproj: mmproj, image: imageURL)
        // PaddleOCR-VL's tokenizer emits raw SentencePiece word markers
        // (U+2581) for spaces; users want real whitespace on the
        // clipboard.
        let cleaned = text.replacingOccurrences(of: "▁", with: " ")
        let trimmed = cleaned.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw OcrError.emptyResult }

        // The CLI returns plain text without boxes — one block covering
        // the whole image until per-region output exists.
        return [OcrBlock(
            rect: CGRect(x: 0, y: 0, width: image.width, height: image.height),
            text: trimmed,
            kind: .text,
            confidence: 1.0,
        )]
    }

    // MARK: - Discovery

    /// 1. explicit path from Settings, 2. well-known install location.
    static func resolveCLI() throws -> String {
        let custom = Settings.shared.ocrCliPath
        if !custom.isEmpty {
            guard FileManager.default.isExecutableFile(atPath: custom) else {
                throw OcrError.cliMissing("设置里配置的路径不可执行：\(custom)")
            }
            return custom
        }
        let defaultLocation = NSHomeDirectory()
            + "/Library/Application Support/AuraShot/mllm_cli"
        if FileManager.default.isExecutableFile(atPath: defaultLocation) {
            return defaultLocation
        }
        throw OcrError.cliMissing("""
        请构建并放置：
          bazel build //cpp/pl/mllm/cli:mllm_cli --config=release
          mkdir -p ~/Library/Application\\ Support/AuraShot
          cp bazel-bin/cpp/pl/mllm/cli/mllm_cli ~/Library/Application\\ Support/AuraShot/
        或在 偏好设置 里指定引擎路径。
        """)
    }

    /// The PaddleOCR-VL checkpoint pair under the configured directory.
    static func resolveModels() throws -> (model: String, mmproj: String) {
        let dir = Settings.shared.ocrModelDir.path
        let model = dir + "/PaddleOCR-VL-1.6-GGUF.gguf"
        let mmproj = dir + "/PaddleOCR-VL-1.6-GGUF-mmproj.gguf"
        guard FileManager.default.fileExists(atPath: model),
              FileManager.default.fileExists(atPath: mmproj) else {
            throw OcrError.modelMissing("目录 \(dir) 下缺少 PaddleOCR-VL-1.6-GGUF{,-mmproj}.gguf")
        }
        return (model, mmproj)
    }

    // MARK: - Process

    private static func writeTempPNG(_ image: CGImage) throws -> URL {
        guard let png = NSBitmapImageRep(cgImage: image)
            .representation(using: .png, properties: [:]) else {
            throw OcrError.processFailed("PNG 编码失败")
        }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("aurashot-ocr-\(UUID().uuidString).png")
        try png.write(to: url)
        return url
    }

    private static func runCLI(cli: String, model: String, mmproj: String, image: URL) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            let process = Process()
            process.executableURL = URL(fileURLWithPath: cli)
            process.arguments = [
                "-m", model,
                "--mmproj", mmproj,
                "-p", "OCR:",
                "--image", image.path,
                "-n", "2048",
                "--backend", "metal",
                "--ctx", "8192",
            ]
            let stdoutPipe = Pipe()
            let stderrPipe = Pipe()
            process.standardOutput = stdoutPipe
            process.standardError = stderrPipe

            var stdoutData = Data()
            var stderrData = Data()
            let lock = NSLock()
            var resumed = false
            let finish: (Result<String, Error>) -> Void = { result in
                lock.lock()
                defer { lock.unlock() }
                guard !resumed else { return }
                resumed = true
                continuation.resume(with: result)
            }

            stdoutPipe.fileHandleForReading.readabilityHandler = { handle in
                lock.lock()
                stdoutData.append(handle.availableData)
                lock.unlock()
            }
            stderrPipe.fileHandleForReading.readabilityHandler = { handle in
                lock.lock()
                stderrData.append(handle.availableData)
                lock.unlock()
            }

            // Generous timeout: cold model load on a big image can be slow.
            let watchdog = DispatchWorkItem {
                process.terminate()
                finish(.failure(OcrError.processFailed("超时（120s）")))
            }
            DispatchQueue.global().asyncAfter(deadline: .now() + 120, execute: watchdog)

            process.terminationHandler = { proc in
                watchdog.cancel()
                stdoutPipe.fileHandleForReading.readabilityHandler = nil
                stderrPipe.fileHandleForReading.readabilityHandler = nil
                lock.lock()
                let out = stdoutData
                let err = stderrData
                lock.unlock()
                guard proc.terminationStatus == 0 else {
                    let detail = String(data: err, encoding: .utf8)?
                        .trimmingCharacters(in: .whitespacesAndNewlines) ?? "exit \(proc.terminationStatus)"
                    finish(.failure(OcrError.processFailed(detail)))
                    return
                }
                finish(.success(String(data: out, encoding: .utf8) ?? ""))
            }

            do {
                try process.run()
            } catch {
                watchdog.cancel()
                finish(.failure(OcrError.processFailed(error.localizedDescription)))
            }
        }
    }
}
