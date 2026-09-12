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
            "找不到 OCR 引擎程序（mllm_server / mllm_cli）。\(detail)"
        case let .modelMissing(detail):
            "找不到 PaddleOCR-VL 模型文件。\(detail)"
        case let .processFailed(detail):
            "识别失败：\(detail)"
        case .emptyResult:
            "没有识别出文字"
        }
    }
}

/// Supervised mllm_server (cpp/pl/mllm/server) child process + HTTP client.
///
/// The server keeps the model warm across requests: the one-shot CLI pays
/// a multi-second model load on every recognition, the server pays it once
/// at boot. The child is spawned on demand, reused for the app's lifetime,
/// and terminated when the app quits. If a healthy server is already
/// listening on the port (e.g. started by hand), it is used as-is.
///
/// Shared singleton: MllmOcrEngine instances come and go with capture
/// sessions, and the settings window shows/controls the same server.
final class MllmServerClient: @unchecked Sendable {
    static let shared = MllmServerClient()

    /// Snapshot for the settings UI (lock-protected reads).
    struct Status {
        enum Phase {
            case stopped // never booted, or terminated
            case booting
            case running
            case failed
        }

        var phase: Phase = .stopped
        /// A healthy server answers on the port but isn't our child
        /// (started by hand / another app). The restart button only
        /// controls our own child.
        var healthyExternal = false
        /// Binary we booted (or are booting) from.
        var binary: String?
        /// Failure detail / informational note.
        var detail = ""
    }

    private let lock = NSLock()
    private var process: Process?
    /// A failed boot is not retried implicitly (booting is expensive);
    /// the engine falls back to the CLI. restart() resets this.
    private var bootAttempted = false
    private var _status = Status()

    var status: Status {
        lock.lock()
        defer { lock.unlock() }
        return _status
    }

    private func setStatus(_ mutate: (inout Status) -> Void) {
        lock.lock()
        mutate(&_status)
        lock.unlock()
    }

    private let baseURL = URL(string: "http://127.0.0.1:8310")!
    private let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 300
        config.timeoutIntervalForResource = 300
        return URLSession(configuration: config)
    }()

    init() {
        // Don't orphan the daemon when the app quits.
        NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification,
            object: nil,
            queue: nil,
        ) { [weak self] _ in
            self?.terminate()
        }
    }

    func terminate() {
        lock.lock()
        let proc = process
        process = nil
        _status.phase = .stopped
        _status.detail = ""
        lock.unlock()
        proc?.terminate()
    }

    /// Healthy-probe result cache: recognize() probes on every call,
    /// and a hung server would otherwise cost the full 1.5s timeout
    /// each time. A positive answer stays trusted for a few seconds.
    private var lastHealthyAt: Date?

    /// Cheap probe: is a healthy mllm server answering on the port?
    func isHealthy() async -> Bool {
        lock.lock()
        if let last = lastHealthyAt, Date().timeIntervalSince(last) < 5 {
            lock.unlock()
            return true
        }
        lock.unlock()

        var request = URLRequest(url: baseURL.appendingPathComponent("healthz"))
        request.timeoutInterval = 1.5
        guard let (data, response) = try? await session.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200,
              String(data: data, encoding: .utf8)?.contains("\"ok\"") ?? false
        else {
            lock.lock()
            lastHealthyAt = nil
            lock.unlock()
            return false
        }
        lock.lock()
        lastHealthyAt = Date()
        lock.unlock()
        return true
    }

    /// Passive refresh for the settings UI: notices a crashed child and
    /// probes for an external server, without touching the boot state
    /// machine.
    func refresh() async {
        lock.lock()
        if let proc = process, !proc.isRunning {
            process = nil
            if _status.phase == .running || _status.phase == .booting {
                _status.phase = .stopped
                _status.detail = "进程已退出"
            }
        }
        lock.unlock()
        let healthy = await isHealthy()
        setStatus { $0.healthyExternal = healthy && self.process == nil }
    }

    /// Boots mllm_server as a child and waits for /healthz. Returns false
    /// when the boot failed or timed out — the caller then falls back to
    /// the one-shot CLI. Cold model load can take tens of seconds, hence
    /// the generous deadline.
    func start(binary: String, model: String, mmproj: String) async -> Bool {
        lock.lock()
        if bootAttempted {
            lock.unlock()
            return false
        }
        bootAttempted = true
        _status = Status(phase: .booting, binary: binary)
        lock.unlock()

        // A failed boot clears bootAttempted: transient causes (port
        // briefly occupied, models just downloaded) must not lock the
        // app onto the slow CLI fallback for its whole lifetime.
        // The engine's busy flag serializes OCR calls, so at most one
        // boot is ever in flight.
        let fail: (String) -> Bool = { detail in
            self.lock.lock()
            self.bootAttempted = false
            self.lock.unlock()
            self.setStatus { $0.phase = .failed; $0.detail = detail }
            return false
        }

        // Logging: the server self-manages a rotating log file
        // (--log_file, generation-rotated at 10 MB x 3). stderr is
        // redirected to a SEPARATE file for stray non-sink writes (abort
        // messages and such) — sharing one path from two independent
        // writers without O_APPEND would corrupt the log.
        let logURL = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/Application Support/AuraShot/mllm_server.log")
        let errURL = logURL.deletingLastPathComponent()
            .appendingPathComponent("mllm_server.err.log")
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: binary)
        proc.arguments = [
            "--model", model,
            "--mmproj", mmproj,
            "--backend", "metal",
            "--ctx", "8192",
            "--listen", "127.0.0.1",
            "--port", "8310",
            "--log_file", logURL.path,
        ]
        FileManager.default.createFile(atPath: errURL.path, contents: nil)
        if let errHandle = try? FileHandle(forWritingTo: errURL) {
            errHandle.seekToEndOfFile()
            proc.standardOutput = errHandle
            proc.standardError = errHandle
        }

        do {
            try proc.run()
        } catch {
            return fail("进程启动失败：\(error.localizedDescription)")
        }
        lock.lock()
        process = proc
        lock.unlock()

        let deadline = Date().addingTimeInterval(180)
        while Date() < deadline {
            if !proc.isRunning {
                lock.lock()
                if process === proc {
                    process = nil
                }
                lock.unlock()
                return fail("进程提前退出（日志见 ~/Library/Application Support/AuraShot/mllm_server.log）")
            }
            if await isHealthy() {
                setStatus { $0.phase = .running; $0.detail = "" }
                return true
            }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
        proc.terminate()
        lock.lock()
        if process === proc {
            process = nil
        }
        lock.unlock()
        return fail("等待就绪超时（180s，日志见 ~/Library/Application Support/AuraShot/mllm_server.log）")
    }

    /// Manual restart from the settings UI: kills our child (if any) and
    /// boots again. A healthy EXTERNAL server on the port is left alone
    /// (and would make the boot fail on the bind).
    func restart(binary: String, model: String, mmproj: String) async -> Bool {
        terminate()
        lock.lock()
        bootAttempted = false
        lock.unlock()
        return await start(binary: binary, model: model, mmproj: mmproj)
    }

    /// POST /v1/ocr with a base64 PNG; returns the raw recognized text.
    func ocr(png: Data, maxTokens: Int) async throws -> String {
        var request = URLRequest(url: baseURL.appendingPathComponent("v1/ocr"))
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "image": png.base64EncodedString(),
            "max_tokens": maxTokens,
        ])

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: request)
        } catch {
            throw OcrError.processFailed("OCR server 请求失败：\(error.localizedDescription)")
        }

        let status = (response as? HTTPURLResponse)?.statusCode ?? -1
        let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        guard status == 200, let text = object?["text"] as? String else {
            let message = (object?["error"] as? [String: Any])?["message"] as? String
                ?? "HTTP \(status)"
            throw OcrError.processFailed("OCR server：\(message)")
        }
        return text
    }
}

/// OCR via the mllm server (preferred) or the mllm CLI as a one-shot
/// fallback.
///
/// Why a process and not a library link: the inference code is C++20
/// with a Metal backend, and this repo builds C++ debug+ASan by
/// default — embedding it would drag all of that into the app. The
/// server is bundled into Contents/Resources as a release build (see
/// opt_binary in BUILD.bazel) and spawned per OCR session; external
/// installs (custom path / well-known location) take precedence when
/// present, and the one-shot CLI remains as the last resort.
final class MllmOcrEngine: OcrEngine, @unchecked Sendable {
    private let lock = NSLock()
    private var busy = false
    private let server = MllmServerClient.shared

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

        let text = try await recognizeText(image)
        // PaddleOCR-VL's tokenizer emits raw SentencePiece word markers
        // (U+2581) for spaces; users want real whitespace on the
        // clipboard.
        let cleaned = text.replacingOccurrences(of: "▁", with: " ")
        let trimmed = cleaned.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw OcrError.emptyResult }

        // The engine returns plain text without boxes — one block covering
        // the whole image until per-region output exists.
        return [OcrBlock(
            rect: CGRect(x: 0, y: 0, width: image.width, height: image.height),
            text: trimmed,
            kind: .text,
            confidence: 1.0,
        )]
    }

    /// Server first (warm model), CLI one-shot as the fallback.
    private func recognizeText(_ image: CGImage) async throws -> String {
        // 1. A healthy server is already up (spawned by us earlier, or by
        //    the user by hand).
        if await server.isHealthy() {
            return try await server.ocr(png: Self.encodePNG(image), maxTokens: 2048)
        }
        // 2. Boot our own server when the binary is available.
        if let serverBinary = Self.resolveServer() {
            let (model, mmproj) = try Self.resolveModels()
            if await server.start(binary: serverBinary, model: model, mmproj: mmproj) {
                return try await server.ocr(png: Self.encodePNG(image), maxTokens: 2048)
            }
        }
        // 3. One-shot CLI fallback (pays a cold model load per call).
        let cli = try Self.resolveCLI()
        let (model, mmproj) = try Self.resolveModels()
        let imageURL = try Self.writeTempPNG(image)
        defer { try? FileManager.default.removeItem(at: imageURL) }
        return try await Self.runCLI(cli: cli, model: model, mmproj: mmproj, image: imageURL)
    }

    // MARK: - Discovery

    /// mllm_server binary: 1. explicit engine path from Settings,
    /// 2. sibling of the legacy CLI path, 3. well-known install location,
    /// 4. bundled inside AuraShot.app. External installs win so a newer
    /// engine can be tried without rebuilding the app. nil when absent
    /// (CLI fallback).
    static func resolveServer() -> String? {
        let engine = Settings.shared.ocrEnginePath
        if !engine.isEmpty, FileManager.default.isExecutableFile(atPath: engine) {
            return engine
        }
        let legacyCli = Settings.shared.ocrCliPath
        if !legacyCli.isEmpty {
            let sibling = (legacyCli as NSString).deletingLastPathComponent + "/mllm_server"
            if FileManager.default.isExecutableFile(atPath: sibling) {
                return sibling
            }
        }
        let defaultLocation = NSHomeDirectory()
            + "/Library/Application Support/AuraShot/mllm_server"
        if FileManager.default.isExecutableFile(atPath: defaultLocation) {
            return defaultLocation
        }
        if let bundled = Bundle.main.path(forResource: "mllm_server", ofType: nil),
           FileManager.default.isExecutableFile(atPath: bundled)
        {
            return bundled
        }
        return nil
    }

    /// One-shot CLI fallback: 1. legacy CLI path from Settings,
    /// 2. sibling of the engine path, 3. well-known install location.
    static func resolveCLI() throws -> String {
        let legacyCli = Settings.shared.ocrCliPath
        if !legacyCli.isEmpty {
            guard FileManager.default.isExecutableFile(atPath: legacyCli) else {
                throw OcrError.cliMissing("设置里配置的路径不可执行：\(legacyCli)")
            }
            return legacyCli
        }
        let engine = Settings.shared.ocrEnginePath
        if !engine.isEmpty {
            let sibling = (engine as NSString).deletingLastPathComponent + "/mllm_cli"
            if FileManager.default.isExecutableFile(atPath: sibling) {
                return sibling
            }
        }
        let defaultLocation = NSHomeDirectory()
            + "/Library/Application Support/AuraShot/mllm_cli"
        if FileManager.default.isExecutableFile(atPath: defaultLocation) {
            return defaultLocation
        }
        throw OcrError.cliMissing("""
        应用 bundle 内未找到 mllm_server（正常安装应自带）。
        可手动安装 server（模型常驻、识别更快）：
          bazel build //cpp/pl/mllm/server:mllm_server --config=release
          mkdir -p ~/Library/Application\\ Support/AuraShot
          cp bazel-bin/cpp/pl/mllm/server/mllm_server ~/Library/Application\\ Support/AuraShot/
        或一次性 CLI：
          bazel build //cpp/pl/mllm/cli:mllm_cli --config=release
          cp bazel-bin/cpp/pl/mllm/cli/mllm_cli ~/Library/Application\\ Support/AuraShot/
        或在 偏好设置 里指定引擎路径。
        """)
    }

    /// The PaddleOCR-VL checkpoint pair under the configured directory.
    /// Without an explicit setting, the legacy ~/models location is
    /// probed as a fallback so existing installs keep working.
    static func resolveModels() throws -> (model: String, mmproj: String) {
        var dirs = [Settings.shared.ocrModelDir.path]
        if !Settings.shared.isModelDirCustomized {
            let legacy = Settings.legacyModelDir.path
            if legacy != dirs[0] {
                dirs.append(legacy)
            }
        }
        for dir in dirs {
            let model = dir + "/PaddleOCR-VL-1.6-GGUF.gguf"
            let mmproj = dir + "/PaddleOCR-VL-1.6-GGUF-mmproj.gguf"
            if FileManager.default.fileExists(atPath: model),
               FileManager.default.fileExists(atPath: mmproj)
            {
                return (model, mmproj)
            }
        }
        throw OcrError.modelMissing("目录 \(dirs[0]) 下缺少 PaddleOCR-VL-1.6-GGUF{,-mmproj}.gguf")
    }

    // MARK: - Process

    private static func encodePNG(_ image: CGImage) throws -> Data {
        guard let png = NSBitmapImageRep(cgImage: image)
            .representation(using: .png, properties: [:])
        else {
            throw OcrError.processFailed("PNG 编码失败")
        }
        return png
    }

    private static func writeTempPNG(_ image: CGImage) throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("aurashot-ocr-\(UUID().uuidString).png")
        try encodePNG(image).write(to: url)
        return url
    }

    /// PaddleOCR-VL was trained behind this exact chat template (see the
    /// GGUF's tokenizer.chat_template: "<|begin_of_sentence|>User: ...\n"
    /// "Assistant:\n"). Sending the bare task prefix "OCR:" puts the model
    /// off-distribution: it never emits EOS and hallucinates page after page
    /// of garbage until the token cap — that was both the slowness and the
    /// garbled output. With the full template it stops right after the real
    /// text (~3x faster and clean output, verified on doc + screenshot
    /// samples).
    private static let ocrPrompt =
        "<|begin_of_sentence|>User: <|IMAGE_START|><|IMAGE_PLACEHOLDER|><|IMAGE_END|>"
            + "OCR:\nAssistant:\n"

    private static func runCLI(cli: String, model: String, mmproj: String, image: URL) async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            let process = Process()
            process.executableURL = URL(fileURLWithPath: cli)
            process.arguments = [
                "-m", model,
                "--mmproj", mmproj,
                "-p", ocrPrompt,
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
                // Process exit does NOT imply the pipe was drained by
                // the handlers — whatever is still buffered would be
                // lost, truncating the tail of the output. The process
                // is dead, so these reads hit EOF immediately.
                let outTail = stdoutPipe.fileHandleForReading.readDataToEndOfFile()
                let errTail = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                lock.lock()
                stdoutData.append(outTail)
                stderrData.append(errTail)
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
