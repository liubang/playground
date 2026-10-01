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
/// lines for a fence-balanced blank-line boundary; every block type
/// terminates at a blank line, so the sealed prefix's blocks are
/// reused as-is and only the growing tail re-parses per frame. Live,
/// one-shot full-text keys never reach the shared NSCache.
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
        // Every block terminates at a blank line, so the boundary
        // always seals whole blocks: prefix + tail equals the
        // whole-source parse exactly.
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
        /// ATX heading line (# … ######); the marker is consumed by
        /// the parser, the view supplies the heading style.
        case heading(level: Int, text: String)
        /// Consecutive list lines grouped into one block so the view
        /// controls inter-item spacing.
        case list([ListItem])
        case code(language: String?, code: String)
        /// GFM pipe table: header row + body rows (the dashed separator
        /// row is consumed by the parser).
        case table(header: [String], rows: [[String]])
        /// Thematic break (---, ***, ___): rendered as a full-width
        /// hairline (the WebUI's .md hr). Left in prose, Apple's inline
        /// parser would reduce it to a single ⸻ glyph — NOT a divider.
        case rule

        var isProse: Bool {
            if case .prose = self {
                return true
            }
            return false
        }
    }

    /// One list line. ordinal == nil is a bullet (-, *, +); checkbox
    /// is non-nil for GFM task items ([ ] / [x]); indent counts
    /// 2-space nesting levels.
    struct ListItem: Equatable {
        let ordinal: Int?
        let indent: Int
        let checkbox: Bool?
        let text: String
    }

    var body: some View {
        let blocks = resolvedBlocks
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { index, block in
                switch block {
                case let .prose(markdown):
                    ProseText(markdown: markdown, cursor: streamCursor && index == blocks.count - 1, live: live)
                case let .heading(level, text):
                    HeadingText(level: level, text: text)
                case let .list(items):
                    MarkdownListView(items: items)
                case let .code(language, code):
                    CodeBlockView(language: language, code: code, deferHighlight: live)
                case let .table(header, rows):
                    MarkdownTableView(header: header, rows: rows)
                case .rule:
                    // .md hr: 1px --bg2 full-width line, 14px margins
                    // (block spacing 10 + vertical padding 4 each side).
                    Rectangle()
                        .fill(Theme.bg2)
                        .frame(height: 1)
                        .padding(.vertical, 4)
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
            case let .heading(_, text): total + text.utf8.count
            case let .list(items): total + items.reduce(0) { $0 + $1.text.utf8.count }
            case let .code(language, code): total + (language?.utf8.count ?? 0) + code.utf8.count
            case let .table(header, rows):
                total + (header + rows.flatMap(\.self)).reduce(0) { $0 + $1.utf8.count }
            case .rule:
                total
            }
        }
        if cost <= 32 * 1024 * 1024 {
            MarkdownCache.blocks.setObject(MarkdownCache.BlocksBox(blocks), forKey: key, cost: cost)
        }
        return blocks
    }

    /// Splits on ``` fences, GFM table blocks, thematic breaks and
    /// block-level line structure (headings, list items, blank-line
    /// paragraph breaks). An unterminated fence — the common case
    /// mid-stream — treats the rest of the input as code, so streaming
    /// code blocks render as code from the first line. Three renderer
    /// gaps force this to be real view structure: AttributedString's
    /// inline parser has no table support, SwiftUI's Text ignores
    /// block-level presentationIntent entirely (verified empirically on
    /// macOS 26 — paragraph breaks, headers and lists all collapse into
    /// one run-on paragraph; only inline attributes apply), and a
    /// thematic break reaches Text as the bare "⸻" glyph its intent
    /// carries, not the WebUI's full-width .md hr.
    static func splitBlocks(_ source: String) -> [Block] {
        var blocks: [Block] = []
        var paragraph: [String] = []
        var listItems: [ListItem] = []
        var code: [String] = []
        var language: String?
        var inCode = false

        func flushParagraph() {
            let markdown = paragraph.joined(separator: "\n")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if !markdown.isEmpty {
                blocks.append(.prose(markdown))
            }
            paragraph = []
        }

        func flushList() {
            if !listItems.isEmpty {
                blocks.append(.list(listItems))
                listItems = []
            }
        }

        func flushProse() {
            flushParagraph()
            flushList()
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
            if trimmed.isEmpty {
                flushProse()
            } else if let heading = parseHeading(line) {
                flushProse()
                blocks.append(heading)
            } else if let rule = parseThematicBreak(line),
                      rule.marker != "-" || !rule.solid || paragraph.isEmpty
            {
                // A solid "-" run also reads as a setext heading
                // underline: it only breaks when no paragraph is open
                // (CommonMark). "*"/"_" rules, and spaced-out "- - -"
                // (never a setext underline), interrupt unconditionally.
                flushProse()
                blocks.append(.rule)
            } else if let item = parseListItem(line) {
                flushParagraph()
                listItems.append(item)
            } else {
                flushList()
                paragraph.append(line)
            }
            i += 1
        }
        if inCode {
            blocks.append(.code(language: language, code: code.joined(separator: "\n")))
        } else {
            flushProse()
        }
        return blocks
    }

    /// ATX heading line: up to 3 leading spaces, 1–6 #s, then a space
    /// or end of line (no space → plain prose, e.g. "#hashtag").
    private static func parseHeading(_ line: String) -> Block? {
        var rest = Substring(line)
        var leading = 0
        while rest.first == " ", leading < 3 {
            rest = rest.dropFirst()
            leading += 1
        }
        var level = 0
        while rest.first == "#" {
            rest = rest.dropFirst()
            level += 1
        }
        guard (1 ... 6).contains(level) else { return nil }
        guard rest.isEmpty || rest.first == " " || rest.first == "\t" else { return nil }
        return .heading(level: level, text: rest.trimmingCharacters(in: .whitespaces))
    }

    /// List item line: optional indent (2 spaces per level), a bullet
    /// (-, *, +) or ordered (1. / 1)) marker, then the text with an
    /// optional GFM task checkbox. Lazy continuation lines are NOT
    /// folded into the previous item — they start a paragraph.
    private static func parseListItem(_ line: String) -> ListItem? {
        var rest = Substring(line)
        var spaces = 0
        while rest.first == " " {
            rest = rest.dropFirst()
            spaces += 1
        }
        var ordinal: Int?
        if let first = rest.first, first == "-" || first == "*" || first == "+" {
            let after = rest.dropFirst()
            guard after.first == " " || after.first == "\t" else { return nil }
            rest = after
        } else {
            var digits = 0
            var index = rest.startIndex
            while index < rest.endIndex, rest[index].isNumber, digits < 9 {
                index = rest.index(after: index)
                digits += 1
            }
            guard digits > 0, index < rest.endIndex,
                  rest[index] == "." || rest[index] == ")" else { return nil }
            ordinal = Int(rest[..<index])
            let after = rest[rest.index(after: index)...]
            guard after.first == " " || after.first == "\t" else { return nil }
            rest = after
        }
        var text = rest.trimmingCharacters(in: .whitespaces)
        var checkbox: Bool?
        if text.hasPrefix("[ ]") {
            checkbox = false
            text = String(text.dropFirst(3)).trimmingCharacters(in: .whitespaces)
        } else if text.hasPrefix("[x]") || text.hasPrefix("[X]") {
            checkbox = true
            text = String(text.dropFirst(3)).trimmingCharacters(in: .whitespaces)
        }
        return ListItem(ordinal: ordinal, indent: spaces / 2, checkbox: checkbox, text: text)
    }

    /// Thematic break line (CommonMark): up to 3 leading spaces, then
    /// 3+ of the same -, * or _ marker (spaces/tabs allowed between
    /// them). Returns the marker plus whether the run is solid (no
    /// internal whitespace — only a solid "-" run doubles as a setext
    /// heading underline), or nil for anything else. Whether a solid
    /// "-" break is a rule or a setext underline depends on an open
    /// paragraph, so the caller makes that call.
    private static func parseThematicBreak(_ line: String) -> (marker: Character, solid: Bool)? {
        var rest = Substring(line)
        var leading = 0
        while rest.first == " ", leading < 3 {
            rest = rest.dropFirst()
            leading += 1
        }
        // 4+ leading spaces = indented code, never a rule.
        guard let marker = rest.first, marker != " ", marker != "\t",
              marker == "-" || marker == "*" || marker == "_" else { return nil }
        var count = 0
        var solid = true
        var seenSpace = false
        for char in rest {
            if char == marker {
                if seenSpace {
                    solid = false
                } // marker after a gap
                count += 1
            } else if char == " " || char == "\t" {
                if count > 0 {
                    seenSpace = true
                }
            } else {
                return nil
            }
        }
        return count >= 3 ? (marker, solid) : nil
    }

    /// A line holding at least two pipes (or one leading pipe) — a
    /// candidate table row. Single-pipe prose stays prose. `\|`
    /// (a backslash-escaped pipe) is literal cell content, not a
    /// boundary, so it doesn't count.
    private static func isTableRow(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("|") {
            return true
        }
        return unescapedPipeCount(trimmed) >= 2
    }

    /// Pipes not preceded by an odd-length backslash run: `\|` is an
    /// escaped literal pipe, `\\|` is an escaped backslash then a
    /// boundary pipe.
    private static func unescapedPipeCount(_ line: String) -> Int {
        var count = 0
        var backslashes = 0
        for char in line {
            if char == "\\" {
                backslashes += 1
            } else {
                if char == "|", backslashes % 2 == 0 {
                    count += 1
                }
                backslashes = 0
            }
        }
        return count
    }

    /// The GFM separator row: only pipes, dashes, colons and spaces,
    /// with at least one dash (| --- | :--- |).
    private static func isSeparatorRow(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard trimmed.contains("-"), trimmed.contains("|") else { return false }
        return trimmed.allSatisfy { $0 == "|" || $0 == "-" || $0 == ":" || $0 == " " }
    }

    /// Splits a pipe row into trimmed cells, dropping the edge pipes.
    /// GFM escapes a literal in-cell pipe as `\|` — the only way to
    /// put a pipe inside a code span in a table — so splitting must
    /// skip it AND consume the backslash (the GFM spec strips the
    /// escape even inside code spans, unlike normal inline parsing).
    private static func splitRow(_ line: String) -> [String] {
        var row = line.trimmingCharacters(in: .whitespaces)
        if row.hasPrefix("|") {
            row = String(row.dropFirst())
        }
        var cells: [String] = []
        var cell = ""
        var backslashes = 0
        for char in row {
            if char == "\\" {
                backslashes += 1
                cell.append(char)
                continue
            }
            if char == "|" {
                if backslashes % 2 == 0 {
                    cells.append(cell)
                    cell = ""
                } else {
                    // Odd backslashes: the escape pairs with this pipe —
                    // drop the escaping backslash, keep the literal pipe.
                    cell.removeLast()
                    cell.append(char)
                }
            } else {
                cell.append(char)
            }
            backslashes = 0
        }
        cells.append(cell)
        // A trailing edge pipe leaves a final empty cell; drop it.
        if cells.count > 1, cells.last?.trimmingCharacters(in: .whitespaces).isEmpty == true {
            cells.removeLast()
        }
        return cells.map { $0.trimmingCharacters(in: .whitespaces) }
    }
}

// MARK: - Prose

private struct ProseText: View {
    let markdown: String
    /// Append the streaming cursor at the end of this paragraph.
    var cursor = false
    /// Live (still-streaming) prose: sealed paragraphs render once
    /// ever; only the growing tail re-parses per flush
    /// (LiveInlineCache). Sealed prose renders whole.
    var live = false

    /// Per-view append-only render state (same pattern as
    /// MarkdownText.liveCache).
    @State private var liveInlineCache = LiveInlineCache()

    var body: some View {
        var text = Text(live ? liveInlineCache.render(markdown) : renderInlineMarkdown(markdown))
        if cursor {
            // The WebUI's cursor also pulses (1.2s); SwiftUI can't
            // animate a concatenated Text run, so the gradient glyph
            // is static — the churning stream itself is the activity
            // signal (the same call the WebUI makes for .reasoning-tail).
            text = text + Text(" ▍")
                .foregroundStyle(Self.cursorGradient)
        }
        return text
            // SwiftUI's system text reads heavier than WKWebView's antialiased
            // 400-weight prose at the same point size on dark surfaces.
            .font(.system(size: Theme.textLg, weight: .light))
            .foregroundStyle(Theme.fg)
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

/// Incremental inline renderer for live (append-only) prose, held per
/// ProseText via @State. Split at the LAST blank line: paragraphs
/// before it are sealed — parse each ONCE on the frame it seals and
/// append; only the growing tail paragraph is re-parsed per 40ms
/// flush. Without this a long prose-only stream re-parses the whole
/// block every frame — measured 10.6s cumulative / 16.4ms worst frame
/// over a 24KB stream (PerfBenchmarksTests.testBenchStreamingTailReparse),
/// blowing the 16.6ms frame budget late in the stream. Sealed prose
/// never takes this path — the final render is always the exact
/// whole-block parse, the same tradeoff as deferHighlight for live
/// code blocks.
final class LiveInlineCache {
    private var sealedSource = ""
    private var sealedRendered = AttributedString()
    private var maxIdentity = 0

    func render(_ markdown: String, size: CGFloat = Theme.textLg) -> AttributedString {
        guard let split = stableProsePrefixSplit(markdown) else {
            // No sealed paragraph yet (a single growing paragraph):
            // the whole source is a changing tail — same cost as the
            // old path, minus the cache pollution.
            return parseInlineMarkdown(markdown, size: size)
        }
        let prefix = String(markdown[..<split])
        if prefix != sealedSource {
            guard prefix.hasPrefix(sealedSource) else {
                // Wholesale replacement (new stream): start over.
                sealedSource = ""
                sealedRendered = AttributedString()
                maxIdentity = 0
                return render(markdown, size: size)
            }
            // Only the NEWLY sealed paragraphs parse — never the whole
            // prefix again (a re-parse per seal spiked the worst frame
            // to 30ms).
            let rendered = parseWithIdentityOffset(String(prefix.dropFirst(sealedSource.count)), size: size)
            bumpIdentity(rendered)
            sealedRendered.append(rendered)
            sealedSource = prefix
        }
        let tail = parseWithIdentityOffset(String(markdown[split...]), size: size)
        var result = sealedRendered
        result.append(tail)
        return result
    }

    /// presentationIntent identities restart at 1 in a fresh parse, so
    /// appending a raw parse MERGES its first paragraph with the
    /// previous one (identical attributes → one run, and the markdown
    /// parser strips the "\n\n" separator — the paragraph break is
    /// GONE). Parse behind one dummy paragraph per consumed identity so
    /// the numbering continues, then drop the dummy characters.
    private func parseWithIdentityOffset(_ source: String, size: CGFloat) -> AttributedString {
        guard maxIdentity > 0 else { return parseInlineMarkdown(source, size: size) }
        let padded = parseInlineMarkdown(String(repeating: "x\n\n", count: maxIdentity) + source, size: size)
        // Each dummy paragraph renders as exactly one "x" character
        // (the parser strips the "\n\n" separators); dropping them
        // leaves the source with identities maxIdentity+1….
        let dummyEnd = padded.characters.index(padded.characters.startIndex, offsetBy: maxIdentity)
        return AttributedString(padded[dummyEnd...])
    }

    private func bumpIdentity(_ rendered: AttributedString) {
        for run in rendered.runs {
            guard let intent = run.presentationIntent else { continue }
            for component in intent.components {
                maxIdentity = max(maxIdentity, component.identity)
            }
        }
    }
}

/// Start of the growing tail: just past the newline of the LAST blank
/// (whitespace-only) line, or nil when no blank line precedes any
/// content. Paragraph breaks end every GFM inline construct, so the
/// prefix and tail parse independently to the same characters and
/// attributes as the whole (link-reference definitions are the known
/// exception — live-only, resolved by the sealed whole-parse).
func stableProsePrefixSplit(_ markdown: String) -> String.Index? {
    var split: String.Index?
    var lineStart = markdown.startIndex
    while lineStart < markdown.endIndex {
        let lineEnd = markdown[lineStart...].firstIndex(of: "\n") ?? markdown.endIndex
        if markdown[lineStart ..< lineEnd].allSatisfy({ $0 == " " || $0 == "\t" || $0 == "\r" }),
           lineEnd < markdown.endIndex, markdown.index(after: lineEnd) < markdown.endIndex
        {
            split = markdown.index(after: lineEnd)
        }
        guard lineEnd < markdown.endIndex else { break }
        lineStart = markdown.index(after: lineEnd)
    }
    return split
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

/// Single tildes in numeric ranges (e.g. 6~8°C) are prose, not GFM
/// strikethrough delimiters. Leave escaped text, code spans and ~~ pairs
/// alone so intentional Markdown keeps its original meaning.
func escapeNumericRangeTildes(_ source: String) -> String {
    guard source.contains("~") else { return source }
    let chars = Array(source)
    var result = ""
    result.reserveCapacity(source.utf8.count)
    var codeDelimiter = 0
    var i = 0
    while i < chars.count {
        let char = chars[i]
        if char == "`" {
            var end = i + 1
            while end < chars.count, chars[end] == "`" {
                end += 1
            }
            let count = end - i
            var slashes = 0
            var j = i
            while j > 0, chars[j - 1] == "\\" {
                slashes += 1
                j -= 1
            }
            if slashes.isMultiple(of: 2) {
                if codeDelimiter == count {
                    codeDelimiter = 0
                } else if codeDelimiter == 0 {
                    // An unmatched backtick is literal text, not a code span.
                    var closing = end
                    while closing < chars.count {
                        if chars[closing] != "`" {
                            closing += 1
                            continue
                        }
                        var next = closing + 1
                        while next < chars.count, chars[next] == "`" {
                            next += 1
                        }
                        if next - closing == count {
                            break
                        }
                        closing = next
                    }
                    if closing < chars.count {
                        codeDelimiter = count
                    }
                }
            }
            result.append(contentsOf: chars[i ..< end])
            i = end
            continue
        }
        if char == "~", codeDelimiter == 0, i + 1 < chars.count {
            // Skip backslash escapes to find the actual preceding character.
            var j = i
            while j > 0, chars[j - 1] == "\\" {
                j -= 1
            }
            if j > 0, chars[j - 1].isNumber, chars[i + 1].isNumber,
               j < 2 || chars[j - 2] != "~",
               i + 2 == chars.count || chars[i + 2] != "~",
               (i - j).isMultiple(of: 2)
            {
                result.append("\\")
            }
        }
        result.append(char)
        i += 1
    }
    return result
}

private func parseInlineMarkdown(_ source: String, size: CGFloat) -> AttributedString {
    typealias SwiftUIAttrs = AttributeScopes.SwiftUIAttributes

    guard var parsed = try? AttributedString(
        markdown: escapeNumericRangeTildes(source),
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
        // Preserve a clear step above the lighter native prose without
        // making **-heavy assistant replies look uniformly bold.
        if run.inlinePresentationIntent?.contains(.stronglyEmphasized) == true {
            parsed[run.range][SwiftUIAttrs.FontAttribute.self] =
                Font.system(size: size, weight: .medium)
        }
    }
    return parsed
}

// MARK: - Heading / list blocks

/// Heading block: the WebUI flattens all levels to 15px/600
/// (blocks.css .md h1–h6). Inline code chips inside the heading keep
/// their mono font (explicit run attributes beat the view font).
private struct HeadingText: View {
    let level: Int
    let text: String

    var body: some View {
        Text(renderInlineMarkdown(text))
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(Theme.fg)
            .lineSpacing(5)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 4)
            .accessibilityHeading(level <= 3 ? .h1 : .h3)
    }
}

/// List block: one row per item — bullet / ordinal / task checkbox in
/// a fixed-width marker column, inline-markdown text after it. Rows
/// pack tighter than block spacing (a list reads as one unit).
private struct MarkdownListView: View {
    let items: [MarkdownText.ListItem]

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                HStack(alignment: .firstTextBaseline, spacing: 7) {
                    marker(item)
                    Text(renderInlineMarkdown(item.text))
                        .font(.system(size: Theme.textLg, weight: .light))
                        .foregroundStyle(Theme.fg)
                        .lineSpacing(7)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(.leading, CGFloat(item.indent) * 16)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private func marker(_ item: MarkdownText.ListItem) -> some View {
        if let checked = item.checkbox {
            Image(systemName: checked ? "checkmark.square.fill" : "square")
                .font(.system(size: 12))
                .foregroundStyle(checked ? Theme.success : Theme.muted)
                .frame(width: 16, alignment: .center)
        } else if let ordinal = item.ordinal {
            Text("\(ordinal).")
                .font(.system(size: Theme.textLg, weight: .light))
                .foregroundStyle(Theme.muted)
                .frame(minWidth: 16, alignment: .trailing)
        } else {
            Text("•")
                .font(.system(size: Theme.textLg))
                .foregroundStyle(Theme.muted)
                .frame(width: 16, alignment: .center)
        }
    }
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
            .font(.system(size: Theme.textMd, weight: isHeader ? .semibold : .light))
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
