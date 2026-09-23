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
import SwiftUI

/// Block-split markdown rendering, styled after the WebUI's `.md` rules
/// (blocks.css): prose at 14.5/1.7, headings squashed to 15/600, inline
/// code in orange on --bg2 chips, fenced blocks as bg1+border panels.
/// VStack transcripts re-evaluate every row's body on each streaming
/// frame; markdown parsing is the hot path, so both block splitting
/// and inline attribute rendering are memoized by source string (the
/// same memoization strategy as SyntaxHighlighter).
private enum MarkdownCache {
    final class BlocksBox: NSObject {
        let value: [MarkdownText.Block]

        init(_ value: [MarkdownText.Block]) {
            self.value = value
        }
    }

    static let blocks: NSCache<NSString, BlocksBox> = {
        let cache = NSCache<NSString, BlocksBox>()
        cache.countLimit = 400
        cache.totalCostLimit = 32 * 1024 * 1024
        return cache
    }()

    static let inline: NSCache<NSString, NSAttributedString> = {
        let cache = NSCache<NSString, NSAttributedString>()
        cache.countLimit = 800
        cache.totalCostLimit = 64 * 1024 * 1024
        return cache
    }()
}

/// Incremental block parser for a live (append-only) streaming source,
/// held per MarkdownText view via @State. Scan only newly completed
/// lines for a fence-balanced blank-line boundary. Reuse split blocks
/// when that boundary really separates code/table blocks; prose spans
/// blank lines and still needs a whole parse to preserve exact text.
/// Live, one-shot full-text keys never reach the shared NSCache.
final class LiveBlockCache {
    private var parsedPrefix = ""
    private var prefixBlocks: [MarkdownText.Block] = []
    private var scannedSource = ""
    private var scannedFences = 0
    private var stableOffset = 0

    func blocks(for source: String) -> [MarkdownText.Block] {
        guard let split = stableBoundary(of: source) else {
            // No fence-balanced paragraph boundary yet (a short text,
            // or one still inside its first block): the whole source
            // is a changing tail — same cost as before, minus the
            // cache pollution.
            return MarkdownText.splitBlocks(source)
        }
        let prefix = source[..<split]
        if prefix != parsedPrefix[...] {
            // The prefix only ever GROWS while live; a mismatch means
            // the source was replaced wholesale, so re-derive rather
            // than trust the stale parse.
            prefixBlocks = MarkdownText.splitBlocks(String(prefix))
            parsedPrefix = String(prefix)
        }
        let tail = String(source[split...])
        if tail.isEmpty {
            return prefixBlocks
        }
        let tailBlocks = MarkdownText.splitBlocks(tail)
        // splitBlocks carries prose across blank lines; only code and
        // table boundaries can safely seal a block for reuse.
        if case .prose? = prefixBlocks.last, case .prose? = tailBlocks.first {
            // A blank line does not seal prose: splitBlocks joins it into
            // one block, preserving the exact number of blank lines. Do
            // not mutate cached prefixBlocks or normalize that separator.
            return MarkdownText.splitBlocks(source)
        }
        return prefixBlocks + tailBlocks
    }

    /// Byte offset just past the last completed, fence-balanced blank
    /// line. An open fence disqualifies boundaries inside it.
    private func stableBoundary(of source: String) -> String.Index? {
        // Validate append-only input, then inspect only lines not already
        // scanned. Keep the incomplete final line for the next frame.
        if !source.hasPrefix(scannedSource) {
            scannedSource = ""
            scannedFences = 0
            stableOffset = 0
            parsedPrefix = ""
            prefixBlocks = []
        }
        let suffix = source.dropFirst(scannedSource.count)
        let scannedBytes = scannedSource.utf8.count
        var completeBytes = 0
        for line in suffix.split(separator: "\n", omittingEmptySubsequences: false).dropLast() {
            let trimmed = line.drop(while: { $0 == " " || $0 == "\t" })
            if trimmed.hasPrefix("```") {
                scannedFences += 1
            }
            completeBytes += line.utf8.count + 1
            if line.allSatisfy({ $0 == " " || $0 == "\t" || $0 == "\r" }),
               scannedFences % 2 == 0
            {
                stableOffset = scannedBytes + completeBytes
            }
        }
        if completeBytes > 0 {
            let end = source.utf8.index(source.startIndex, offsetBy: scannedBytes + completeBytes)
            scannedSource = String(source[..<end])
        }
        guard stableOffset > 0 else { return nil }
        return source.utf8.index(source.startIndex, offsetBy: stableOffset)
    }
}

struct MarkdownText: View {
    let source: String
    /// Live (still-streaming) text: code blocks render unhighlighted —
    /// an unterminated fence grows with every flush, and re-running the
    /// JS highlighter on the whole block each frame would hog the main
    /// thread. Highlighting pops in when the segment seals.
    var live = false
    /// The WebUI's .stream-cursor: a primary→info gradient "▍" riding
    /// the END of the rendered content while the segment streams.
    var streamCursor = false

    /// Per-view append-only scan state. Reuse sealed code/table blocks
    /// where safe; prose spanning blank lines remains a growing block
    /// and is parsed whole, without polluting the shared block cache.
    @State private var liveCache = LiveBlockCache()

    enum Block: Equatable {
        case prose(String)
        case code(language: String?, code: String)
        /// GFM pipe table: header row + body rows (the dashed separator
        /// row is consumed by the parser).
        case table(header: [String], rows: [[String]])

        var isProse: Bool {
            if case .prose = self {
                return true
            }
            return false
        }
    }

    var body: some View {
        let blocks = resolvedBlocks
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { index, block in
                switch block {
                case let .prose(markdown):
                    ProseText(markdown: markdown, cursor: streamCursor && index == blocks.count - 1)
                case let .code(language, code):
                    CodeBlockView(language: language, code: code, deferHighlight: live)
                case let .table(header, rows):
                    MarkdownTableView(header: header, rows: rows)
                }
            }
            // A non-prose tail (a growing code block) can't carry the
            // cursor inline — it takes its own line after the block.
            if streamCursor, let last = blocks.last, !last.isProse {
                StreamCursorGlyph()
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// Sealed sources go through the shared memoization; live ones
    /// parse incrementally (append-only deltas).
    private var resolvedBlocks: [Block] {
        live ? liveCache.blocks(for: source) : Self.blocks(source)
    }

    /// Splits on ``` fences and GFM table blocks. An unterminated fence
    /// — the common case mid-stream — treats the rest of the input as
    /// code, so streaming code blocks render as code from the first
    /// line. AttributedString's inline parser has no table support:
    /// without this, pipe tables flatten into run-on paragraphs.
    static func blocks(_ source: String) -> [Block] {
        let key = source as NSString
        if let cached = MarkdownCache.blocks.object(forKey: key) {
            return cached.value
        }
        let blocks = splitBlocks(source)
        // Keys and parsed blocks both retain source text; NSCache's count
        // alone cannot constrain a history containing very large messages.
        let cost = source.utf8.count + blocks.reduce(0) { total, block in
            switch block {
            case let .prose(text): total + text.utf8.count
            case let .code(language, code): total + (language?.utf8.count ?? 0) + code.utf8.count
            case let .table(header, rows):
                total + (header + rows.flatMap(\.self)).reduce(0) { $0 + $1.utf8.count }
            }
        }
        if cost <= 32 * 1024 * 1024 {
            MarkdownCache.blocks.setObject(MarkdownCache.BlocksBox(blocks), forKey: key, cost: cost)
        }
        return blocks
    }

    static func splitBlocks(_ source: String) -> [Block] {
        var blocks: [Block] = []
        var prose: [String] = []
        var code: [String] = []
        var language: String?
        var inCode = false

        func flushProse() {
            let markdown = prose.joined(separator: "\n")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if !markdown.isEmpty {
                blocks.append(.prose(markdown))
            }
            prose = []
        }

        let lines = source.components(separatedBy: "\n")
        var i = 0
        while i < lines.count {
            let line = lines[i]
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("```") {
                if inCode {
                    blocks.append(.code(language: language, code: code.joined(separator: "\n")))
                    code = []
                    language = nil
                    inCode = false
                } else {
                    flushProse()
                    let lang = String(trimmed.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                    language = lang.isEmpty ? nil : lang
                    inCode = true
                }
                i += 1
                continue
            }
            if inCode {
                code.append(line)
                i += 1
                continue
            }
            // Table start: a pipe row immediately followed by the dashed
            // separator row (| --- | :---: | ---: |).
            if i + 1 < lines.count, isTableRow(line), isSeparatorRow(lines[i + 1]) {
                flushProse()
                let header = splitRow(line)
                i += 2
                var rows: [[String]] = []
                while i < lines.count, isTableRow(lines[i]), !lines[i].trimmingCharacters(in: .whitespaces).isEmpty {
                    rows.append(splitRow(lines[i]))
                    i += 1
                }
                blocks.append(.table(header: header, rows: rows))
                continue
            }
            prose.append(line)
            i += 1
        }
        if inCode {
            blocks.append(.code(language: language, code: code.joined(separator: "\n")))
        } else {
            flushProse()
        }
        return blocks
    }

    /// A line holding at least two pipes (or one leading pipe) — a
    /// candidate table row. Single-pipe prose stays prose.
    private static func isTableRow(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("|") {
            return true
        }
        return trimmed.filter { $0 == "|" }.count >= 2
    }

    /// The GFM separator row: only pipes, dashes, colons and spaces,
    /// with at least one dash (| --- | :--- |).
    private static func isSeparatorRow(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard trimmed.contains("-"), trimmed.contains("|") else { return false }
        return trimmed.allSatisfy { $0 == "|" || $0 == "-" || $0 == ":" || $0 == " " }
    }

    /// Splits a pipe row into trimmed cells, dropping the edge pipes.
    private static func splitRow(_ line: String) -> [String] {
        var row = line.trimmingCharacters(in: .whitespaces)
        if row.hasPrefix("|") {
            row = String(row.dropFirst())
        }
        if row.hasSuffix("|") {
            row = String(row.dropLast())
        }
        return row.components(separatedBy: "|")
            .map { $0.trimmingCharacters(in: .whitespaces) }
    }
}

// MARK: - Prose

private struct ProseText: View {
    let markdown: String
    /// Append the streaming cursor at the end of this paragraph.
    var cursor = false

    var body: some View {
        var text = Text(renderInlineMarkdown(markdown))
        if cursor {
            // The WebUI's cursor also pulses (1.2s); SwiftUI can't
            // animate a concatenated Text run, so the gradient glyph
            // is static — the churning stream itself is the activity
            // signal (the same call the WebUI makes for .reasoning-tail).
            text = text + Text(" ▍")
                .foregroundStyle(Self.cursorGradient)
        }
        return text
            .font(.system(size: Theme.textLg))
            .lineSpacing(7) // ≈ the WebUI's 1.7 line-height
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// .stream-cursor: linear-gradient(180deg, primary, info).
    static let cursorGradient = LinearGradient(
        colors: [Theme.primary, Theme.info],
        startPoint: .top,
        endPoint: .bottom,
    )
}

/// The standalone stream cursor for non-prose tails: same gradient
/// glyph, WITH the WebUI's 1.2s breathing (a View here can animate,
/// unlike a concatenated Text run).
private struct StreamCursorGlyph: View {
    @State private var dim = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Text("▍")
            .font(.system(size: Theme.textLg))
            .foregroundStyle(ProseText.cursorGradient)
            .opacity(dim && !reduceMotion ? 0.4 : 1)
            .frame(maxWidth: .infinity, alignment: .leading)
            .onAppear {
                guard !reduceMotion else { return }
                withAnimation(.easeInOut(duration: 1.2).repeatForever(autoreverses: true)) {
                    dim = true
                }
            }
    }
}

/// Inline markdown → AttributedString with the WebUI's .md styling:
/// orange mono chips for inline code, headings squashed to 15/600.
/// Shared by prose paragraphs (size textLg) and table cells (textMd)
/// — pass the surrounding point size so strong-emphasis runs match it.
func renderInlineMarkdown(_ source: String, size: CGFloat = Theme.textLg) -> AttributedString {
    let key = "\(size)|\(source)" as NSString
    if let cached = MarkdownCache.inline.object(forKey: key) {
        return AttributedString(cached)
    }
    let rendered = parseInlineMarkdown(source, size: size)
    let attributed = NSAttributedString(rendered)
    // Include the full key and the attributed characters. Runs and
    // attributes add overhead, so charge a conservative multiple.
    let cost = source.utf8.count + attributed.length * 8
    if cost <= 64 * 1024 * 1024 {
        MarkdownCache.inline.setObject(attributed, forKey: key, cost: cost)
    }
    return rendered
}

private func parseInlineMarkdown(_ source: String, size: CGFloat) -> AttributedString {
    typealias SwiftUIAttrs = AttributeScopes.SwiftUIAttributes

    guard var parsed = try? AttributedString(
        markdown: source,
        options: .init(interpretedSyntax: .full),
    ) else {
        return AttributedString(source)
    }
    for run in parsed.runs {
        // Inline `code`: orange on a --bg2 chip (blocks.css .md code).
        if run.inlinePresentationIntent?.contains(.code) == true {
            parsed[run.range][SwiftUIAttrs.BackgroundColorAttribute.self] = Theme.bg2
            parsed[run.range][SwiftUIAttrs.ForegroundColorAttribute.self] = Theme.highlight
            parsed[run.range][SwiftUIAttrs.FontAttribute.self] = Theme.monoSm
            continue
        }
        // The WebUI flattens all heading levels to 15px/600.
        if let intent = run.presentationIntent,
           (1 ... 6).contains(where: { level in
               intent.components.contains { $0.kind == .header(level: level) }
           })
        {
            parsed[run.range][SwiftUIAttrs.FontAttribute.self] =
                Font.system(size: 15, weight: .semibold)
            continue
        }
        // Strong emphasis renders as semibold, not the parser's
        // default bold: **-heavy assistant prose became a glaring
        // bright wall at weight 700 on the dark surfaces.
        if run.inlinePresentationIntent?.contains(.stronglyEmphasized) == true {
            parsed[run.range][SwiftUIAttrs.FontAttribute.self] =
                Font.system(size: size, weight: .semibold)
        }
    }
    return parsed
}

// MARK: - Table (.md table)

/// GFM pipe table, styled after blocks.css: bordered cells (1px --bg2,
/// 5px 12px padding), the header row on --bg1 at weight 600, and a
/// horizontal scroll for tables wider than the column.
private struct MarkdownTableView: View {
    let header: [String]
    let rows: [[String]]

    private var columnCount: Int {
        max(header.count, rows.map(\.count).max() ?? 0)
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Grid(alignment: .leading, horizontalSpacing: 0, verticalSpacing: 0) {
                GridRow {
                    ForEach(0 ..< columnCount, id: \.self) { col in
                        cell(text: col < header.count ? header[col] : "", isHeader: true)
                    }
                }
                ForEach(rows.indices, id: \.self) { row in
                    GridRow {
                        ForEach(0 ..< columnCount, id: \.self) { col in
                            cell(text: col < rows[row].count ? rows[row][col] : "", isHeader: false)
                        }
                    }
                }
            }
        }
        .padding(.vertical, 2) // .md table margin: 10px 0 (block spacing adds the rest)
    }

    private func cell(text: String, isHeader: Bool) -> some View {
        Text(renderInlineMarkdown(text, size: Theme.textMd))
            .font(.system(size: Theme.textMd, weight: isHeader ? .semibold : .regular))
            .foregroundStyle(Theme.fg)
            .lineSpacing(5.5) // .md table inherits the body's 1.65 line-height at 13px
            .textSelection(.enabled)
            .padding(.horizontal, 12)
            .padding(.vertical, 5)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(isHeader ? Theme.bg1 : Color.clear)
            // Cell borders double up between neighbours, reading as a
            // single 1px line (border-collapse).
            .overlay(Rectangle().strokeBorder(Theme.bg2, lineWidth: 1))
    }
}

// MARK: - Code block (.md pre)

struct CodeBlockView: View {
    let language: String?
    let code: String
    /// Skip hljs (live streaming blocks); renders plain mono instead.
    var deferHighlight = false

    @State private var copied = false
    @State private var hovering = false

    /// hljs-highlighted code (same engine and code.css theme as the
    /// WebUI); plain mono text when the language is unknown or the
    /// block is still streaming.
    private var highlightedCode: AttributedString {
        deferHighlight
            ? SyntaxHighlighter.plain(code.isEmpty ? " " : code)
            : SyntaxHighlighter.attributed(code.isEmpty ? " " : code, language: language)
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Text(highlightedCode)
                .lineSpacing(4.5) // ≈ the WebUI's 1.55 line-height (.md pre)
                .textSelection(.enabled)
                .padding(.horizontal, 14)
                .padding(.vertical, 12)
        }
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
        // WebUI pre has no chrome; copy appears on hover, top-right.
        .overlay(alignment: .topTrailing) {
            if hovering {
                Button(action: copy) {
                    Label(copied ? "Copied" : (language ?? "Copy"),
                          systemImage: copied ? "checkmark" : "doc.on.doc")
                        .font(.system(size: Theme.textXs))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Theme.bg2, in: Capsule())
                }
                .buttonStyle(.plain)
                .foregroundStyle(copied ? Theme.success : Theme.muted)
                .padding(6)
                .transition(.opacity)
            }
        }
        .onHover { hovering = $0 }
    }

    private func copy() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(code, forType: .string)
        copied = true
        Task {
            try? await Task.sleep(for: .seconds(1.5))
            copied = false
        }
    }
}
