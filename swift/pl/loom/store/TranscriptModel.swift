// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import AppKit
import CryptoKit
import Foundation

// MARK: - Transcript model (derived render data, built on state change)

/// Everything the transcript view needs, derived from the authoritative
/// message list in ONE pass. Previously these were SwiftUI computed
/// properties (toolResults / visibleMessages / actionMessageIds, plus a
/// per-row assistantItems and the ToolBlock model — LCS diff included),
/// which meant every streaming frame re-walked the entire history
/// three times and re-ran every edit's 400x400 LCS DP. SessionStore
/// rebuilds this model only when `messages` or `state` actually change
/// (turn boundaries), and the per-tool render model is memoized by
/// call content, so a body re-evaluation is pure lookup.
struct TranscriptModel {
    /// One transcript line: a message row, or a turn-summary card
    /// appended at a finished turn's boundary (WebUI closeTurn — the
    /// card has no message of its own; it is keyed by run_id).
    enum Row: Identifiable, Equatable {
        case message(MessageRowModel)
        case turnSummary(TurnSummary)

        var id: String {
            switch self {
            case let .message(model):
                model.message.id
            case let .turnSummary(summary):
                "tsm-\(summary.runId ?? "turn\(summary.turn ?? 0)")"
            }
        }
    }

    /// The message-row payload (the old Row struct): the source
    /// message plus its precomputed render items and the
    /// turn-boundary action flag.
    struct MessageRowModel: Equatable {
        let message: Message
        let items: [Item]
        let showActions: Bool
    }

    /// One render unit inside an assistant row (tool_call + tool_result
    /// already merged, tool render model precomputed).
    enum Item: Equatable {
        case markdown(String)
        case reasoning(ContentPart.Reasoning)
        case tool(ToolRenderModel)
        case image(ContentPart.ImageContent)
        case artifact(ContentPart.Artifact)
    }

    static let empty = TranscriptModel(rows: [])

    let rows: [Row]

    /// Builds the model from the authoritative history. `midTurn` is
    /// the ChatView gate (running/cancelling/awaitingApproval) that
    /// keeps the action row off the transcript tail while a turn is
    /// in flight (WebUI closeTurn). `turnSummaries` is the snapshot's
    /// per-turn file-change projection: each becomes a closing card
    /// appended at its turn boundary.
    static func build(
        messages: [Message], midTurn: Bool, turnSummaries: [TurnSummary] = [],
    ) -> TranscriptModel {
        // Cross-message call_id → result map (the WebUI's histTools):
        // tool results arrive in their own assistant messages and patch
        // the block created for the matching call.
        var toolResults: [String: ContentPart.ToolResult] = [:]
        for message in messages where message.role == .assistant {
            for part in message.parts {
                if case let .toolResult(result) = part {
                    toolResults[result.callId] = result
                }
            }
        }

        // WebUI closeTurn: the action row attaches to the LAST text
        // segment of a finished turn — settled at the next user message,
        // and at the transcript tail only when no turn is in flight.
        var actionIds: Set<String> = []
        var candidate: String?
        for message in messages {
            switch message.role {
            case .user:
                if let candidate {
                    actionIds.insert(candidate)
                }
                candidate = nil
            case .assistant:
                if !message.copyText.isEmpty {
                    candidate = message.id
                }
            default:
                break
            }
        }
        if !midTurn, let candidate {
            actionIds.insert(candidate)
        }

        // Per-turn file-change projection (WebUI transcript.ts): keyed
        // by run_id — the same run_id stamped into assistant message
        // metadata — each summary becomes a closing card emitted at
        // its turn boundary (before the next user bubble; at the tail).
        var summariesByRun: [String: TurnSummary] = [:]
        for summary in turnSummaries {
            if let runId = summary.runId, !(summary.changes ?? []).isEmpty {
                summariesByRun[runId] = summary
            }
        }
        var emitted: Set<String> = []
        var lastRunId = ""

        var rows: [Row] = []
        rows.reserveCapacity(messages.count + summariesByRun.count)

        func emitSummary() {
            guard !lastRunId.isEmpty, !emitted.contains(lastRunId),
                  let summary = summariesByRun[lastRunId]
            else { return }
            emitted.insert(lastRunId)
            rows.append(.turnSummary(summary))
        }

        for message in messages {
            switch message.role {
            case .user:
                // Turn boundary: close out the previous turn's file
                // summary before opening the next bubble (no-op when
                // the run wrote no files).
                emitSummary()
                rows.append(.message(MessageRowModel(message: message, items: [], showActions: false)))
            case .assistant:
                // The agent loop stamps run_id metadata when persisting
                // the message; a run's summaries key off its LAST one.
                if let runId = message.metadata?["run_id"], !runId.isEmpty {
                    if runId != lastRunId {
                        emitSummary()
                        lastRunId = runId
                    }
                }
                let items = assistantItems(of: message, toolResults: toolResults)
                // Pure tool_result carriers fold into their call's block
                // and leave no row of their own (WebUI parity).
                if items.isEmpty {
                    continue
                }
                rows.append(.message(MessageRowModel(
                    message: message,
                    items: items,
                    showActions: actionIds.contains(message.id),
                )))
            default:
                rows.append(.message(MessageRowModel(message: message, items: [], showActions: false)))
            }
        }
        emitSummary()
        return TranscriptModel(rows: rows)
    }

    /// Flattens an assistant message's parts into render items, folding
    /// each tool_call together with the tool_result that shares its
    /// call_id (WebUI buildFromSnapshot: results only PATCH the block
    /// created for the call — they never create a block of their own).
    private static func assistantItems(
        of message: Message,
        toolResults: [String: ContentPart.ToolResult],
    ) -> [Item] {
        var items: [Item] = []
        for part in message.parts {
            switch part {
            case let .text(text):
                if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                    items.append(.markdown(text))
                }
            case let .reasoning(reasoning):
                items.append(.reasoning(reasoning))
            case let .toolCall(call):
                items.append(.tool(ToolRenderModel(
                    item: ToolBlockItem(call: call, result: toolResults[call.id]),
                )))
            case .toolResult:
                // Consumed by the pairing map; an orphaned result (no
                // call anywhere) is dropped, exactly like the WebUI.
                break
            case let .image(image):
                items.append(.image(image))
            case let .artifact(artifact):
                // WebUI transcript.ts: model_only artifacts (view_image)
                // skip the display channel entirely.
                if !artifact.modelOnly {
                    items.append(.artifact(artifact))
                }
            case .unknown:
                break
            }
        }
        return items
    }
}

// MARK: - Message helpers

/// A tool call merged with its result (same call_id) — the WebUI's
/// single .block-tool per invocation.
struct ToolBlockItem {
    let call: ContentPart.ToolCall?
    let result: ContentPart.ToolResult?

    var name: String {
        call?.name ?? "tool"
    }
}

extension Message {
    /// Text copied by the message action row (prose parts only).
    var copyText: String {
        parts.compactMap { part in
            if case let .text(text) = part {
                return text
            }
            return nil
        }.joined(separator: "\n\n")
    }
}

// MARK: - Tool render model (shared by history blocks and live draft)

/// Normalized tool-card render data — the former ToolBlock.Model, moved
/// out of the view so the expensive derivation (excerpting, image/
/// artifact extraction, the LCS diff) happens in TranscriptModel.build
/// instead of on every view-body evaluation. Diff results are memoized
/// by content, so repeated snapshot rebuilds (every turn end) do not
/// re-run the DP.
struct ToolRenderModel: Equatable {
    var name = "tool"
    var callId: String?
    var target: String?
    var status = Status.running
    var durationMs: Int64?
    var errorMessage: String?
    /// Display excerpt: 600 chars + "\n…" for history; the live
    /// preview arrives pre-bounded from tool.completed.
    var output: String?
    /// Copy source (WebUI getFullText); the live path only has the
    /// bounded preview.
    var fullOutput: String?
    var diff: String?
    var images: [ContentPart.ImageContent] = []
    var artifacts: [ContentPart.Artifact] = []
    /// Text attachments already represented by this command's output disclosure.
    var outputAttachments: [OutputAttachment] = []
    var commandOutputTruncated = false
    var commandOutputFormatted = false

    struct OutputAttachment: Equatable {
        let name: String
        let artifact: ContentPart.Artifact
    }

    enum Status {
        case running, success, failed, cancelled
    }

    /// Finalized history block (snapshot rebuild).
    init(item: ToolBlockItem) {
        name = item.name
        callId = item.call?.id ?? item.result?.callId
        if let result = item.result {
            if result.error != nil {
                status = .failed
            } else {
                switch result.status {
                case "success", "ok", "done": status = .success
                case "cancelled", "canceled": status = .cancelled
                case "error", "timeout", "failed": status = .failed
                default: status = .success
                }
            }
            errorMessage = result.error?.message
            // WebUI histCompletion: duration = finished_at − started_at.
            durationMs = result.durationMs
        } else {
            status = .running
        }
        target = Self.displayTarget(of: item)
        if let full = Self.fullOutputText(of: item) {
            fullOutput = full
            // Display excerpt (WebUI histCompletion: 600 chars + "\n…").
            output = full.count > 600 ? String(full.prefix(600)) + "\n…" : full
        }
        images = Self.resultImages(of: item)
        artifacts = Self.resultArtifacts(of: item, images: images)
        mergeCommandOutput()
        diff = Self.diffText(of: item)
    }

    /// Live draft-turn block: the event stream's ToolCallState —
    /// server-provided target/diff, tool.completed's bounded preview in
    /// the same output disclosure history uses, and the completion's
    /// display-bound artifact refs.
    init(live state: ToolCallState) {
        name = state.name
        callId = state.id
        target = state.target
        switch state.status {
        case .prepared, .running:
            status = .running
        case .success, .unknown:
            status = .success
        case .error, .timeout:
            status = .failed
        case .cancelled:
            status = .cancelled
        }
        durationMs = state.durationMs
        errorMessage = state.errorMessage
        if let preview = state.preview, !preview.isEmpty {
            output = preview
            fullOutput = preview
        }
        diff = state.diff
        artifacts = state.artifacts
        mergeCommandOutput()
    }

    /// Parse only complete run_cmd JSON. A live preview may contain a truncated
    /// JSON prefix; leave both it and its attachments untouched in that case.
    private mutating func mergeCommandOutput() {
        guard name == "run_cmd", let fullOutput,
              let data = fullOutput.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let stdout = json["stdout"] as? String,
              let stderr = json["stderr"] as? String
        else { return }

        var lines: [String] = []
        for (name, body) in [("stdout", stdout), ("stderr", stderr)] {
            let clipped = json["\(name)_preview_truncated"] as? Bool == true
            if !body.isEmpty {
                lines.append("\(name)\(clipped ? " (preview truncated)" : ""):\n\(body)")
            }
            if let ref = json["\(name)_artifact"] as? [String: Any],
               let id = ref["id"] as? String,
               ref["media_type"] == nil || ref["media_type"] as? String == "text/plain",
               let index = artifacts.firstIndex(where: { $0.id == id && ($0.mediaType == nil || $0.mediaType == "text/plain") })
            {
                outputAttachments.append(OutputAttachment(name: name, artifact: artifacts.remove(at: index)))
            }
        }
        var status: [String] = []
        if let exitCode = json["exit_code"] as? Int {
            status.append("exit code: \(exitCode)")
        }
        if let signal = json["signal"] as? String, !signal.isEmpty {
            status.append("signal: \(signal)")
        }
        if json["timed_out"] as? Bool == true {
            status.append("timed out")
        }
        if json["cancelled"] as? Bool == true {
            status.append("cancelled")
        }
        if !status.isEmpty {
            lines.append(status.joined(separator: " · "))
        }
        if let note = json["note"] as? String, !note.isEmpty {
            lines.append("note: \(note)")
        }
        commandOutputFormatted = true
        let formatted = lines.joined(separator: "\n\n")
        let rendered = formatted.isEmpty ? "(no stdout/stderr)" : formatted
        let excerptTruncated = rendered.count > 600
        commandOutputTruncated = excerptTruncated || json["truncated"] as? Bool == true ||
            json["stdout_preview_truncated"] as? Bool == true ||
            json["stderr_preview_truncated"] as? Bool == true
        output = excerptTruncated ? String(rendered.prefix(600)) + "\n…" : rendered
        self.fullOutput = rendered
    }

    // MARK: Derivation (snapshot history)

    /// Base64 image parts inside the tool result's content.
    private static func resultImages(of item: ToolBlockItem) -> [ContentPart.ImageContent] {
        guard let content = item.result?.content else { return [] }
        return content.compactMap { part in
            if case let .image(image) = part {
                return image
            }
            return nil
        }
    }

    /// Artifact references inside the result content — only rendered
    /// when the result carries no inline images (WebUI precedence).
    /// model_only artifacts (view_image) are for the model only: the
    /// WebUI's histCompletion filters them out, and so do we.
    private static func resultArtifacts(
        of item: ToolBlockItem, images: [ContentPart.ImageContent],
    ) -> [ContentPart.Artifact] {
        guard images.isEmpty, let content = item.result?.content else { return [] }
        return content.compactMap { part in
            if case let .artifact(artifact) = part, !artifact.modelOnly {
                return artifact
            }
            return nil
        }
    }

    /// Full output text: the result's text parts joined, falling back
    /// to the call's arguments while the call is still running.
    private static func fullOutputText(of item: ToolBlockItem) -> String? {
        if let content = item.result?.content {
            let text = content.compactMap { part -> String? in
                if case let .text(text) = part {
                    return text
                }
                return nil
            }.joined(separator: "\n")
            if !text.isEmpty {
                return text
            }
        }
        if item.result == nil, let args = item.call?.arguments?.prettyPrinted, !args.isEmpty {
            return args
        }
        return nil
    }

    /// Header target: the canonical file path / command from the args.
    private static func displayTarget(of item: ToolBlockItem) -> String? {
        guard let args = item.call?.arguments else { return nil }
        for key in ["path", "file_path", "cmd", "command", "pattern", "query", "goal", "task"] {
            if let value = args[key]?.stringValue, !value.isEmpty {
                return value.count > 120 ? String(value.prefix(120)) + "…" : value
            }
        }
        return nil
    }

    /// Snapshot rebuilds carry no diff; recompute one from edit/write
    /// args — one-to-one with the WebUI's diffForToolCall (diff.ts):
    /// write = pure addition; edit = two-sided LCS (400-line cap per
    /// side), and the `+++ b/{path}` header feeds the DiffView's file
    /// label and highlight language. Memoized by content: a session
    /// re-renders its whole history on every turn end, but an edit's
    /// diff never changes once the call is finalized.
    private static func diffText(of item: ToolBlockItem) -> String? {
        guard let name = item.call?.name, let args = item.call?.arguments else { return nil }
        let oldText: String
        let newText: String
        switch name {
        case "edit":
            guard let newString = args["new_string"]?.stringValue else { return nil }
            oldText = args["old_string"]?.stringValue ?? ""
            newText = newString
        case "write":
            guard let content = args["content"]?.stringValue else { return nil }
            oldText = ""
            newText = content
        default:
            return nil
        }
        let path = args["path"]?.stringValue ?? args["file_path"]?.stringValue
        return ToolDiffCache.diff(oldText: oldText, newText: newText, path: path)
    }
}

// MARK: - Diff computation (diff.ts parity) + memoization

/// Memoizes rendered unified diffs by (old, new, path) content. The
/// LCS DP is the transcript's single most expensive pure computation
/// (up to 400x400 cells per edit call) and its inputs are immutable
/// once the call is finalized — a perfect cache.
enum ToolDiffCache {
    private static let cache: NSCache<NSString, NSString> = {
        let cache = NSCache<NSString, NSString>()
        cache.countLimit = 200
        cache.totalCostLimit = 32 * 1024 * 1024
        return cache
    }()

    /// Returns the display diff (WebUI diffForToolCall), or nil when
    /// old == new. `path` supplies the `+++ b/{path}` header.
    static func diff(oldText: String, newText: String, path: String?) -> String? {
        guard oldText != newText else { return nil }
        let key = "\(oldText)\u{1}\(newText)\u{1}\(path ?? "")" as NSString
        if let cached = cache.object(forKey: key) {
            return cached as String
        }
        var text = diffTexts(oldText, newText)
        if !text.isEmpty, let path, !path.isEmpty {
            text = "+++ b/\(path)\n" + text
        }
        // The key retains both inputs; charge it as well as the result.
        let cost = key.length * 2 + (text as NSString).length * 2
        if cost <= 32 * 1024 * 1024 {
            cache.setObject(text as NSString, forKey: key, cost: cost)
        }
        return text.isEmpty ? nil : text
    }

    /// diff.ts DIFF_MAX_INPUT_LINES.
    private static let diffMaxInputLines = 400

    private static func splitDiffLines(_ text: String) -> [String] {
        guard !text.isEmpty else { return [] }
        var text = text
        if text.hasSuffix("\n") {
            text.removeLast()
        }
        return text.components(separatedBy: "\n")
    }

    private struct DiffOp {
        enum Kind { case ctx, del, add }
        let kind: Kind
        let line: String
    }

    /// Line-level LCS diff — the same DP and backtrack direction as
    /// diff.ts (del wins ties), so both clients render identical hunks.
    private static func lcsDiff(_ oldLines: [String], _ newLines: [String]) -> [DiffOp] {
        let n = oldLines.count
        let m = newLines.count
        var dp = [[Int]](repeating: [Int](repeating: 0, count: m + 1), count: n + 1)
        if n > 0, m > 0 {
            for i in stride(from: n - 1, through: 0, by: -1) {
                for j in stride(from: m - 1, through: 0, by: -1) {
                    dp[i][j] = oldLines[i] == newLines[j]
                        ? dp[i + 1][j + 1] + 1
                        : max(dp[i + 1][j], dp[i][j + 1])
                }
            }
        }
        var ops: [DiffOp] = []
        var i = 0
        var j = 0
        while i < n, j < m {
            if oldLines[i] == newLines[j] {
                ops.append(DiffOp(kind: .ctx, line: oldLines[i]))
                i += 1
                j += 1
            } else if dp[i + 1][j] >= dp[i][j + 1] {
                ops.append(DiffOp(kind: .del, line: oldLines[i]))
                i += 1
            } else {
                ops.append(DiffOp(kind: .add, line: newLines[j]))
                j += 1
            }
        }
        while i < n {
            ops.append(DiffOp(kind: .del, line: oldLines[i]))
            i += 1
        }
        while j < m {
            ops.append(DiffOp(kind: .add, line: newLines[j]))
            j += 1
        }
        return ops
    }

    /// diff.ts diffTexts: changed lines keep 1 line of context above
    /// and below; unchanged runs collapse into "...".
    private static func diffTexts(_ oldText: String, _ newText: String) -> String {
        if oldText == newText {
            return ""
        }
        let ops: [DiffOp] = if oldText.isEmpty || newText.isEmpty {
            splitDiffLines(oldText).map { DiffOp(kind: .del, line: $0) }
                + splitDiffLines(newText).map { DiffOp(kind: .add, line: $0) }
        } else {
            lcsDiff(
                Array(splitDiffLines(oldText).prefix(diffMaxInputLines)),
                Array(splitDiffLines(newText).prefix(diffMaxInputLines)),
            )
        }
        var show = [Bool](repeating: false, count: ops.count)
        for (index, op) in ops.enumerated() where op.kind != .ctx {
            show[index] = true
            if index > 0 {
                show[index - 1] = true
            }
            if index + 1 < ops.count {
                show[index + 1] = true
            }
        }
        var out: [String] = []
        var skipped = false
        for (index, op) in ops.enumerated() {
            guard show[index] else {
                skipped = true
                continue
            }
            if skipped, !out.isEmpty {
                out.append("...")
            }
            skipped = false
            let prefix = switch op.kind {
            case .ctx: "  "
            case .del: "- "
            case .add: "+ "
            }
            out.append(prefix + op.line)
        }
        return out.joined(separator: "\n")
    }
}

// MARK: - Parsed unified diff (DiffView input)

/// diff.ts parseDiff: the `+++ b/…` header supplies the file label;
/// unmatched lines degrade to context (lenient, same as the WebUI).
struct ParsedDiff {
    enum Kind { case hunk, add, del, ctx }

    struct Line {
        let kind: Kind
        let sign: String
        let text: String
    }

    var file = ""
    var lines: [Line] = []
    var adds = 0
    var dels = 0
}

func parseDiff(_ diffText: String) -> ParsedDiff {
    var out = ParsedDiff()
    var sawContent = false
    for raw in diffText.components(separatedBy: "\n") {
        if raw.hasPrefix("+++ ") {
            var file = String(raw.dropFirst(4))
            if file.hasPrefix("b/") {
                file.removeFirst(2)
            }
            out.file = file
            continue
        }
        if raw.hasPrefix("--- ") || raw.hasPrefix("diff ") || raw.hasPrefix("index ") {
            continue
        }
        if raw.hasPrefix("@@") {
            out.lines.append(ParsedDiff.Line(kind: .hunk, sign: "", text: raw))
            sawContent = true
            continue
        }
        var kind: ParsedDiff.Kind = .ctx
        var sign = " "
        var text = raw
        if raw.hasPrefix("+") {
            kind = .add
            sign = "+"
            text = String(raw.dropFirst())
            out.adds += 1
        } else if raw.hasPrefix("-") {
            kind = .del
            sign = "−"
            text = String(raw.dropFirst())
            out.dels += 1
        } else if raw.hasPrefix(" ") {
            text = String(raw.dropFirst())
        } else if raw.isEmpty, !sawContent {
            continue
        }
        out.lines.append(ParsedDiff.Line(kind: kind, sign: sign, text: text))
        sawContent = true
    }
    return out
}

// MARK: - Diff parse cache

/// Memoizes ParsedDiff by source text: DiffView re-evaluates `parsed`
/// on every transcript frame while the diff string itself is stable.
enum DiffParseCache {
    private final class Box: NSObject {
        let value: ParsedDiff

        init(_ value: ParsedDiff) {
            self.value = value
        }
    }

    private static let cache: NSCache<NSString, Box> = {
        let cache = NSCache<NSString, Box>()
        cache.countLimit = 200
        cache.totalCostLimit = 32 * 1024 * 1024
        return cache
    }()

    static func parse(_ text: String) -> ParsedDiff {
        let key = text as NSString
        if let cached = cache.object(forKey: key) {
            return cached.value
        }
        let parsed = parseDiff(text)
        // Parsed lines copy text out of the source; include both the
        // original diff key and all retained line strings in the cost.
        let cost = key.length * 2 + parsed.lines.reduce(0) { $0 + $1.text.utf16.count * 2 + 64 }
        if cost <= 32 * 1024 * 1024 {
            cache.setObject(Box(parsed), forKey: key, cost: cost)
        }
        return parsed
    }
}

// MARK: - Inline image decode cache

/// base64 → NSImage memoization for InlineImageView. Without it every
/// transcript re-evaluation (each streaming frame) re-decodes every
/// visible image — base64 decode + bitmap creation for what is often
/// a multi-MB PNG.
enum InlineImageCache {
    private static let byteLimit = 64 * 1024 * 1024
    private static let cache: NSCache<NSString, NSImage> = {
        let cache = NSCache<NSString, NSImage>()
        cache.countLimit = 100
        cache.totalCostLimit = byteLimit
        return cache
    }()

    static func image(base64: String) -> NSImage? {
        // A cryptographic digest avoids retaining a second multi-MB copy
        // of the base64 payload as the cache key. Hashing is linear, but
        // hits still avoid base64 decoding and image construction.
        let key = SHA256.hash(data: Data(base64.utf8)).map { String(format: "%02x", $0) }
            .joined() as NSString
        if let cached = cache.object(forKey: key) {
            return cached
        }
        guard let data = Data(base64Encoded: base64), let image = NSImage(data: data) else {
            return nil
        }
        // AppKit may lazily decode image representations. Budget the
        // backing bitmap when known, not just the compressed PNG bytes.
        let bitmapBytes = image.representations.compactMap { $0 as? NSBitmapImageRep }
            .map { $0.bytesPerRow * $0.pixelsHigh }.max() ?? 0
        let cost = max(data.count, bitmapBytes)
        if cost <= byteLimit {
            cache.setObject(image, forKey: key, cost: cost)
        }
        return image
    }
}
