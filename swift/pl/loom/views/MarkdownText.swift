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
        return cache
    }()

    static let inline: NSCache<NSString, NSAttributedString> = {
        let cache = NSCache<NSString, NSAttributedString>()
        cache.countLimit = 800
        return cache
    }()
}

struct MarkdownText: View {
    let source: String

    enum Block: Equatable {
        case prose(String)
        case code(language: String?, code: String)
        /// GFM pipe table: header row + body rows (the dashed separator
        /// row is consumed by the parser).
        case table(header: [String], rows: [[String]])
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ForEach(Array(Self.blocks(source).enumerated()), id: \.offset) { _, block in
                switch block {
                case let .prose(markdown):
                    ProseText(markdown: markdown)
                case let .code(language, code):
                    CodeBlockView(language: language, code: code)
                case let .table(header, rows):
                    MarkdownTableView(header: header, rows: rows)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
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
        MarkdownCache.blocks.setObject(MarkdownCache.BlocksBox(blocks), forKey: key)
        return blocks
    }

    private static func splitBlocks(_ source: String) -> [Block] {
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

    var body: some View {
        Text(renderInlineMarkdown(markdown))
            .font(.system(size: Theme.textLg))
            .lineSpacing(7) // ≈ the WebUI's 1.7 line-height
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Inline markdown → AttributedString with the WebUI's .md styling:
/// orange mono chips for inline code, headings squashed to 15/600.
/// Shared by prose paragraphs and table cells.
func renderInlineMarkdown(_ source: String) -> AttributedString {
    let key = source as NSString
    if let cached = MarkdownCache.inline.object(forKey: key) {
        return AttributedString(cached)
    }
    let rendered = parseInlineMarkdown(source)
    MarkdownCache.inline.setObject(NSAttributedString(rendered), forKey: key)
    return rendered
}

private func parseInlineMarkdown(_ source: String) -> AttributedString {
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
        Text(renderInlineMarkdown(text))
            .font(.system(size: Theme.textMd, weight: isHeader ? .semibold : .regular))
            .foregroundStyle(Theme.fg)
            .lineSpacing(2)
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

    @State private var copied = false
    @State private var hovering = false

    /// hljs-highlighted code (same engine and code.css theme as the
    /// WebUI); plain mono text when the language is unknown.
    private var highlightedCode: AttributedString {
        SyntaxHighlighter.attributed(code.isEmpty ? " " : code, language: language)
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Text(highlightedCode)
                .lineSpacing(2)
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
