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
import JavaScriptCore
@testable import Loom
import SwiftUI
import XCTest

/// Before/after benchmarks for the transcript performance work. Each
/// case measures the OLD call pattern (recompute on every streaming
/// frame — what the computed-property/ToolBlock.init design did)
/// against the NEW one (derive once, memoize, look up) in the same
/// process, and prints a `LOOM-BENCH` line with both numbers. The
/// "before" code paths are faithful re-implementations of the removed
/// call patterns over the same underlying algorithms.
final class PerfBenchmarksTests: XCTestCase {
    private static let frames = 60 // one second of streaming at 60fps

    private func timed(_ body: () -> Void) -> Double {
        let start = ContinuousClock.now
        body()
        let elapsed = ContinuousClock.now - start
        return Double(elapsed.components.seconds) * 1000
            + Double(elapsed.components.attoseconds) / 1e15 // ms
    }

    // MARK: 1. Diff computation — per-frame recompute vs memoized

    /// A finalized edit call's diff is immutable, but the old design
    /// re-ran the 400x400 LCS DP on every view-body evaluation (every
    /// streaming frame). The new design computes it once in
    /// TranscriptModel.build and serves every later frame from
    /// ToolDiffCache.
    func testBenchDiffComputation() {
        // 8 edit calls with 400-line diffs (the DIFF_MAX_INPUT_LINES cap).
        let calls: [(old: String, new: String)] = (0 ..< 8).map { index in
            let old = (0 ..< 400).map { line in
                line % 3 == 0 ? "let value\(line) = \(line)" : "    body(\(index), \(line))"
            }.joined(separator: "\n")
            let new = (0 ..< 400).map { line in
                line % 3 == 0 ? "let value\(line) = \(line * 2)" : "    body(\(index), \(line))"
            }.joined(separator: "\n")
            return (old, new)
        }

        // BEFORE: every frame recomputes every diff (unique content per
        // frame forces a cache miss = the old uncached path's cost).
        let before = timed {
            for frame in 0 ..< Self.frames {
                for (old, new) in calls {
                    _ = ToolDiffCache.diff(
                        oldText: old + "\n// frame \(frame)",
                        newText: new + "\n// frame \(frame)",
                        path: "f\(frame).swift",
                    )
                }
            }
        }

        // AFTER: one compute per call; the remaining frames are cache hits.
        let after = timed {
            for _ in 0 ..< Self.frames {
                for (old, new) in calls {
                    _ = ToolDiffCache.diff(oldText: old, newText: new, path: "f.swift")
                }
            }
        }

        report("diff-computation(8 calls x \(Self.frames) frames)", before: before, after: after)
        XCTAssertLessThan(after, before)
    }

    // MARK: 2. Transcript derived data — per-frame 3-pass walk vs build-once

    /// The old ChatView derived toolResults + visibleMessages (+ per-row
    /// assistantItems with the full tool render model) + actionMessageIds
    /// on EVERY body evaluation; a usage-token tick re-ran all of it over
    /// the entire history. The new SessionStore derives everything once
    /// per messages/state change into TranscriptModel.
    func testBenchTranscriptDerivedData() {
        let messages = Self.syntheticHistory()

        // BEFORE: the old computed-property trio, per frame. (This even
        // benefits from the new diff cache — the true old code was
        // slower; see testBenchDiffComputation.)
        let before = timed {
            for _ in 0 ..< Self.frames {
                _ = Self.legacyFrame(messages: messages)
            }
        }

        // AFTER: one build covers the whole second of frames.
        let after = timed {
            let model = TranscriptModel.build(messages: messages, midTurn: true)
            _ = model.rows.count
        }

        report("transcript-derived-data(120 msgs x \(Self.frames) frames)", before: before, after: after)
        XCTAssertLessThan(after, before)
    }

    /// Faithful re-implementation of the removed per-frame derivation:
    /// ChatView.toolResults + visibleMessages (+ ToolBlock model init)
    /// + actionMessageIds.
    private static func legacyFrame(messages: [Message]) -> Int {
        var toolResults: [String: ContentPart.ToolResult] = [:]
        for message in messages where message.role == .assistant {
            for part in message.parts {
                if case let .toolResult(result) = part {
                    toolResults[result.callId] = result
                }
            }
        }
        var rendered = 0
        for message in messages where message.role == .assistant {
            for part in message.parts {
                switch part {
                case let .toolCall(call):
                    let model = ToolRenderModel(
                        item: ToolBlockItem(call: call, result: toolResults[call.id]),
                    )
                    rendered += model.fullOutput?.count ?? 0
                    rendered += model.diff?.count ?? 0
                case let .text(text):
                    rendered += text.count
                default:
                    break
                }
            }
        }
        var candidate: String?
        var actions = 0
        for message in messages {
            switch message.role {
            case .user:
                if candidate != nil {
                    actions += 1
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
        return rendered + actions
    }

    private static func syntheticHistory() -> [Message] {
        let prose = String(repeating: "The quick brown fox jumps over the lazy dog. ", count: 6)
        var messages: [Message] = []
        for index in 0 ..< 40 {
            messages.append(Message(
                id: "u\(index)", role: .user, status: .final,
                parts: [.text("question \(index)")], createdAt: nil,
            ))
            var parts: [ContentPart] = [
                .text(prose),
                .toolCall(ContentPart.ToolCall(
                    id: "c\(index)", name: "run_cmd",
                    arguments: .object(["command": .string("make test")]),
                )),
            ]
            messages.append(Message(
                id: "a\(index)", role: .assistant, status: .final,
                parts: parts, createdAt: nil,
            ))
            parts = [
                .toolResult(ContentPart.ToolResult(
                    callId: "c\(index)", status: "success",
                    content: [.text(String(repeating: "ok\n", count: 40))],
                    error: nil, startedAt: nil, finishedAt: nil,
                )),
                .text("done \(index)"),
            ]
            messages.append(Message(
                id: "r\(index)", role: .assistant, status: .final,
                parts: parts, createdAt: nil,
            ))
        }
        return messages
    }

    // MARK: 3. Inline image decode — per-frame decode vs memoized NSImage

    /// Every transcript re-evaluation used to base64-decode and re-create
    /// every visible inline image; the decode is now memoized.
    func testBenchInlineImageDecode() {
        let base64 = Self.samplePNGBase64()

        // BEFORE: decode on every frame (old InlineImageView).
        let before = timed {
            for _ in 0 ..< Self.frames {
                if let data = Data(base64Encoded: base64) {
                    _ = NSImage(data: data)
                }
            }
        }

        // AFTER: first frame decodes, the rest hit InlineImageCache.
        let after = timed {
            for _ in 0 ..< Self.frames {
                _ = InlineImageCache.image(base64: base64)
            }
        }

        report("inline-image-decode(1 image x \(Self.frames) frames)", before: before, after: after)
        XCTAssertLessThan(after, before)
    }

    private static func samplePNGBase64() -> String {
        guard let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: 1200, pixelsHigh: 800,
            bitsPerSample: 8, samplesPerPixel: 4,
            hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0, bitsPerPixel: 0,
        ), let png = rep.representation(using: .png, properties: [:]) else {
            return ""
        }
        return png.base64EncodedString()
    }

    // MARK: 4. First open — cold markdown parse over a long history

    /// Opening a long session renders every history message with cold
    /// caches: MarkdownText.blocks per assistant text plus
    /// renderInlineMarkdown per prose block. Unique sources force cache
    /// misses — the cold path a real first open pays before SwiftUI
    /// layout even starts.
    func testBenchFirstOpenColdMarkdown() {
        let sources = Self.longSessionAssistantTexts(turns: 200)
        measure("first-open-cold-markdown(\(sources.count) msgs)") {
            for source in sources {
                for block in MarkdownText.blocks(source) {
                    if case let .prose(text) = block {
                        _ = renderInlineMarkdown(text)
                    }
                }
            }
        }
    }

    /// One assistant turn's worth of markdown: prose with inline
    /// code/bold, a fenced code block, and a pipe table. Unique per
    /// turn so the shared block/inline caches always miss (cold open).
    private static func longSessionAssistantTexts(turns: Int) -> [String] {
        (0 ..< turns).map { index in
            """
            Turn \(index) summary: the change touches **\(3 + index % 5) files** and keeps the \
            `build_transcript` path intact for turn \(index).

            - first point for turn \(index), with `inline_code(\(index))` and \
              [a link](https://example.com/\(index))
            - second point explaining why turn \(index) matters

            ```swift
            func handler\(index)(input: Int) -> String {
                let value = input * \(index)
                return "\\(value)"
            }
            ```

            | column | value |
            | --- | --- |
            | turn | \(index) |
            | files | \(3 + index % 5) |

            Closing paragraph for turn \(index), wrapping up the reasoning in a \
            couple of sentences so the prose block has realistic length and \
            inline `code_spans` to attribute.
            """
        }
    }

    // MARK: 5. First open — cold hljs highlighting over history code blocks

    /// CodeBlockView calls SyntaxHighlighter.attributed synchronously in
    /// body; the cache only dedupes REPEATED evaluations. A first open
    /// of a session with N code blocks runs the JS highlighter N times
    /// on the main thread. This measures that cold cost with the real
    /// highlight.js bundle (runfiles) in a fresh JSContext — the same
    /// engine and call pattern as HighlightBridge.
    func testBenchFirstOpenColdHighlight() throws {
        guard let context = Self.makeHighlightContext() else {
            throw XCTSkip("highlight.min.js not found in runfiles")
        }
        let snippets = Self.historyCodeSnippets(count: 100)
        measure("first-open-cold-hljs(\(snippets.count) code blocks)") {
            for (language, code) in snippets {
                _ = Self.highlight(code, language: language, in: context)
            }
        }
    }

    /// Locates the @highlight_js runfile and evaluates it in a fresh
    /// context. Bazel 8 lays external repos out as TOP-LEVEL runfiles
    /// dirs under their canonical names (e.g. +_repo_rules+highlight_js),
    /// so match by suffix directly under TEST_SRCDIR.
    private static func makeHighlightContext() -> JSContext? {
        guard let srcdir = ProcessInfo.processInfo.environment["TEST_SRCDIR"],
              let repos = try? FileManager.default.contentsOfDirectory(atPath: srcdir)
        else { return nil }
        for repo in repos where repo.hasSuffix("highlight_js") {
            let path = ((srcdir as NSString).appendingPathComponent(repo) as NSString)
                .appendingPathComponent("file/highlight.min.js")
            if let source = try? String(contentsOfFile: path, encoding: .utf8),
               let context = JSContext()
            {
                context.evaluateScript(source)
                return context
            }
        }
        return nil
    }

    /// Same call shape as HighlightBridge.highlight.
    private static func highlight(_ code: String, language: String, in context: JSContext) -> String? {
        guard let hljs = context.objectForKeyedSubscript("hljs"),
              hljs.invokeMethod("getLanguage", withArguments: [language])?.isUndefined == false,
              let result = hljs.invokeMethod(
                  "highlight",
                  withArguments: [code, ["language": language, "ignoreIllegals": true]],
              )
        else { return nil }
        return result.objectForKeyedSubscript("value")?.toString()
    }

    /// Representative history code blocks: 30–60 line snippets across
    /// the languages a coding session actually produces.
    private static func historyCodeSnippets(count: Int) -> [(language: String, code: String)] {
        let templates: [(String, String)] = [
            ("swift", """
            struct Row\\(i): Identifiable, Equatable {
                let id: String
                let items: [Item\\(i)]
                var showActions: Bool { !items.isEmpty }
                func rendered(index: Int) -> String {
                    items.map { "\\($0)-\\(index)" }.joined(separator: ", ")
                }
            }
            """),
            ("python", """
            def transform_\\(i)(records):
                result = []
                for record in records:
                    if record.get("kind") == "metric\\(i)":
                        result.append(record["value"] * \\(i))
                return sorted(result, reverse=True)
            """),
            ("go", """
            func handle\\(i)(ctx context.Context, req *Request) (*Response, error) {
                if err := req.Validate(); err != nil {
                    return nil, fmt.Errorf("request %d: %w", \\(i), err)
                }
                return &Response{ID: req.ID, Seq: \\(i)}, nil
            }
            """),
            ("bash", """
            #!/usr/bin/env bash
            set -euo pipefail
            for f in logs/run-\\(i)-*.log; do
                grep -E "ERROR|WARN" "$f" | tail -n \\(i)
            done
            """),
        ]
        return (0 ..< count).map { index in
            let (language, template) = templates[index % templates.count]
            var code = template.replacingOccurrences(of: "\\(i)", with: "\(index)")
            // Pad to a realistic 30–60 lines: history blocks are rarely tiny.
            while code.components(separatedBy: "\n").count < 30 + index % 30 {
                code += "\n// padding line \(index) — keeps the block at history scale"
            }
            return (language, code)
        }
    }

    // MARK: 6. Streaming — tail re-parse cost over a whole turn

    /// LiveBlockCache seals prefixes only at fence-balanced blank-line
    /// boundaries; prose spanning blank lines is re-split AND
    /// re-inline-rendered whole on every 40ms flush (BEFORE).
    /// LiveInlineCache parses each paragraph once when it seals and
    /// re-parses only the growing tail per frame (AFTER).
    /// Measures cumulative and worst-frame cost across a simulated
    /// stream: pure prose (the pathological case) vs prose + code
    /// fences (block-level prefix sealing already worked there).
    func testBenchStreamingTailReparse() {
        struct Stats {
            var splitMs = 0.0
            var renderMs = 0.0
            var maxFrameMs = 0.0
            var frames = 0
        }

        func simulate(finalSize: Int, fenced: Bool, liveSplit: Bool) -> Stats {
            let cache = LiveBlockCache()
            let inlineCache = LiveInlineCache()
            var source = ""
            var stats = Stats()
            for chunk in Self.streamChunks(finalSize: finalSize, fenced: fenced) {
                source += chunk
                let t0 = ContinuousClock.now
                let blocks = cache.blocks(for: source)
                let t1 = ContinuousClock.now
                if case let .prose(text)? = blocks.last {
                    _ = liveSplit ? inlineCache.render(text) : renderInlineMarkdown(text)
                }
                let t2 = ContinuousClock.now
                let split = Self.milliseconds(t0, t1)
                let render = Self.milliseconds(t1, t2)
                stats.splitMs += split
                stats.renderMs += render
                stats.maxFrameMs = max(stats.maxFrameMs, split + render)
                stats.frames += 1
            }
            return stats
        }

        var proseOnly: (before: Stats, after: Stats)!
        for (label, fenced) in [("prose-only", false), ("prose+fences", true)] {
            // AFTER first: its stable-prefix cache entries must not
            // advantage the BEFORE run (unique whole-block keys anyway).
            let after = simulate(finalSize: 24000, fenced: fenced, liveSplit: true)
            let before = simulate(finalSize: 24000, fenced: fenced, liveSplit: false)
            for (tag, stats) in [("before", before), ("after", after)] {
                print(String(
                    format: "LOOM-BENCH %-46@ total=%8.2fms split=%7.2fms render=%8.2fms max-frame=%6.2fms frames=%d",
                    "streaming-tail-reparse(\(label), \(tag))" as NSString,
                    stats.splitMs + stats.renderMs, stats.splitMs, stats.renderMs,
                    stats.maxFrameMs, stats.frames,
                ))
            }
            if !fenced {
                proseOnly = (before, after)
            }
        }
        XCTAssertLessThan(proseOnly.after.renderMs, proseOnly.before.renderMs)
        XCTAssertLessThan(proseOnly.after.maxFrameMs, 16.6, "worst frame must fit the 60fps budget")
    }

    /// A complete stream text (~finalSize bytes), then cut into 16-char
    /// deltas — a typical 40ms token batch.
    private static func streamChunks(finalSize: Int, fenced: Bool) -> [String] {
        var text = ""
        var block = 0
        while text.utf8.count < finalSize {
            if fenced, block % 6 == 5 {
                text += "```swift\nlet value\(block) = compute(\(block))\nprint(value\(block))\n```\n\n"
            } else {
                text += "Sentence \(block) of the streaming reply, with `code\(block)` and **bold** text. "
                if block % 4 == 3 {
                    text += "\n\n"
                }
            }
            block += 1
        }
        var chunks: [String] = []
        var rest = Substring(text)
        while !rest.isEmpty {
            let end = rest.index(rest.startIndex, offsetBy: min(16, rest.count))
            chunks.append(String(rest[..<end]))
            rest = rest[end...]
        }
        return chunks
    }

    // MARK: 7. Long session — full rebuild per turn + per-frame row diff

    /// Two long-history costs the current design still pays:
    ///  - rebuildTranscript: TranscriptModel.build re-walks ALL messages
    ///    on every turn boundary / snapshot reconcile (O(history));
    ///  - per-frame ForEach diff: while streaming (~25fps), SwiftUI
    ///    compares every row's Equatable against its previous value —
    ///    O(rows) deep comparisons per frame even though bodies skip.
    func testBenchLargeHistoryCosts() {
        let messages = Self.largeHistory(turns: 1000) // 3 msgs/turn = 3000 messages
        var model: TranscriptModel!
        measure("large-history-build(3000 msgs)") {
            model = TranscriptModel.build(messages: messages, midTurn: true)
        }

        let rows = model.rows
        var matches = 0
        measure("per-frame-row-diff(\(rows.count) rows x \(Self.frames) frames)") {
            for _ in 0 ..< Self.frames {
                for row in rows where row == row {
                    matches += 1
                }
            }
        }
        XCTAssertEqual(matches, rows.count * Self.frames)
    }

    /// syntheticHistory scaled up: user prompt + assistant text/tool
    /// call + assistant tool result per turn.
    private static func largeHistory(turns: Int) -> [Message] {
        let prose = String(repeating: "The quick brown fox jumps over the lazy dog. ", count: 6)
        var messages: [Message] = []
        messages.reserveCapacity(turns * 3)
        for index in 0 ..< turns {
            messages.append(Message(
                id: "u\(index)", role: .user, status: .final,
                parts: [.text("question \(index)")], createdAt: nil,
            ))
            messages.append(Message(
                id: "a\(index)", role: .assistant, status: .final,
                parts: [
                    .text(prose),
                    .toolCall(ContentPart.ToolCall(
                        id: "c\(index)", name: "run_cmd",
                        arguments: .object(["command": .string("make test")]),
                    )),
                ],
                createdAt: nil,
            ))
            messages.append(Message(
                id: "r\(index)", role: .assistant, status: .final,
                parts: [
                    .toolResult(ContentPart.ToolResult(
                        callId: "c\(index)", status: "success",
                        content: [.text(String(repeating: "ok\n", count: 40))],
                        error: nil, startedAt: nil, finishedAt: nil,
                    )),
                    .text("done \(index)"),
                ],
                createdAt: nil,
            ))
        }
        return messages
    }

    // MARK: 8. Streaming — live reasoning excerpt per frame

    /// ReasoningBlock derives its collapsed summary/tail line on every
    /// streaming frame. The old excerpt split the ENTIRE reasoning
    /// text and (for the tail) copied a reversed array — O(text) with
    /// large allocations, twice per frame. reasoningExcerpt scans from
    /// the relevant end and stops at the first non-empty line.
    func testBenchLiveReasoningExcerpt() {
        var beforeMs = 0.0
        var afterMs = 0.0
        var beforeMax = 0.0
        var afterMax = 0.0
        var frames = 0
        var text = ""
        for chunk in Self.streamChunks(finalSize: 24000, fenced: false) {
            text += chunk
            let t0 = ContinuousClock.now
            let oldSummary = Self.legacyExcerpt(text, fromEnd: false)
            let oldTail = Self.legacyExcerpt(text, fromEnd: true)
            let t1 = ContinuousClock.now
            let newSummary = reasoningExcerpt(text, fromEnd: false)
            let newTail = reasoningExcerpt(text, fromEnd: true)
            let t2 = ContinuousClock.now
            // Equivalence at every frame, not just at the end.
            XCTAssertEqual(newSummary, oldSummary)
            XCTAssertEqual(newTail, oldTail)
            let before = Self.milliseconds(t0, t1)
            let after = Self.milliseconds(t1, t2)
            beforeMs += before
            afterMs += after
            beforeMax = max(beforeMax, before)
            afterMax = max(afterMax, after)
            frames += 1
        }
        print(String(
            format: "LOOM-BENCH %-46@ total=%8.2fms max-frame=%6.2fms frames=%d",
            "reasoning-excerpt(24KB stream, before)" as NSString, beforeMs, beforeMax, frames,
        ))
        print(String(
            format: "LOOM-BENCH %-46@ total=%8.2fms max-frame=%6.2fms frames=%d",
            "reasoning-excerpt(24KB stream, after)" as NSString, afterMs, afterMax, frames,
        ))
        XCTAssertLessThan(afterMs, beforeMs)
    }

    /// Faithful re-implementation of the removed ReasoningBlock.excerpt
    /// (full split + reversed copy).
    private static func legacyExcerpt(_ text: String, fromEnd: Bool) -> String? {
        let lines = text.components(separatedBy: "\n")
        let ordered = fromEnd ? Array(lines.reversed()) : lines
        for raw in ordered {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if !line.isEmpty {
                return line.count > 96 ? String(line.prefix(96)) + "…" : line
            }
        }
        return nil
    }

    // MARK: 9. Streaming — live tool render-model rebuild per frame

    /// DraftView constructs ToolBlock(live:) on every streaming frame,
    /// and its init rebuilt ToolRenderModel(live:) — including a
    /// JSONSerialization of run_cmd previews — for EVERY live tool,
    /// though a tool's state only changes on tool lifecycle events.
    /// LiveToolModelCache rebuilds only on state change.
    func testBenchLiveToolModelRebuild() {
        let states = (0 ..< 10).map { Self.liveRunCmdState(index: $0) }
        let frames = 600

        let before = timed {
            for _ in 0 ..< frames {
                for state in states {
                    _ = ToolRenderModel(live: state)
                }
            }
        }
        let caches = states.map { _ in LiveToolModelCache() }
        let after = timed {
            for _ in 0 ..< frames {
                for (index, state) in states.enumerated() {
                    _ = caches[index].model(live: state)
                }
            }
        }
        // The memo returns the same model a rebuild would produce.
        for (index, state) in states.enumerated() {
            XCTAssertEqual(caches[index].model(live: state), ToolRenderModel(live: state))
        }
        report("live-tool-model-rebuild(10 tools x \(frames) frames)", before: before, after: after)
        XCTAssertLessThan(after, before)
    }

    private static func liveRunCmdState(index: Int) -> ToolCallState {
        var state = ToolCallState(id: "call-\(index)", name: "run_cmd")
        state.status = .running
        state.target = "make test-\(index)"
        state.preview = """
        {"stdout":"\(String(repeating: "ok \(index)\\n", count: 40))","stderr":"","exit_code":0,"timed_out":false,"cancelled":false}
        """
        return state
    }

    // MARK: 7. Real-session cold open (env-gated)

    /// End-to-end cold-open cost over a REAL session snapshot: point
    /// LOOM_SNAPSHOT_BENCH at a `GET /v1/sessions/{id}/snapshot` response
    /// body and this measures each first-open stage over the real
    /// content — JSON decode, transcript build (tool render models,
    /// LCS diffs), cold markdown parse, cold hljs highlighting. Skipped
    /// without the env var: no real session data is ever committed.
    func testBenchRealSnapshotColdOpen() throws {
        guard let path = ProcessInfo.processInfo.environment["LOOM_SNAPSHOT_BENCH"],
              let data = FileManager.default.contents(atPath: path)
        else { throw XCTSkip("LOOM_SNAPSHOT_BENCH not set") }

        let decodeMs = timed {
            Self.snapshotBox = try? LoomJSON.decoder.decode(Snapshot.self, from: data)
        }
        let snapshot = try XCTUnwrap(Self.snapshotBox, "snapshot decode failed")
        let messages = snapshot.messages ?? []

        var modelBox: TranscriptModel?
        let buildMs = timed {
            modelBox = TranscriptModel.build(
                messages: messages, midTurn: false, turnSummaries: snapshot.turnSummaries ?? [],
            )
        }

        // Cold markdown: every assistant text part, whole-block split +
        // inline render per prose block (the first-open render path).
        let texts = messages.flatMap { message in
            message.parts.compactMap { part -> String? in
                guard case let .text(text) = part, message.role == .assistant else { return nil }
                return text
            }
        }
        var codeBlocks: [(language: String, code: String)] = []
        let markdownMs = timed {
            for text in texts {
                for block in MarkdownText.blocks(text) {
                    switch block {
                    case let .prose(prose): _ = renderInlineMarkdown(prose)
                    case let .code(language, code): codeBlocks.append((language ?? "plaintext", code))
                    case .table: break
                    }
                }
            }
        }

        var hljsMs = 0.0
        if let context = Self.makeHighlightContext() {
            hljsMs = timed {
                for (language, code) in codeBlocks {
                    _ = Self.highlight(code, language: language, in: context)
                }
            }
        }

        print(
            "LOOM-BENCH real-snapshot: \(messages.count) msgs, \(texts.count) prose parts, "
                + "\(codeBlocks.count) code blocks, \(modelBox?.rows.count ?? 0) rows, json \(data.count) bytes",
        )
        print(String(
            format: "LOOM-BENCH stages: decode=%.1fms build=%.1fms markdown=%.1fms hljs=%.1fms total=%.1fms",
            decodeMs, buildMs, markdownMs, hljsMs, decodeMs + buildMs + markdownMs + hljsMs,
        ))
    }

    /// Decode landing pad: keeps the try out of the timed closure.
    private nonisolated(unsafe) static var snapshotBox: Snapshot?

    // MARK: 8. Real-session first layout (env-gated)

    /// The cold-open stages above are fast (~72ms for a 283-message
    /// session); the remaining first-open cost is SwiftUI materializing
    /// the non-lazy transcript VStack (exact heights are required for
    /// bottom anchoring — see ChatView). This hosts the real rows
    /// offscreen and times the first layout, alongside tail-window
    /// variants that estimate a progressive-rendering first paint.
    @MainActor
    func testBenchRealSnapshotFirstLayout() throws {
        guard let path = ProcessInfo.processInfo.environment["LOOM_SNAPSHOT_BENCH"],
              let data = FileManager.default.contents(atPath: path),
              let snapshot = try? LoomJSON.decoder.decode(Snapshot.self, from: data)
        else { throw XCTSkip("LOOM_SNAPSHOT_BENCH not set") }
        let model = TranscriptModel.build(
            messages: snapshot.messages ?? [], midTurn: false,
            turnSummaries: snapshot.turnSummaries ?? [],
        )
        // The app warms the highlight JSContext at launch (RootView.task),
        // so layout timings should not include bundle evaluation.
        _ = SyntaxHighlighter.attributed(" ", language: "swift")

        func firstLayoutMs(_ rows: [TranscriptModel.Row]) -> Double {
            timed {
                let hosting = NSHostingView(rootView: VStack(alignment: .leading, spacing: 20) {
                    ForEach(rows) { row in
                        switch row {
                        case let .message(rowModel): MessageRow(row: rowModel)
                        case .turnSummary: EmptyView()
                        }
                    }
                })
                hosting.frame = NSRect(x: 0, y: 0, width: 800, height: 100_000)
                hosting.layoutSubtreeIfNeeded()
                _ = hosting.fittingSize
            }
        }

        let all = firstLayoutMs(model.rows)
        let tail60 = firstLayoutMs(Array(model.rows.suffix(60)))
        let tail30 = firstLayoutMs(Array(model.rows.suffix(30)))
        print(String(
            format: "LOOM-BENCH first-layout: rows=%d all=%.0fms tail60=%.0fms tail30=%.0fms",
            model.rows.count, all, tail60, tail30,
        ))
    }

    // MARK: Reporting

    private static func milliseconds(_ from: ContinuousClock.Instant, _ to: ContinuousClock.Instant) -> Double {
        let elapsed = to - from
        return Double(elapsed.components.seconds) * 1000
            + Double(elapsed.components.attoseconds) / 1e15
    }

    private func measure(_ name: String, _ body: () -> Void) {
        print(String(format: "LOOM-BENCH %-46@ value=%9.2fms", name as NSString, timed(body)))
    }

    private func report(_ name: String, before: Double, after: Double) {
        let speedup = after > 0 ? before / after : .infinity
        print(String(
            format: "LOOM-BENCH %-46@ before=%9.2fms after=%9.3fms speedup=%6.1fx",
            name as NSString, before, after, speedup,
        ))
    }
}
