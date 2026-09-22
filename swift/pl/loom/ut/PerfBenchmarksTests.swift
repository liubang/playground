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
@testable import Loom
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

    // MARK: Reporting

    private func report(_ name: String, before: Double, after: Double) {
        let speedup = after > 0 ? before / after : .infinity
        print(String(
            format: "LOOM-BENCH %-46@ before=%9.2fms after=%9.3fms speedup=%6.1fx",
            name as NSString, before, after, speedup,
        ))
    }
}
