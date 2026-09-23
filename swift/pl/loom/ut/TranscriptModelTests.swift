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

/// TranscriptModel derivation: tool call/result pairing across messages,
/// orphaned results, the action-row turn gate, and the render model's
/// status/diff derivation.
final class TranscriptModelTests: XCTestCase {
    private func user(_ id: String, _ text: String) -> Message {
        Message(id: id, role: .user, status: .final, parts: [.text(text)], createdAt: nil)
    }

    private func assistant(_ id: String, _ parts: [ContentPart]) -> Message {
        Message(id: id, role: .assistant, status: .final, parts: parts, createdAt: nil)
    }

    private func call(_ id: String, name: String = "read_file") -> ContentPart {
        .toolCall(ContentPart.ToolCall(id: id, name: name, arguments: nil))
    }

    private func result(_ callId: String, status: String = "success") -> ContentPart {
        .toolResult(ContentPart.ToolResult(
            callId: callId, status: status,
            content: [.text("ok")], error: nil,
            startedAt: nil, finishedAt: nil,
        ))
    }

    // MARK: Rows & pairing

    func testToolResultArrivingInLaterMessageMergesIntoCallRow() {
        let model = TranscriptModel.build(messages: [
            assistant("a1", [call("c1")]),
            assistant("a2", [result("c1")]),
        ], midTurn: false)

        // The result-only message folds into the call's block; only the
        // call row remains.
        XCTAssertEqual(model.rows.count, 1)
        guard case let .message(rowModel) = model.rows[0],
              case let .tool(render) = rowModel.items[0]
        else {
            return XCTFail("expected a tool item")
        }
        XCTAssertEqual(render.status, .success)
        XCTAssertEqual(render.fullOutput, "ok")
    }

    func testOrphanToolResultIsDropped() {
        let model = TranscriptModel.build(messages: [
            assistant("a1", [result("nope")]),
        ], midTurn: false)
        XCTAssertTrue(model.rows.isEmpty)
    }

    func testModelOnlyArtifactsAreFiltered() {
        var artifact = ContentPart.Artifact(id: "art_1", size: 10, mediaType: "image/png")
        artifact.modelOnly = true
        let model = TranscriptModel.build(messages: [
            assistant("a1", [.text("done"), .artifact(artifact)]),
        ], midTurn: false)
        XCTAssertEqual(model.rows.count, 1)
        guard case let .message(rowModel) = model.rows[0] else {
            return XCTFail("expected a message row")
        }
        XCTAssertEqual(rowModel.items.count, 1)
        guard case .markdown = rowModel.items[0] else {
            return XCTFail("expected only the markdown item")
        }
    }

    // MARK: Action-row turn gate (WebUI closeTurn)

    func testActionsAttachToLastAssistantTextBeforeNextUser() {
        let model = TranscriptModel.build(messages: [
            assistant("a1", [.text("first")]),
            assistant("a2", [.text("second")]),
            user("u1", "next"),
        ], midTurn: false)
        let actions = model.rows.compactMap { row -> String? in
            guard case let .message(model) = row, model.showActions else { return nil }
            return model.message.id
        }
        XCTAssertEqual(actions, ["a2"])
    }

    func testTailActionsGatedByMidTurn() {
        let messages = [
            user("u1", "hi"),
            assistant("a1", [.text("answer")]),
        ]
        func tailShowsActions(_ model: TranscriptModel) -> Bool? {
            guard case let .message(rowModel) = model.rows.last else { return nil }
            return rowModel.showActions
        }
        XCTAssertEqual(
            tailShowsActions(TranscriptModel.build(messages: messages, midTurn: false)),
            true,
        )
        XCTAssertEqual(
            tailShowsActions(TranscriptModel.build(messages: messages, midTurn: true)),
            false,
        )
    }

    // MARK: Turn summaries (WebUI closeTurn / .block-turn-summary)

    private func summary(_ runId: String, _ paths: [String]) -> TurnSummary {
        TurnSummary(
            runId: runId, turn: 1, cancelled: nil, failed: nil,
            changes: paths.map { TurnFileChange(path: $0, created: true, edits: 1, size: nil) },
        )
    }

    private func assistant(_ id: String, _ text: String, runId: String) -> Message {
        var message = assistant(id, [.text(text)])
        message.metadata = ["run_id": runId]
        return message
    }

    func testTurnSummaryMountsAtTurnBoundaryAndTail() {
        let model = TranscriptModel.build(messages: [
            user("u1", "q1"),
            assistant("a1", "first", runId: "run_1"),
            user("u2", "q2"),
            assistant("a2", "second", runId: "run_2"),
        ], midTurn: false, turnSummaries: [
            summary("run_1", ["a.txt"]),
            summary("run_2", ["b.txt"]),
        ])
        // run_1's card sits between its last block and the next user
        // bubble; run_2's closes the transcript tail.
        XCTAssertEqual(model.rows.map(\.id), ["u1", "a1", "tsm-run_1", "u2", "a2", "tsm-run_2"])
    }

    func testTurnSummarySkippedWithoutMatchingRunIdOrFiles() {
        // No run_id metadata: no boundary, no card.
        let untagged = TranscriptModel.build(messages: [
            user("u1", "q1"),
            assistant("a1", [.text("first")]),
        ], midTurn: false, turnSummaries: [summary("run_1", ["a.txt"])])
        XCTAssertEqual(untagged.rows.map(\.id), ["u1", "a1"])

        // Empty changes: the card itself is skipped (WebUI filters
        // change-less summaries before building the map).
        let empty = TranscriptModel.build(messages: [
            user("u1", "q1"),
            assistant("a1", "first", runId: "run_1"),
        ], midTurn: false, turnSummaries: [
            TurnSummary(runId: "run_1", turn: 1, cancelled: nil, failed: nil, changes: []),
        ])
        XCTAssertEqual(empty.rows.map(\.id), ["u1", "a1"])
    }

    func testTurnSummaryEmitsOncePerRun() {
        let model = TranscriptModel.build(messages: [
            user("u1", "q1"),
            assistant("a1", "first", runId: "run_1"),
            assistant("a2", "more", runId: "run_1"),
        ], midTurn: false, turnSummaries: [summary("run_1", ["a.txt"])])
        XCTAssertEqual(model.rows.map(\.id), ["u1", "a1", "a2", "tsm-run_1"])
    }

    func testTurnSummariesCloseAtRunChangeWithoutUserMessage() {
        let model = TranscriptModel.build(messages: [
            user("u1", "q1"),
            assistant("a1", "first", runId: "run_1"),
            assistant("a2", "more", runId: "run_1"),
            assistant("a3", "second", runId: "run_2"),
        ], midTurn: false, turnSummaries: [
            summary("run_1", ["a.txt"]),
            summary("run_2", ["b.txt"]),
        ])
        XCTAssertEqual(
            model.rows.map(\.id),
            ["u1", "a1", "a2", "tsm-run_1", "a3", "tsm-run_2"],
        )
    }

    // MARK: ToolRenderModel

    func testStatusMappingAndDuration() {
        let started = Date(timeIntervalSince1970: 1000)
        let finished = started.addingTimeInterval(1.25)
        let item = ToolBlockItem(
            call: ContentPart.ToolCall(id: "c", name: "run_cmd", arguments: nil),
            result: ContentPart.ToolResult(
                callId: "c", status: "timeout",
                content: nil, error: nil,
                startedAt: started, finishedAt: finished,
            ),
        )
        let model = ToolRenderModel(item: item)
        XCTAssertEqual(model.status, .failed)
        XCTAssertEqual(model.durationMs, 1250)

        let running = ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: "c", name: "run_cmd", arguments: nil),
            result: nil,
        ))
        XCTAssertEqual(running.status, .running)
    }

    func testEditArgsProduceUnifiedDiffWithPathHeader() {
        let args: JSONValue = .object([
            "path": .string("src/main.swift"),
            "old_string": .string("let a = 1\nlet b = 2\n"),
            "new_string": .string("let a = 1\nlet b = 3\n"),
        ])
        let model = ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: "c", name: "edit", arguments: args),
            result: nil,
        ))
        let diff = try? XCTUnwrap(model.diff)
        XCTAssertTrue(diff?.hasPrefix("+++ b/src/main.swift\n") == true)
        XCTAssertTrue(diff?.contains("- let b = 2") == true)
        XCTAssertTrue(diff?.contains("+ let b = 3") == true)
        // The memoized path returns the identical rendering.
        let again = ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: "c", name: "edit", arguments: args),
            result: nil,
        ))
        XCTAssertEqual(again.diff, model.diff)
    }

    func testWriteArgsProducePureAdditionDiff() {
        let args: JSONValue = .object([
            "path": .string("new.txt"),
            "content": .string("hello\nworld\n"),
        ])
        let model = ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: "c", name: "write", arguments: args),
            result: nil,
        ))
        XCTAssertEqual(model.diff, "+++ b/new.txt\n+ hello\n+ world")
    }

    // MARK: parseDiff

    func testParseDiffCountsAndFileLabel() {
        let parsed = parseDiff("+++ b/f.txt\n  ctx\n- old\n+ new\n+ more")
        XCTAssertEqual(parsed.file, "f.txt")
        XCTAssertEqual(parsed.adds, 2)
        XCTAssertEqual(parsed.dels, 1)
        XCTAssertEqual(parsed.lines.count, 4)
        XCTAssertEqual(parsed.lines[0].kind, .ctx)
        XCTAssertEqual(parsed.lines[1].kind, .del)
        XCTAssertEqual(parsed.lines[1].sign, "−")
        XCTAssertEqual(parsed.lines[2].kind, .add)
    }

    func testDiffParseCacheReturnsConsistentResults() {
        let text = "+ a\n+ b"
        XCTAssertEqual(DiffParseCache.parse(text).lines.count, parseDiff(text).lines.count)
    }

    // MARK: Live conversion

    func testLiveToolCallStateConvertsToSameShape() {
        var state = ToolCallState(id: "c", name: "edit")
        state.target = "f.swift"
        state.status = .success
        state.durationMs = 42
        state.preview = "preview output"
        let model = ToolRenderModel(live: state)
        XCTAssertEqual(model.name, "edit")
        XCTAssertEqual(model.target, "f.swift")
        XCTAssertEqual(model.status, .success)
        XCTAssertEqual(model.durationMs, 42)
        XCTAssertEqual(model.output, "preview output")
        XCTAssertEqual(model.fullOutput, "preview output")
    }
}
