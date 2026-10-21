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

    func testLiveUnicodeScalarAppendsAndCombiningBoundaries() {
        let sources = [
            "e\u{301}\n\n中👩🏽‍💻\n\n🇨🇦\n\n각 tail",
            "first\n\n \u{301}\n\n```swift\nlet café = \"👨‍👩‍👧\"\n\n```\n\nlast",
            "first\n\n\u{301}tail\n\nsecond",
            "a\r\n\r\nb\r\n\r\n```\r\nx\r\n\r\n```\r\n\r\nlast",
        ]
        for source in sources {
            // Scalars, not Characters: expose appends that merge an old
            // end Character, including ZWJ, combining marks and CR + LF.
            assertIncrementalMatchesWhole(source.unicodeScalars.map { String($0) })
        }
    }

    func testLiveReplacementResetsEvenWhenCompletedPrefixIsUnchanged() {
        let cache = LiveBlockCache()
        let sources = [
            "first\n\nsecond\n\nunfinished tail",
            "first\n\nsec", // Shorter, with the same sealed prefix.
            "first\n\nsecond replacement grows beyond the previous source\n\nlast",
            "first\n\nsecond replacement grows beyond the previous source\n\nchanged tail",
            "first\n\n```swift\ncode\n\n", // Replace an unsealed tail with a fence.
            "first\n\nplain\n\nlast", // Remove the open fence.
            "FIRST\n\nplain\n\nlast", // Edit already sealed content.
            "", "x", "x\n\n", "x\n\nagain",
            "é\n\ntail",
            "e\u{301}\n\ntail grows", // Canonically equal, different UTF-8 offsets.
            "é\n\ntail grows further\n\nlast",
        ]
        for source in sources {
            XCTAssertEqual(cache.blocks(for: source), MarkdownText.splitBlocks(source), source.debugDescription)
            XCTAssertEqual(cache.blocks(for: source), MarkdownText.splitBlocks(source), "repeated snapshot")
        }
    }

    func testLiveSealsMultipleRegionsInOneAppend() {
        assertIncrementalMatchesWhole([
            "first\n\n",
            "second\n\nthird\n\n- one\n- two\n\n> quote\n> more\n\n",
            "| a | b |\n| --- | --- |\n| 1 | 2 |\n\n```swift\ncode\n\n```\n\n",
            "last\n\n\n",
        ])
    }

    func testLiveFenceTableAndListBoundaryPrecedence() {
        let sources = [
            // Pipe rows inside a table are NOT parsed as fences.
            "| a | b |\n| --- | --- |\n``` | body |\n\n```swift\ncode\n\n```\n\nafter",
            // Only the first consecutive quote line can start a table.
            "> quote\n> a | b |\n| --- | --- |\n```\n\ncode\n```\n\nafter",
            "> a | b |\n| --- | --- |\n``` | row |\n\nafter",
            "- a | b |\n| --- | --- |\n| 1 | 2 |\n\n- item\n  - nested\n\nafter",
            "first\n\n\u{00A0}```swift\ncode\n\n```\n\nafter",
            "first\n\n```\n\n```\n\n```py\n\nopen tail",
            "first\n\n| header |\n\n| --- |\n\n---\n\nlast",
        ]
        for source in sources {
            assertIncrementalMatchesWhole(source.unicodeScalars.map { String($0) })
        }
    }

    func testLiveSingleLongTailWithAndWithoutSealedPrefix() {
        let chunks = (0 ..< 64).map { "segment \($0) café 👩🏽‍💻 " + String(repeating: "x", count: 96) }
        assertIncrementalMatchesWhole(chunks)
        assertIncrementalMatchesWhole(["sealed\n\n"] + chunks + ["\n\n", "last"])
        assertIncrementalMatchesWhole(["sealed\n\n```swift\n"] + chunks + ["\n\n", "```\n\nlast"])
    }

    /// No production instrumentation: run the previous cache in the test
    /// itself. Prebuilt snapshots keep stream construction out of timing.
    /// Report median cumulative/worst/p95 costs, without noisy speed gates.
    func testBenchLiveNewlySealedPrefixAgainstPreviousCache() {
        let benchmarkStart = ProcessInfo.processInfo.systemUptime
        /// Keep the matrix bounded: 32 frames per growing tail, 64 repeated
        /// frames, one warmup and three measured rounds. No nanosecond gates.
        func snapshots(_ chunks: [String]) -> [String] {
            var source = ""
            return chunks.map {
                source += $0
                return source
            }
        }
        func stream(_ source: String) -> [String] {
            // Scalar chunks are valid UTF-8, but may split grapheme clusters.
            let scalars = Array(source.unicodeScalars)
            let step = max(1, (scalars.count + 31) / 32)
            return snapshots(stride(from: 0, to: scalars.count, by: step).map { start in
                String(String.UnicodeScalarView(scalars[start ..< min(start + step, scalars.count)]))
            })
        }
        func ascii(_ bytes: Int) -> String {
            String(String(repeating: "ordinary text ", count: bytes / 14 + 1).prefix(bytes))
        }
        func unicode(_ bytes: Int) -> String {
            let unit = "中 e\u{301} 👩🏽‍💻 🇨🇦 "
            return String(repeating: unit, count: bytes / unit.utf8.count)
        }
        var cases: [(name: String, snapshots: [String], useOld: Bool)] = []
        for paragraphs in [160, 320] {
            var chunks: [String] = []
            for index in 0 ..< paragraphs {
                chunks += ["Paragraph \(index): " + ascii(112), "\n", "\n"]
            }
            cases.append(("sealed-prefix-\(paragraphs)", snapshots(chunks), true))
        }
        cases += [
            ("single-paragraph-short-4KB", stream(ascii(4096)), true),
            ("single-paragraph-long-24KB", stream(ascii(24576)), true),
            ("open-fence-24KB", stream("```swift\n" + ascii(24576)), true),
            ("repeat-sealed-1KB", Array(repeating: ascii(1022) + "\n\n", count: 64), true),
            ("unicode-single-paragraph-4KB", stream(unicode(4096)), false),
            ("unicode-single-paragraph-24KB", stream(unicode(24576)), false),
            ("unicode-open-fence-24KB", stream("```swift\n" + unicode(24576)), false),
            ("unicode-repeat-sealed-1KB", Array(repeating: unicode(1022) + "\n\n", count: 64), false),
        ]
        let mixedChunks = [
            "## Result\n", "\nIntro with **bold** and `code`", ".\n", "\n",
            "- first", " item\n- [ ] second\n", "  - nested\n", "\n",
            "```swift", "\nlet value = 1\n", "\nlet next = 2\n", "```", "\n", "\n",
            "| key | value |\n", "| --- | --- |\n", "| a | 1 |\n", "| b | 2 |", "\n\n",
            "> quote\n", "> continuation\n", "\n", "Final paragraph", ".\n\n",
        ]
        cases.append(("mixed-chunk-stream", snapshots(Array(repeating: mixedChunks, count: 3).flatMap(\.self)), true))
        let unicodeMixed = mixedChunks.map {
            $0.replacingOccurrences(of: "first", with: "中 e\u{301} 👩🏽‍💻")
                .replacingOccurrences(of: "value", with: "值")
                .replacingOccurrences(of: "paragraph", with: "段落 🇨🇦")
        }
        cases.append(("unicode-mixed-chunk-stream", snapshots(Array(repeating: unicodeMixed, count: 3).flatMap(\.self)), false))

        /// Content-sensitive checksum, not just block counts: all text and
        /// structural fields participate. Hashing and assertions are OUTSIDE
        /// the timed parse. The same process uses the same Hasher seed.
        func checksum(_ blocks: [MarkdownText.Block]) -> Int {
            var hash = Hasher()
            hash.combine(blocks.count)
            for block in blocks {
                switch block {
                case let .prose(text):
                    hash.combine(0); hash.combine(text)
                case let .heading(level, text):
                    hash.combine(1); hash.combine(level); hash.combine(text)
                case let .list(items):
                    hash.combine(2); hash.combine(items.count)
                    for item in items {
                        hash.combine(item.ordinal); hash.combine(item.indent)
                        hash.combine(item.checkbox); hash.combine(item.text)
                    }
                case let .code(language, code):
                    hash.combine(3); hash.combine(language); hash.combine(code)
                case let .table(header, rows):
                    hash.combine(4); hash.combine(header); hash.combine(rows)
                case let .quote(text):
                    hash.combine(5); hash.combine(text)
                case .rule:
                    hash.combine(6)
                }
            }
            return hash.finalize()
        }
        func median(_ values: [Double]) -> Double {
            values.sorted()[values.count / 2]
        }
        for benchmark in cases {
            let expected = benchmark.snapshots.map { checksum(MarkdownText.splitBlocks($0)) }
            func timed(_ parse: (String) -> [MarkdownText.Block]) -> (total: Double, worst: Double, p95: Double) {
                var times: [Double] = []
                times.reserveCapacity(benchmark.snapshots.count)
                for (index, snapshot) in benchmark.snapshots.enumerated() {
                    let start = ProcessInfo.processInfo.systemUptime
                    let blocks = parse(snapshot)
                    times.append((ProcessInfo.processInfo.systemUptime - start) * 1000)
                    XCTAssertEqual(checksum(blocks), expected[index], "\(benchmark.name), frame \(index)")
                }
                let sorted = times.sorted()
                return (times.reduce(0, +), sorted.last ?? 0, sorted[(sorted.count * 95 + 99) / 100 - 1])
            }
            func reference() -> (total: Double, worst: Double, p95: Double) {
                benchmark.useOld
                    ? timed(PreviousLiveBlockCache().blocks(for:))
                    : timed(MarkdownText.splitBlocks)
            }
            // Warm BOTH paths. Fresh caches reproduce the entire stream.
            _ = reference()
            _ = timed(LiveBlockCache().blocks(for:))
            var old: [(total: Double, worst: Double, p95: Double)] = []
            var new: [(total: Double, worst: Double, p95: Double)] = []
            for round in 0 ..< 3 {
                // Alternate order to reduce systematic thermal/order bias.
                if round.isMultiple(of: 2) {
                    old.append(reference())
                    new.append(timed(LiveBlockCache().blocks(for:)))
                } else {
                    new.append(timed(LiveBlockCache().blocks(for:)))
                    old.append(reference())
                }
            }
            let oldTotal = median(old.map(\.total))
            let newTotal = median(new.map(\.total))
            print(String(
                format: "LiveBlockCache %@: bytes=%d frames=%d reference=%@ rounds=3 median total/worst/p95(ms) old=%.3f/%.3f/%.3f new=%.3f/%.3f/%.3f new/old=%.3fx",
                benchmark.name, benchmark.snapshots.last?.utf8.count ?? 0, benchmark.snapshots.count,
                benchmark.useOld ? "old-cache" : "whole-Unicode",
                oldTotal, median(old.map(\.worst)), median(old.map(\.p95)),
                newTotal, median(new.map(\.worst)), median(new.map(\.p95)),
                newTotal / max(oldTotal, Double.leastNonzeroMagnitude),
            ))
        }
        print(String(format: "LiveBlockCache benchmark matrix wall=%.3fs (target <3s, not a timing assertion)",
                     ProcessInfo.processInfo.systemUptime - benchmarkStart))
    }

    /// Frozen pre-optimization implementation, used only for the ASCII
    /// benchmark; Unicode and replacement correctness use splitBlocks.
    private final class PreviousLiveBlockCache {
        private var parsedPrefix = ""
        private var prefixBlocks: [MarkdownText.Block] = []
        private var scannedSource = ""
        private var scannedFences = 0
        private var stableOffset = 0

        func blocks(for source: String) -> [MarkdownText.Block] {
            guard let split = stableBoundary(of: source) else {
                return MarkdownText.splitBlocks(source)
            }
            let prefix = source[..<split]
            if prefix != parsedPrefix[...] {
                prefixBlocks = MarkdownText.splitBlocks(String(prefix))
                parsedPrefix = String(prefix)
            }
            let tail = String(source[split...])
            return tail.isEmpty ? prefixBlocks : prefixBlocks + MarkdownText.splitBlocks(tail)
        }

        private func stableBoundary(of source: String) -> String.Index? {
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
                if line.allSatisfy({ $0 == " " || $0 == "\t" || $0 == "\r" }), scannedFences % 2 == 0 {
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
