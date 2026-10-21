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

    // MARK: Read-only success grouping

    private func readTool(
        _ id: String, name: String = "read_file", status: String = "success",
        content: [ContentPart] = [.text("output")], durationMs: Int64? = nil,
        error: ContentPart.ToolResult.ResultError? = nil,
    ) -> ToolRenderModel {
        let start = durationMs.map { _ in Date(timeIntervalSince1970: 1000) }
        return ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: id, name: name, arguments: nil),
            result: ContentPart.ToolResult(
                callId: id, status: status, content: content, error: error,
                startedAt: start,
                finishedAt: durationMs.map { start!.addingTimeInterval(Double($0) / 1000) },
            ),
        ))
    }

    private func flatten(_ items: [TranscriptModel.Item]) -> [TranscriptModel.Item] {
        items.flatMap { item in
            if case let .toolGroup(group) = item {
                return group.tools.map { TranscriptModel.Item.tool($0) }
            }
            return [item]
        }
    }

    func testReadOnlySuccessGroupsAtTwoAndKeepsSingleton() {
        let first = readTool("c1")
        let second = readTool("c2", name: "list_dir")
        XCTAssertEqual(TranscriptModel.groupReadOnlyTools([.tool(first)]), [.tool(first)])
        let source: [TranscriptModel.Item] = [.tool(first), .tool(second)]
        let grouped = TranscriptModel.groupReadOnlyTools(source)
        guard case let .toolGroup(group) = grouped.first else { return XCTFail("expected group") }
        XCTAssertEqual(grouped.count, 1)
        XCTAssertEqual(group.id, "tool-group-c1")
        XCTAssertEqual(group.tools, [first, second])
        XCTAssertEqual(group.verbs, ["read", "list"])
        XCTAssertEqual(flatten(grouped), source)
        XCTAssertEqual(grouped, TranscriptModel.groupReadOnlyTools(source))
    }

    func testReadOnlyGroupCapEightPreservesOrderAndRemainder() {
        for count in [8, 9, 10, 17] {
            let source = (0 ..< count).map { TranscriptModel.Item.tool(readTool("c\($0)")) }
            let grouped = TranscriptModel.groupReadOnlyTools(source)
            XCTAssertEqual(flatten(grouped), source)
            XCTAssertEqual(grouped.count, (count + 7) / 8)
            for item in grouped {
                if case let .toolGroup(group) = item {
                    XCTAssertTrue((2 ... 8).contains(group.tools.count))
                }
            }
            if count % 8 == 1 {
                XCTAssertEqual(grouped.last, source.last)
            }
        }
    }

    func testEveryVisibleContentKindBreaksToolGroups() {
        let boundaries: [TranscriptModel.Item] = [
            .markdown("answer"),
            .reasoning(ContentPart.Reasoning(text: "thinking", redacted: nil, durationMs: nil)),
            .image(ContentPart.ImageContent(mediaType: "image/png", data: "image")),
            .artifact(ContentPart.Artifact(id: "art", size: 1, mediaType: "text/plain")),
        ]
        for boundary in boundaries {
            let source: [TranscriptModel.Item] = [
                .tool(readTool("c1")), .tool(readTool("c2")), boundary,
                .tool(readTool("c3")), .tool(readTool("c4")),
            ]
            let grouped = TranscriptModel.groupReadOnlyTools(source)
            XCTAssertEqual(grouped.count, 3)
            XCTAssertEqual(grouped[1], boundary)
            XCTAssertEqual(flatten(grouped), source)
        }
    }

    func testRunningFailedCancelledAndUnknownSuccessAliasesStayUngrouped() {
        var tools = ["failed", "cancelled", "running", "ok", "done", "unknown"].map {
            readTool("c-\($0)", status: $0)
        }
        tools.append(ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: "running", name: "read_file", arguments: nil), result: nil,
        )))
        tools.append(readTool("error", error: .init(code: "error", message: "")))
        var live = ToolCallState(id: "live", name: "read_file")
        live.status = .success
        tools.append(ToolRenderModel(live: live))
        for tool in tools {
            let source: [TranscriptModel.Item] = [.tool(readTool("first")), .tool(tool), .tool(readTool("last"))]
            XCTAssertFalse(tool.canGroupReadOnlySuccess)
            XCTAssertEqual(TranscriptModel.groupReadOnlyTools(source), source)
        }
    }

    func testOnlyExactWhitelistNamesAreEligible() {
        let allowed = ["read_file", "list_dir", "glob", "grep", "search", "search_text", "web_fetch",
                       "web_search", "kb_search", "kb_read", "read_skill"]
        for name in allowed {
            XCTAssertTrue(readTool(name, name: name).canGroupReadOnlySuccess, name)
        }
        let excluded = ["edit", "write", "run_cmd", "update_plan", "delegate_task", "resource",
                        "read_skill_resource", "mcp__read_file", "Read_file", "read_file_extra"]
        for name in excluded {
            XCTAssertFalse(readTool(name, name: name).canGroupReadOnlySuccess, name)
        }
    }

    func testRichResultsErrorsAttachmentsAndDiffNeverGroup() {
        var hiddenArtifact = ContentPart.Artifact(id: "hidden", size: 1, mediaType: "text/plain")
        hiddenArtifact.modelOnly = true
        let artifact = ContentPart.Artifact(id: "art", size: 1, mediaType: "text/plain")
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "image")
        var attachment = readTool("attachment")
        attachment.outputAttachments = [.init(name: "stdout", artifact: artifact)]
        var diff = readTool("diff")
        diff.diff = "+ rich"
        var error = readTool("error")
        error.errorMessage = ""
        let tools = [
            readTool("image", content: [.image(image)]),
            readTool("artifact", content: [.artifact(artifact)]),
            readTool("hidden", content: [.artifact(hiddenArtifact)]),
            readTool("both", content: [.image(image), .artifact(artifact)]),
            attachment, diff, error,
        ]
        for tool in tools {
            XCTAssertFalse(tool.canGroupReadOnlySuccess)
            let source: [TranscriptModel.Item] = [.tool(readTool("first")), .tool(tool), .tool(readTool("last"))]
            XCTAssertEqual(TranscriptModel.groupReadOnlyTools(source), source)
        }
    }

    func testGroupDurationIsSumOnlyWhenAllDefinedAndVerbsDeduplicate() {
        let first = readTool("c1", name: "grep", durationMs: 250)
        let second = readTool("c2", name: "search_text", durationMs: 750)
        let group = ToolGroupModel(tools: [first, second])
        XCTAssertEqual(group.combinedDurationMs, 1000)
        XCTAssertEqual(group.verbs, ["search"])
        XCTAssertNil(ToolGroupModel(tools: [first, readTool("missing")]).combinedDurationMs)
        var overflowing = first
        overflowing.durationMs = Int64.max
        XCTAssertNil(ToolGroupModel(tools: [overflowing, second]).combinedDurationMs)
    }

    func testBuildGroupsWithinMessageAndDoesNotMutateSource() {
        let messages = [assistant("a", [call("c1"), call("c2"), result("c1"), result("c2")])]
        let before = messages
        let model = TranscriptModel.build(messages: messages, midTurn: false)
        guard case let .message(row) = model.rows.first,
              case let .toolGroup(group) = row.items.first else { return XCTFail("expected history group") }
        XCTAssertEqual(row.message, messages[0])
        XCTAssertEqual(group.tools.map(\.callId), ["c1", "c2"])
        XCTAssertEqual(messages, before)
        XCTAssertEqual(model, TranscriptModel.build(messages: messages, midTurn: false))
    }

    func testMessageRunUserSystemAndSummaryBoundariesRemainIntact() {
        var first = assistant("a1", [call("c1"), result("c1")])
        first.metadata = ["run_id": "run1"]
        var second = assistant("a2", [call("c2"), result("c2")])
        second.metadata = ["run_id": "run2"]
        let system = Message(id: "system", role: .system, status: .final, parts: [.text("notice")], createdAt: nil)
        let model = TranscriptModel.build(messages: [first, second, system, user("u", "next")], midTurn: false,
                                          turnSummaries: [summary("run1", ["file"])])
        XCTAssertEqual(model.rows.map(\.id), ["a1", "tsm-run1", "a2", "system", "u"])
        for row in model.rows {
            if case let .message(message) = row {
                XCTAssertFalse(message.items.contains {
                    if case .toolGroup = $0 {
                        return true
                    }; return false
                })
            }
        }
    }

    func testInterruptedAndActionRowsKeepIndividualTools() {
        let interrupted = Message(
            id: "interrupted", role: .assistant, status: .interrupted,
            parts: [call("c1"), call("c2"), result("c1"), result("c2")], createdAt: nil,
        )
        let action = assistant("action", [.text("answer"), call("c3"), call("c4"), result("c3"), result("c4")])
        let model = TranscriptModel.build(messages: [interrupted, action], midTurn: false)
        for row in model.rows {
            guard case let .message(message) = row else { return XCTFail("expected message") }
            XCTAssertFalse(message.items.contains {
                if case .toolGroup = $0 {
                    return true
                }; return false
            })
        }
        guard case let .message(last) = model.rows.last else { return XCTFail("expected action row") }
        XCTAssertTrue(last.showActions)
    }

    func testInvisibleTextAndArtifactPartsStillBreakGrouping() {
        var artifact = ContentPart.Artifact(id: "hidden", size: 1, mediaType: "text/plain")
        artifact.modelOnly = true
        for boundary in [ContentPart.text(" \n"), .artifact(artifact)] {
            let model = TranscriptModel.build(messages: [assistant("a", [
                call("c1"), boundary, call("c2"), result("c1"), result("c2"),
            ])], midTurn: false)
            guard case let .message(row) = model.rows.first else { return XCTFail("expected row") }
            XCTAssertEqual(row.items.count, 2)
            XCTAssertFalse(row.items.contains {
                if case .toolGroup = $0 {
                    return true
                }; return false
            })
        }
    }

    func testMidTurnGroupsPastHistoryButNotCurrentPersistedFinalSegments() {
        let messages = [
            user("past-user", "past"),
            assistant("past", [call("p1"), call("p2"), result("p1"), result("p2")]),
            user("current-user", "current"),
            assistant("current", [call("c1"), call("c2"), result("c1"), result("c2")]),
            assistant("current-later", [call("c3"), call("c4"), result("c3"), result("c4")]),
        ]
        let active = TranscriptModel.build(messages: messages, midTurn: true)
        let settled = TranscriptModel.build(messages: messages, midTurn: false)
        for (index, id) in [(1, "past"), (3, "current"), (4, "current-later")] {
            guard case let .message(activeRow) = active.rows[index],
                  case let .message(settledRow) = settled.rows[index],
                  case .toolGroup = settledRow.items.first else { return XCTFail("expected settled group") }
            XCTAssertEqual(activeRow.message.id, id)
            XCTAssertEqual(flatten(activeRow.items), flatten(settledRow.items))
            if id == "past" {
                XCTAssertEqual(activeRow, settledRow)
            } else {
                XCTAssertEqual(activeRow.items.count, 2)
                for item in activeRow.items {
                    guard case .tool = item else { return XCTFail("active tool must remain visible") }
                }
            }
        }
    }

    func testMidTurnWithoutUserBoundaryKeepsAllToolsVisible() {
        let messages = [assistant("a", [call("c1"), call("c2"), result("c1"), result("c2")])]
        let model = TranscriptModel.build(messages: messages, midTurn: true)
        guard case let .message(row) = model.rows.first else { return XCTFail("expected row") }
        XCTAssertEqual(row.items.count, 2)
        for item in row.items {
            guard case .tool = item else { return XCTFail("active tool must remain visible") }
        }
    }

    func testTraceToolLookupFindsGroupedAndIndividualCallsWithoutChangingIdentity() {
        let model = TranscriptModel.build(messages: [assistant("a", [
            call("c1"), call("c2"), result("c1"), result("c2"),
            call("edit", name: "edit"), result("edit"),
        ])], midTurn: true)
        // Put the same source into a finished earlier turn so grouping is enabled.
        guard case let .message(source) = model.rows.first else { return XCTFail("expected row") }
        let grouped = TranscriptModel.build(messages: [source.message, user("next", "next")], midTurn: true)
        guard case let .message(row) = grouped.rows.first,
              case let .toolGroup(group) = row.items.first else { return XCTFail("expected group") }
        let identity = group.id
        XCTAssertTrue(row.containsTool(callId: "c1"))
        XCTAssertTrue(row.containsTool(callId: "c2"))
        XCTAssertTrue(row.containsTool(callId: "edit"))
        XCTAssertFalse(row.containsTool(callId: "missing"))
        XCTAssertTrue(group.containsTool(callId: "c2"))
        XCTAssertFalse(group.containsTool(callId: "edit"))
        XCTAssertFalse(group.containsTool(callId: nil))
        XCTAssertEqual(group.id, identity)
        XCTAssertEqual(row.message, source.message)
        for callId in ["c1", "c2", "edit"] {
            XCTAssertEqual(grouped.toolLocation(callId: callId), TranscriptModel.ToolLocation(
                rowId: "a", scrollId: "chat-tool-\(callId)",
            ))
        }
        XCTAssertNil(grouped.toolLocation(callId: "missing"))
        XCTAssertNotEqual(grouped.toolLocation(callId: "c1"), grouped.toolLocation(callId: "c2"))
    }

    func testMessageRowEqualityIncludesRevealTarget() {
        let source = assistant("a", [call("c1"), call("c2"), result("c1"), result("c2")])
        let model = TranscriptModel.build(messages: [source], midTurn: false)
        guard case let .message(row) = model.rows.first else { return XCTFail("expected row") }
        let plain = MessageRow(row: row)
        let request = ToolRevealRequest(callId: "c2", sequence: 1)
        let targeted = MessageRow(row: row, toolRevealRequest: request)
        XCTAssertNotEqual(plain, targeted)
        XCTAssertEqual(targeted, MessageRow(row: row, toolRevealRequest: request))
        XCTAssertNotEqual(targeted, MessageRow(
            row: row, toolRevealRequest: ToolRevealRequest(callId: "c1", sequence: 1),
        ))
        // A repeated locate invalidates equality even for the same call.
        XCTAssertNotEqual(targeted, MessageRow(
            row: row, toolRevealRequest: ToolRevealRequest(callId: "c2", sequence: 2),
        ))
    }

    func testCurrentModelReferenceDoesNotMislabelBareSnapshotModel() {
        XCTAssertEqual(currentModelReference(provider: "p", model: "actual", defaultRef: "q/default"), "p/actual")
        XCTAssertEqual(currentModelReference(provider: "", model: "actual", defaultRef: "q/default"), "actual")
        // Even matching names cannot identify a provider unambiguously.
        XCTAssertEqual(currentModelReference(provider: "", model: "default", defaultRef: "q/default"), "default")
        XCTAssertEqual(currentModelReference(provider: "", model: "actual", defaultRef: nil), "actual")
        XCTAssertEqual(currentModelReference(provider: "", model: "", defaultRef: "q/default"), "q/default")
        XCTAssertEqual(currentModelReference(provider: "", model: "", defaultRef: nil), "")
    }

    func testMarkdownHeadingScaleMatchesDesignContract() {
        XCTAssertEqual((1 ... 6).map(markdownHeadingSize), [22, 18, 16, 15, 15, 15])
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

    // MARK: Command output / attachment merging

    func testRunCmdHistoryMergesOnlyMatchingOutputArtifacts() {
        let stdout = ContentPart.Artifact(id: "stdout-id", size: 738, mediaType: "text/plain")
        let unrelated = ContentPart.Artifact(id: "other-id", size: 12, mediaType: "text/plain")
        let payload = """
        {"stdout":"first\\nsecond","stderr":"","exit_code":0,"stdout_artifact":{"id":"stdout-id","size":738}}
        """
        let result = ContentPart.ToolResult(
            callId: "cmd", status: "success",
            content: [.text(payload), .artifact(stdout), .artifact(unrelated)],
            error: nil, startedAt: nil, finishedAt: nil,
        )
        let model = ToolRenderModel(item: ToolBlockItem(
            call: ContentPart.ToolCall(id: "cmd", name: "run_cmd", arguments: nil), result: result,
        ))
        XCTAssertTrue(model.output?.contains("first\nsecond") == true)
        XCTAssertFalse(model.output?.contains("\\n") == true)
        XCTAssertEqual(model.outputAttachments.map(\.name), ["stdout"])
        XCTAssertEqual(model.outputAttachments.first?.artifact.id, stdout.id)
        XCTAssertEqual(model.artifacts.map(\.id), [unrelated.id])
        XCTAssertTrue(model.fullOutput?.contains("exit code: 0") == true)
    }

    func testRunCmdLiveCompleteJSONMergesBothStreams() {
        var state = ToolCallState(id: "cmd", name: "run_cmd")
        state.preview = """
        {"stdout":"ok","stderr":"warning","stdout_artifact":{"id":"out"},"stderr_artifact":{"id":"err"},"stdout_preview_truncated":true}
        """
        state.artifacts = [
            ContentPart.Artifact(id: "out", size: 120, mediaType: "text/plain"),
            ContentPart.Artifact(id: "err", size: 8, mediaType: "text/plain"),
        ]
        let model = ToolRenderModel(live: state)
        XCTAssertEqual(model.outputAttachments.map(\.name), ["stdout", "stderr"])
        XCTAssertTrue(model.artifacts.isEmpty)
        XCTAssertTrue(model.commandOutputFormatted)
        XCTAssertTrue(model.commandOutputTruncated)
        XCTAssertTrue(model.output?.contains("warning") == true)
    }

    func testRunCmdLiveTruncatedJSONPreservesPreviewAndArtifact() {
        var state = ToolCallState(id: "cmd", name: "run_cmd")
        state.preview = "{\"stdout\":\"partial"
        state.artifacts = [ContentPart.Artifact(id: "stdout-id", size: 738, mediaType: "text/plain")]
        let model = ToolRenderModel(live: state)
        XCTAssertEqual(model.output, state.preview)
        XCTAssertTrue(model.outputAttachments.isEmpty)
        XCTAssertEqual(model.artifacts.map(\.id), ["stdout-id"])
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
