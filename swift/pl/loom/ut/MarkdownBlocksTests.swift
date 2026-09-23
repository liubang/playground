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
        XCTAssertEqual(MarkdownText.splitBlocks("| a |\n\n| --- |"), [
            .prose("| a |\n\n| --- |"),
        ])
        XCTAssertEqual(MarkdownText.splitBlocks("| a |\n\n\n| --- |"), [
            .prose("| a |\n\n\n| --- |"),
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
}
