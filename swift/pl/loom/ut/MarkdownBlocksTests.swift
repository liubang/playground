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

@testable import Loom
import XCTest

/// MarkdownText block splitting: the live incremental parser
/// (LiveBlockCache) must agree with the whole-source parser at EVERY
/// append step of a stream — the transcript renders their output
/// directly, so a divergence would show up as flickering or
/// mis-parsed blocks while tokens stream in.
final class MarkdownBlocksTests: XCTestCase {
    // MARK: Whole-source splitBlocks (baseline behavior)

    func testProseAndCodeFenceSplit() {
        let blocks = MarkdownText.splitBlocks("hello\n\n```swift\nlet a = 1\n```\n\nbye")
        XCTAssertEqual(blocks, [
            .prose("hello"),
            .code(language: "swift", code: "let a = 1"),
            .prose("bye"),
        ])
    }

    func testUnterminatedFenceTreatsRestAsCode() {
        let blocks = MarkdownText.splitBlocks("intro\n\n```py\nprint(1)\nprint(2)")
        XCTAssertEqual(blocks, [
            .prose("intro"),
            .code(language: "py", code: "print(1)\nprint(2)"),
        ])
    }

    func testTableBlock() {
        let blocks = MarkdownText.splitBlocks("| a | b |\n| --- | --- |\n| 1 | 2 |")
        XCTAssertEqual(blocks, [
            .table(header: ["a", "b"], rows: [["1", "2"]]),
        ])
    }

    /// `\|` is a GFM-escaped literal pipe (cell content, not a column
    /// boundary) — the only way to write a pipe inside a code span in
    /// a table. The escape is consumed even inside code spans.
    func testTableEscapedPipe() {
        let blocks = MarkdownText.splitBlocks(
            "| key | value |\n"
                + "| --- | --- |\n"
                + "| pattern | `{x\\|y}:n` |\n"
                + "| a\\|b | c \\\\| d |",
        )
        XCTAssertEqual(blocks, [
            .table(
                header: ["key", "value"],
                // `\\` is an escaped backslash — the pipe after it
                // still separates cells.
                rows: [["pattern", "`{x|y}:n`"], ["a|b", "c \\\\", "d"]],
            ),
        ])
    }

    // MARK: Block-level line structure (headings, lists, paragraphs)

    func testHeadings() {
        XCTAssertEqual(MarkdownText.splitBlocks("# Title\n\n## Sub\nbody"), [
            .heading(level: 1, text: "Title"),
            .heading(level: 2, text: "Sub"),
            .prose("body"),
        ])
        // No space after #, or 4+ leading spaces: plain prose.
        XCTAssertEqual(MarkdownText.splitBlocks("#hashtag"), [.prose("#hashtag")])
        XCTAssertEqual(MarkdownText.splitBlocks("    # indented"), [.prose("# indented")])
        // Up to 3 leading spaces still count; ####### is not a heading.
        XCTAssertEqual(MarkdownText.splitBlocks("   ## ok"), [.heading(level: 2, text: "ok")])
        XCTAssertEqual(MarkdownText.splitBlocks("####### too deep"), [.prose("####### too deep")])
    }

    func testLists() {
        XCTAssertEqual(MarkdownText.splitBlocks("- a\n- b\n\n1. x\n2) y"), [
            .list([
                MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "a"),
                MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "b"),
            ]),
            .list([
                MarkdownText.ListItem(ordinal: 1, indent: 0, checkbox: nil, text: "x"),
                MarkdownText.ListItem(ordinal: 2, indent: 0, checkbox: nil, text: "y"),
            ]),
        ])
        // Task checkboxes and nesting indent.
        XCTAssertEqual(MarkdownText.splitBlocks("- [ ] todo\n  - [x] done"), [
            .list([
                MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: false, text: "todo"),
                MarkdownText.ListItem(ordinal: nil, indent: 1, checkbox: true, text: "done"),
            ]),
        ])
        // A list ends at a non-list line without needing a blank line.
        XCTAssertEqual(MarkdownText.splitBlocks("- a\ntail"), [
            .list([MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "a")]),
            .prose("tail"),
        ])
        // "-" alone is prose; an ordered marker needs a trailing space.
        XCTAssertEqual(MarkdownText.splitBlocks("-"), [.prose("-")])
        XCTAssertEqual(MarkdownText.splitBlocks("1.x"), [.prose("1.x")])
    }

    func testHeadingAndListInterleaveWithProse() {
        let blocks = MarkdownText.splitBlocks("intro **bold**\n## h\n- one\n- two `code`\nafter")
        XCTAssertEqual(blocks, [
            .prose("intro **bold**"),
            .heading(level: 2, text: "h"),
            .list([
                MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "one"),
                MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "two `code`"),
            ]),
            .prose("after"),
        ])
    }

    func testLiveHeadingAndListStream() {
        assertIncrementalMatchesWhole([
            "# Title", "\n\n- one", "\n- two\n\n", "tail",
        ])
        assertIncrementalMatchesWhole([
            "intro\n", "\n", "1. a\n", "2. b\n", "\n", "## h\n", "body",
        ])
    }

    // MARK: Thematic breaks (---, ***, ___)

    func testThematicBreakBetweenParagraphs() {
        XCTAssertEqual(MarkdownText.splitBlocks("前言。\n\n---\n\n**标题**"), [
            .prose("前言。"),
            .rule,
            .prose("**标题**"),
        ])
        XCTAssertEqual(MarkdownText.splitBlocks("\n---\n"), [.rule])
        // Longer runs, other markers, up to 3 leading spaces.
        XCTAssertEqual(MarkdownText.splitBlocks("-----"), [.rule])
        XCTAssertEqual(MarkdownText.splitBlocks("***"), [.rule])
        XCTAssertEqual(MarkdownText.splitBlocks("  ___"), [.rule])
        // NOT breaks: too short, mixed markers, 4-space indent (code).
        XCTAssertEqual(MarkdownText.splitBlocks("--"), [.prose("--")])
        XCTAssertEqual(MarkdownText.splitBlocks("---x"), [.prose("---x")])
        XCTAssertEqual(MarkdownText.splitBlocks("---*"), [.prose("---*")])
        XCTAssertEqual(MarkdownText.splitBlocks("    ---"), [.prose("---")])
    }

    /// A solid "-" run right after a paragraph line is a setext
    /// heading underline, NOT a rule (the inline parse styles it).
    /// "*"/"_" runs and spaced-out "- - -" interrupt unconditionally.
    func testThematicBreakVsSetextUnderline() {
        XCTAssertEqual(MarkdownText.splitBlocks("标题\n---"), [.prose("标题\n---")])
        XCTAssertEqual(MarkdownText.splitBlocks("标题  \n  ---  "), [.prose("标题  \n  ---")])
        XCTAssertEqual(MarkdownText.splitBlocks("a\n***\nb"), [.prose("a"), .rule, .prose("b")])
        XCTAssertEqual(MarkdownText.splitBlocks("a\n- - -\nb"), [.prose("a"), .rule, .prose("b")])
        // A rule can also end a list (no open paragraph).
        XCTAssertEqual(MarkdownText.splitBlocks("- a\n---"), [
            .list([MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "a")]),
            .rule,
        ])
    }

    /// A fence swallows rule-like lines; tables own dashed rows.
    func testThematicBreakDoesNotInvadeCodeOrTables() {
        XCTAssertEqual(MarkdownText.splitBlocks("```\n---\n```"), [
            .code(language: nil, code: "---"),
        ])
        XCTAssertEqual(MarkdownText.splitBlocks("| a |\n| --- |\n| 1 |"), [
            .table(header: ["a"], rows: [["1"]]),
        ])
    }

    func testLiveThematicBreakStream() {
        assertIncrementalMatchesWhole([
            "前言\n", "\n", "-", "-", "-\n", "\n", "标题",
        ])
        assertIncrementalMatchesWhole([
            "a\n", "***", "\nb",
        ])
    }

    // MARK: Blockquotes (> …)

    func testBlockquote() {
        XCTAssertEqual(MarkdownText.splitBlocks("intro\n\n> 注意：这是引用\n> 第二行\n\nafter"), [
            .prose("intro"),
            .quote("注意：这是引用\n第二行"),
            .prose("after"),
        ])
    }

    /// A quote ends at the first non-quote line — no blank line
    /// required (a quote cannot swallow the paragraph after it).
    func testBlockquoteInterruptedByProseAndList() {
        XCTAssertEqual(MarkdownText.splitBlocks("> quoted\nplain"), [
            .quote("quoted"),
            .prose("plain"),
        ])
        XCTAssertEqual(MarkdownText.splitBlocks("> q\n- item"), [
            .quote("q"),
            .list([MarkdownText.ListItem(ordinal: nil, indent: 0, checkbox: nil, text: "item")]),
        ])
    }

    /// Up to 3 leading spaces and a missing space after ">" are
    /// valid; a bare ">" is an empty quote line (a paragraph break
    /// inside the quote). 4+ leading spaces is indented code, and
    /// ">>" keeps the inner marker as literal text.
    func testBlockquoteMarkerVariants() {
        XCTAssertEqual(MarkdownText.splitBlocks("   > spaced\n>nospace\n>\n> after gap"), [
            .quote("spaced\nnospace\n\nafter gap"),
        ])
        XCTAssertEqual(MarkdownText.splitBlocks("> a\n>> b"), [.quote("a\n> b")])
        XCTAssertEqual(MarkdownText.splitBlocks("    > not a quote"), [.prose("> not a quote")])
    }

    func testLiveBlockquoteStream() {
        assertIncrementalMatchesWhole([
            "intro\n", "\n> 引用", "\n> 第二行\n", "\n", "after",
        ])
        assertIncrementalMatchesWhole([
            "> q", "uoted\nplain",
        ])
    }

    // MARK: Inline Markdown numeric ranges

    func testNumericRangesDoNotBecomeStrikethrough() {
        let source = "今天 32.5°C，明天 6~8°C，周五 25~27°C；PM2.5 8~92，风速 14~15 km/h。"
        XCTAssertEqual(String(renderInlineMarkdown(source).characters), source)
        XCTAssertEqual(escapeNumericRangeTildes(source),
                       "今天 32.5°C，明天 6\\~8°C，周五 25\\~27°C；PM2.5 8\\~92，风速 14\\~15 km/h。")
    }

    func testNumericTildesRespectCodeEscapesAndIntentionalStrikethrough() {
        let source = "`6~8` 和 ``25~27``，6\\~8、6\\\\~8，~~已过时~~，6～8，6~8"
        XCTAssertEqual(escapeNumericRangeTildes(source),
                       "`6~8` 和 ``25~27``，6\\~8、6\\\\\\~8，~~已过时~~，6～8，6\\~8")
        XCTAssertEqual(String(renderInlineMarkdown("~~已过时~~ 与 6~8").characters), "已过时 与 6~8")
        XCTAssertEqual(escapeNumericRangeTildes("`未闭合 6~8 与 25~27"),
                       "`未闭合 6\\~8 与 25\\~27")
        XCTAssertEqual(escapeNumericRangeTildes("\\`转义 25~27"), "\\`转义 25\\~27")
    }

    // MARK: LiveBlockCache invariants

    /// Feeds the chunks as an append-only stream; after each append
    /// the incremental parse must equal the whole-source parse.
    private func assertIncrementalMatchesWhole(
        _ chunks: [String],
        file: StaticString = #filePath,
        line: UInt = #line,
    ) {
        let cache = LiveBlockCache()
        var source = ""
        for chunk in chunks {
            source += chunk
            XCTAssertEqual(
                cache.blocks(for: source),
                MarkdownText.splitBlocks(source),
                "incremental parse diverged at source: \(source.debugDescription)",
                file: file,
                line: line,
            )
        }
    }

    func testLiveMultiParagraphStream() {
        assertIncrementalMatchesWhole([
            "First para",
            "graph grows",
            "\n\nSecond ",
            "paragraph\n\nThird",
        ])
    }

    /// A blank line inside an OPEN fence is code, not a paragraph
    /// boundary — the incremental parser must not split there.
    func testLiveCodeFenceWithBlankLineInside() {
        assertIncrementalMatchesWhole([
            "intro\n\n",
            "```swift\nlet a = 1\n",
            "\nlet b = 2\n",
            "```\n\n",
            "after",
        ])
    }

    /// A blank line between a pipe header and a separator prevents a table.
    /// Check both the first blank and another blank arriving in a later frame.
    func testLiveHeaderSeparatedFromSeparatorByBlankLines() {
        assertIncrementalMatchesWhole([
            "| a |\n", "\n", "| --- |",
        ])
        assertIncrementalMatchesWhole([
            "| a |\n", "\n", "\n", "| --- |",
        ])
        // Blank lines are block boundaries now (SwiftUI Text ignores
        // presentationIntent; structure is real view structure).
        XCTAssertEqual(MarkdownText.splitBlocks("| a |\n\n| --- |"), [
            .prose("| a |"),
            .prose("| --- |"),
        ])
        XCTAssertEqual(MarkdownText.splitBlocks("| a |\n\n\n| --- |"), [
            .prose("| a |"),
            .prose("| --- |"),
        ])
        assertIncrementalMatchesWhole([
            "| a |\n", "\n", "| --- |\n", "| 1 |\n", "\n",
            "| b |\n", "| --- |\n", "| 2 |",
        ])
        assertIncrementalMatchesWhole([
            "| a |\n", "\n", "\n", "| --- |\n", "\n",
            "```swift\n", "let x = 1\n", "```\n", "\n", "tail",
        ])
    }

    func testLiveTableThenProse() {
        assertIncrementalMatchesWhole([
            "| a | b |\n| --- | --- |\n",
            "| 1 | 2 |\n\n",
            "tail text",
        ])
    }

    func testLiveUnterminatedFenceGrowsAsCode() {
        assertIncrementalMatchesWhole([
            "text\n\n```py\n",
            "print(1)\n",
            "print(2)",
        ])
    }

    /// No fence-balanced paragraph boundary yet: falls back to the
    /// whole-source parse every frame (previous behavior).
    func testLiveNoBoundaryFallsBackToWholeParse() {
        let cache = LiveBlockCache()
        let source = "a single growing paragraph without breaks"
        XCTAssertEqual(cache.blocks(for: source), MarkdownText.splitBlocks(source))
    }

    /// A source that ends exactly on a boundary must not lose or
    /// duplicate the trailing empty tail.
    func testLiveSourceEndingOnBoundary() {
        assertIncrementalMatchesWhole([
            "only paragraph",
            "\n\n",
        ])
    }

    func testLivePreservesMultipleBlankLinesAndPrefixAcrossFrames() {
        assertIncrementalMatchesWhole([
            "first\n", "\n", "second", "\n\n\n", "third",
            "\n\n```swift\n", "let x = 1\n", "\n", "```\n\n", "last",
        ])
    }

    func testLiveBoundaryAcrossPartialLinesAndReplacement() {
        let cache = LiveBlockCache()
        for source in ["first\n", "first\n ", "first\n \n", "first\n \n```py\n", "first\n \n```py\ncode"] {
            XCTAssertEqual(cache.blocks(for: source), MarkdownText.splitBlocks(source))
        }
        let replacement = "replacement\n\n| a | b |\n| --- | --- |\n| 1 | 2 |"
        XCTAssertEqual(cache.blocks(for: replacement), MarkdownText.splitBlocks(replacement))
    }

    func testLiveUnicodeAndCRLFStream() {
        assertIncrementalMatchesWhole(["你好\r\n", "\r\n", "世界\r\n", "\r\n", "```swift\n", "let 名 = 1\n", "```\n"])
    }

    // MARK: Live prose incremental rendering (LiveInlineCache)

    /// The live incremental render must produce the SAME characters
    /// AND the same attributes as the whole-block parse at every
    /// append step of a prose stream — the transcript displays its
    /// output directly, so any divergence would flicker while tokens
    /// stream in. (Paragraph breaks ride presentationIntent
    /// identities; a naive prefix+tail concat merges the boundary
    /// paragraphs into one run and the break is lost.)
    func testLiveProseSplitRenderMatchesWholeAtEveryStep() {
        let chunks = [
            "First paragraph with `code` and **bold**",
            " keeps growing",
            "\n\nSecond para",
            "graph with a [link](https://example.com) and 6~8°C",
            "\n\n\nThird\nspanning lines\n\nFourth tail",
        ]
        let cache = LiveInlineCache()
        var source = ""
        for chunk in chunks {
            source += chunk
            let live = cache.render(source)
            let whole = renderInlineMarkdown(source)
            XCTAssertEqual(
                String(live.characters), String(whole.characters),
                "characters diverged at: \(source.debugDescription)",
            )
            XCTAssertEqual(live, whole, "attributes diverged at: \(source.debugDescription)")
        }
    }

    /// stableProsePrefixSplit: the split sits just past the LAST blank
    /// line; sources without a blank line (or ending on one) stay whole.
    func testStableProsePrefixSplitBoundaries() {
        XCTAssertEqual(stableProsePrefixSplit("a\n\nb").map { String("a\n\nb"[$0...]) }, "b")
        XCTAssertEqual(stableProsePrefixSplit("a\n\nb\n\nc").map { String("a\n\nb\n\nc"[$0...]) }, "c")
        // Whitespace-only lines count as blank.
        XCTAssertEqual(stableProsePrefixSplit("a\n \nb").map { String("a\n \nb"[$0...]) }, "b")
        // No blank line, or the blank line is the tail: no split.
        XCTAssertNil(stableProsePrefixSplit("one growing paragraph"))
        XCTAssertNil(stableProsePrefixSplit("a\n\n"))
        XCTAssertNil(stableProsePrefixSplit(""))
    }

    /// Multiple blank lines between paragraphs survive the split render
    /// exactly (splitBlocks preserves them; the render must too).
    func testLiveProseSplitRenderPreservesBlankRuns() {
        let source = "first\n\n\n\nsecond\n\n\nthird"
        let cache = LiveInlineCache()
        XCTAssertEqual(
            String(cache.render(source).characters),
            String(renderInlineMarkdown(source).characters),
        )
        XCTAssertEqual(cache.render(source), renderInlineMarkdown(source))
    }
}
