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

    // MARK: Explicit turn closure projection

    private func runMessage(_ id: String, _ parts: [ContentPart], run: String = "r", status: MessageStatus = .final) -> Message {
        Message(id: id, role: .assistant, status: status, parts: parts, createdAt: nil, metadata: ["run_id": run])
    }

    private var successClosure: TurnClosure {
        TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: ["final"])
    }

    func testClosureEligibilityIsExplicitAndComplete() {
        let messages = [runMessage("process", [.text("working")]), runMessage("final", [.text("done")])]
        let legacy = TranscriptModel.build(messages: messages, midTurn: false)
        XCTAssertEqual(legacy.projectedRows(expandedRuns: []), legacy.rows)
        for closure in [
            TurnClosure(),
            TurnClosure(runId: "r", outcome: "succeeded"),
            TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: []),
            TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: ["final", "missing"]),
            TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: ["final", "final"]),
        ] {
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [closure])
            XCTAssertTrue(model.processes.isEmpty)
            XCTAssertEqual(model.projectedRows(expandedRuns: []), legacy.rows)
        }
        for outcome in ["failed", "cancelled", "budget_exhausted", "truncated", "unknown"] {
            let closure = TurnClosure(runId: "r", outcome: outcome, finalAnswerMessageIds: ["final"])
            XCTAssertTrue(TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [closure]).processes.isEmpty)
        }
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
        XCTAssertEqual(model.projectedRows(expandedRuns: []).map(\.id), ["process-r", "final"])
        XCTAssertTrue(TranscriptModel.build(messages: messages, midTurn: true, turnClosures: [successClosure]).processes.isEmpty)
        XCTAssertTrue(TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure, successClosure]).processes.isEmpty)
    }

    func testFinalReferencesRequireSameRunFinalStatusAndAnswerContent() {
        for final in [
            runMessage("final", [.text("done")], run: "other"),
            runMessage("final", [.text("done")], status: .draft),
            runMessage("final", [.text("done")], status: .interrupted),
            runMessage("final", [.reasoning(.init(text: "thinking", redacted: nil, durationMs: nil))]),
            user("final", "not an answer"),
        ] {
            let model = TranscriptModel.build(messages: [runMessage("process", [.text("work")]), final], midTurn: false, turnClosures: [successClosure])
            XCTAssertTrue(model.processes.isEmpty)
        }
        let duplicate = runMessage("final", [.text("answer")])
        XCTAssertTrue(TranscriptModel.build(messages: [duplicate, duplicate], midTurn: false, turnClosures: [successClosure]).processes.isEmpty)
    }

    func testPureQADoesNotCreateEmptyProcessEntry() {
        let model = TranscriptModel.build(messages: [user("u", "hi"), runMessage("final", [.text("hello")])], midTurn: false, turnClosures: [successClosure])
        XCTAssertTrue(model.processes.isEmpty)
        XCTAssertEqual(model.projectedRows(expandedRuns: []).map(\.id), ["u", "final"])
    }

    func testFinalReasoningSeparatesFromBodyImagesArtifactsAndReconstructs() {
        let reasoning = ContentPart.Reasoning(text: "thought", redacted: nil, durationMs: 10)
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "image")
        let artifact = ContentPart.Artifact(id: "artifact", size: 1, mediaType: "text/plain")
        let model = TranscriptModel.build(messages: [
            user("u", "question"), runMessage("final", [.reasoning(reasoning), .text("answer"), .image(image), .artifact(artifact)]),
        ], midTurn: false, turnClosures: [successClosure])
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["u", "process-r", "final"])
        guard case let .message(final) = folded.last else { return XCTFail("missing final") }
        XCTAssertEqual(final.items, [.markdown("answer"), .image(image), .artifact(artifact)])
        XCTAssertTrue(final.showActions)
        let expanded = model.projectedRows(expandedRuns: ["r"]).filter { row in
            if case .process = row {
                return false
            }; return true
        }
        XCTAssertEqual(expanded, model.rows)
    }

    func testProcessIsFlatOrderedAcrossSteerAndKeepsSummaryVisible() {
        let messages = [
            user("u", "q"),
            runMessage("tools", [call("c1"), result("c1"), call("c2"), result("c2")]),
            user("steer", "also check"),
            runMessage("work", [.text("more work")]),
            runMessage("final", [.text("answer")]),
        ]
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnSummaries: [summary("r", ["x"])], turnClosures: [successClosure])
        XCTAssertEqual(model.projectedRows(expandedRuns: []).map(\.id), ["u", "process-r", "steer", "final", "tsm-r"])
        let expanded = model.projectedRows(expandedRuns: ["r"]).filter { row in
            if case .process = row {
                return false
            }; return true
        }
        XCTAssertEqual(expanded, model.rows)
        guard case let .message(tools) = model.rows[1] else { return XCTFail("tools") }
        XCTAssertEqual(tools.items.count, 2)
        XCTAssertFalse(tools.items.contains {
            if case .toolGroup = $0 {
                return true
            }; return false
        })
        XCTAssertEqual(model.toolLocation(callId: "c2")?.rowId, "tools")
        XCTAssertEqual(model.toolLocation(callId: "c2")?.processRunId, "r")
        XCTAssertEqual(model.toolLocation(callId: "c2")?.scrollId, "chat-tool-c2")
        // A new active run must not prevent the completed earlier run projection.
        XCTAssertEqual(TranscriptModel.build(messages: messages, midTurn: true, turnClosures: [successClosure], activeRunId: "new").processes["r"], ["final"])
    }

    func testMultipleFinalAnswersAndLongHiddenProcessKeepFinalInTailWindow() {
        let process = (0 ..< 80).map { runMessage("p\($0)", [.text("work")]) }
        let closure = TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: ["f1", "f2"])
        let messages = [user("u", "q")] + process + [runMessage("f1", [.text("one")]), runMessage("f2", [.text("two")])]
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [closure])
        XCTAssertEqual(model.projectedRows(expandedRuns: []).suffix(30).map(\.id), ["u", "process-r", "f1", "f2"])
        XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
            if case .process = $0 {
                return false
            }; return true
        }, model.rows)
        XCTAssertTrue(TranscriptModel.build(messages: Array(messages.dropLast()), midTurn: false, turnClosures: [closure]).processes.isEmpty)
        let invisible = TranscriptModel.build(messages: [runMessage("p", [.text("  ")]), runMessage("final", [.text("answer")])], midTurn: false, turnClosures: [successClosure])
        XCTAssertTrue(invisible.processes.isEmpty)
    }

    func testHoldOpenTransitionAndManualChoiceAreSticky() {
        var state = TurnProcessState()
        state.followingBottom = true
        state.beginRun("r")
        state.reconcile(eligibleRuns: []) // closure missing: no decision yet
        state.reconcile(eligibleRuns: ["r"])
        XCTAssertFalse(state.expandedRuns.contains("r"))
        state.setExpanded(runId: "r", expanded: true)
        state.beginRun("next")
        state.reconcile(eligibleRuns: ["r", "next"])
        XCTAssertTrue(state.expandedRuns.contains("r"))
        state.setExpanded(runId: "r", expanded: false)
        state.reconcile(eligibleRuns: ["r"])
        XCTAssertFalse(state.expandedRuns.contains("r"))
        var reader = TurnProcessState()
        reader.followingBottom = true
        reader.beginRun("r")
        reader.holdOpen = true
        reader.beginRun("next") // late closure cannot erase earlier reading intent
        reader.reconcile(eligibleRuns: ["r"])
        XCTAssertTrue(reader.expandedRuns.contains("r"))
        var historical = TurnProcessState()
        historical.followingBottom = true
        historical.reconcile(eligibleRuns: ["old"])
        XCTAssertFalse(historical.expandedRuns.contains("old"))
        var away = TurnProcessState()
        away.beginRun("r")
        away.followingBottom = true
        away.reconcile(eligibleRuns: ["r"])
        XCTAssertTrue(away.expandedRuns.contains("r"))
    }

    func testAnyIncompleteOrFailedToolBlocksEntireRunEvenAfterRecovery() {
        let badResults: [[ContentPart]] = [
            [], [result("c", status: "running")], [result("c", status: "error")],
            [result("c", status: "cancelled")], [result("c", status: "timeout")],
            [result("c", status: "ok")], [result("c", status: "unknown")],
            [result("c"), result("c")],
            [.toolResult(.init(callId: "c", status: "success", content: nil,
                               error: .init(code: "error", message: "failed"), startedAt: nil, finishedAt: nil))],
        ]
        for results in badResults {
            let messages = [runMessage("work", [.text("working"), call("c")]),
                            runMessage("results", results),
                            runMessage("recovery", [call("recovered"), result("recovered")]),
                            runMessage("final", [.text("done")])]
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
            XCTAssertTrue(model.processes.isEmpty)
            XCTAssertEqual(model.projectedRows(expandedRuns: []), model.rows)
        }
        let orphan = TranscriptModel.build(messages: [runMessage("work", [.text("work"), result("orphan", status: "error")]),
                                                      runMessage("final", [.text("done")])], midTurn: false, turnClosures: [successClosure])
        XCTAssertTrue(orphan.processes.isEmpty)
    }

    func testRecoveredCrossRunResultDisplaysButCannotAuthorizeFolding() {
        for resultRun: String? in ["other", nil] {
            var carrier = runMessage("result", [result("c")])
            carrier.metadata = resultRun.map { ["run_id": $0] }
            let messages = [runMessage("work", [call("c")]), carrier,
                            runMessage("final", [.text("done")])]
            let before = messages
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
            XCTAssertTrue(model.processes.isEmpty)
            XCTAssertEqual(model.projectedRows(expandedRuns: []), model.rows)
            XCTAssertEqual(model.rows.map(\.id), ["work", "final"])
            guard case let .message(row) = model.rows.first, case let .tool(tool) = row.items.first
            else { return XCTFail("missing recovered tool") }
            XCTAssertEqual(tool.status, .success)
            XCTAssertEqual(tool.fullOutput, "ok")
            XCTAssertEqual(messages, before)
            XCTAssertEqual(row.message, messages[0])
        }
    }

    func testLegacyCallDisplaysLastGlobalResultStampedWithNewRun() {
        let messages = [
            assistant("legacy", [call("c")]),
            runMessage("earlier", [result("c", status: "error")], run: "recovery1"),
            runMessage("latest", [result("c")], run: "recovery2"),
        ]
        let before = messages
        let model = TranscriptModel.build(messages: messages, midTurn: false)
        XCTAssertEqual(model.rows.map(\.id), ["legacy"])
        guard case let .message(row) = model.rows.first, case let .tool(tool) = row.items.first
        else { return XCTFail("missing legacy tool") }
        XCTAssertEqual(tool.status, .success)
        XCTAssertEqual(tool.fullOutput, "ok")
        XCTAssertEqual(messages, before)
        XCTAssertEqual(row.message, messages[0])
    }

    func testDisplayPrefersLastSameRunResultWhenCallIDsConflict() {
        let model = TranscriptModel.build(messages: [
            runMessage("work", [.text("work"), call("c"), result("c")]),
            runMessage("other", [call("c"), result("c"), result("c", status: "error")], run: "other"),
            runMessage("final", [.text("done")]),
        ], midTurn: false, turnClosures: [successClosure])
        XCTAssertNotNil(model.processes["r"])
        XCTAssertEqual(model.projectedRows(expandedRuns: []).map(\.id), ["process-r", "other", "final"])
        guard case let .message(work) = model.rows.first, case let .tool(first) = work.items.last,
              case let .message(other) = model.rows[1], case let .tool(second) = other.items.first
        else { return XCTFail("missing conflicting tools") }
        XCTAssertEqual(first.status, .success)
        XCTAssertEqual(second.status, .failed)
    }

    func testDeliveryAndReviewRemainVisibleWithoutEmptyProcessHeader() {
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "image")
        let artifact = ContentPart.Artifact(id: "artifact", size: 10, mediaType: "text/plain")
        for name in ["write", "edit", "apply_patch", "multi_edit", "MultiEdit", "write_file"] {
            let messages = [runMessage("delivery", [.image(image), .artifact(artifact), call("c", name: name), result("c")]),
                            runMessage("final", [.text("answer")])]
            let before = messages
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
            let rowsBefore = model.rows
            let folded = model.projectedRows(expandedRuns: [])
            XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-delivery"], name)
            guard case let .delivery(delivery) = folded.last,
                  case let .message(source) = model.rows.first,
                  case let .tool(tool) = delivery.items.last
            else { return XCTFail("delivery") }
            XCTAssertEqual(delivery.source, source)
            XCTAssertEqual(delivery.items.count, 3)
            XCTAssertTrue(delivery.showsReview)
            XCTAssertTrue(tool.needsReview) // absent diff still needs an entrance to the original process
            XCTAssertNil(tool.diff)
            XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
                if case .process = $0 {
                    return false
                }; return true
            }, rowsBefore)
            XCTAssertEqual(model.rows, rowsBefore)
            XCTAssertEqual(messages, before)
            XCTAssertEqual(model.toolLocation(callId: "c")?.processRunId, "r")
        }
        var unsupported = readTool("review")
        unsupported.diff = "unsupported diff"
        XCTAssertTrue(unsupported.needsReview)
        unsupported.diff = String(repeating: "+line\n", count: 1000)
        XCTAssertTrue(unsupported.needsReview)
        XCTAssertTrue(unsupported.hasDelivery(showsReview: true))
        XCTAssertFalse(unsupported.hasDelivery(showsReview: false))
    }

    private func richResult(_ id: String, _ content: [ContentPart]) -> ContentPart {
        .toolResult(.init(callId: id, status: "success", content: content, error: nil,
                          startedAt: nil, finishedAt: nil))
    }

    private func deliveries(_ rows: [TranscriptModel.Row]) -> [TranscriptModel.DeliveryRowModel] {
        rows.compactMap { row in
            if case let .delivery(delivery) = row {
                return delivery
            }
            return nil
        }
    }

    func testToolDeliveryUsesNormalizedInlineArtifactsAndCommandStreams() {
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "original-base64")
        let artifact = ContentPart.Artifact(id: "download", size: 10, mediaType: "application/pdf")
        let stdout = ContentPart.Artifact(id: "out", size: 12, mediaType: "text/plain")
        let stderr = ContentPart.Artifact(id: "err", size: 13, mediaType: "text/plain")
        var hidden = ContentPart.Artifact(id: "model-only", size: 10, mediaType: "application/pdf")
        hidden.modelOnly = true
        let messages = [
            runMessage("inline", [call("image", name: "generate_image"),
                                  richResult("image", [.image(image), .artifact(artifact), .artifact(hidden)])]),
            runMessage("attachment", [call("file", name: "resource"),
                                      richResult("file", [.artifact(artifact), .artifact(hidden)])]),
            runMessage("hidden", [.artifact(hidden), call("hidden-tool", name: "view_image"),
                                  richResult("hidden-tool", [.artifact(hidden)])]),
            runMessage("command", [call("cmd", name: "run_cmd"), richResult("cmd", [
                .text("{\"stdout\":\"ok\",\"stderr\":\"warn\",\"stdout_artifact\":{\"id\":\"out\"},\"stderr_artifact\":{\"id\":\"err\"}}"),
                .artifact(stdout), .artifact(stderr), .artifact(artifact), .artifact(hidden),
            ])]),
            runMessage("final", [.text("answer")]),
        ]
        let before = messages
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
        let rowsBefore = model.rows
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-inline", "delivery-attachment", "delivery-command"])
        let delivered = deliveries(folded)
        guard delivered.count == 3,
              case let .tool(inline) = delivered[0].items.first,
              case let .tool(file) = delivered[1].items.first,
              case let .tool(command) = delivered[2].items.first
        else { return XCTFail("missing normalized media") }
        XCTAssertEqual(inline.images, [image])
        XCTAssertTrue(inline.artifacts.isEmpty) // inline images take precedence over all artifact refs
        XCTAssertEqual(file.artifacts, [artifact])
        XCTAssertEqual(command.artifacts, [artifact])
        XCTAssertEqual(command.deliveryArtifacts, [artifact])
        XCTAssertEqual(command.outputAttachments.map(\.name), ["stdout", "stderr"])
        XCTAssertEqual(command.outputAttachments.map(\.artifact), [stdout, stderr])
        XCTAssertTrue(command.commandOutputFormatted)
        XCTAssertEqual(command.fullOutput, "stdout:\nok\n\nstderr:\nwarn")
        XCTAssertEqual(delivered.map(\.source), model.rows.compactMap {
            guard case let .message(row) = $0, ["inline", "attachment", "command"].contains(row.message.id) else { return nil }
            return row
        })
        XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
            if case .process = $0 {
                return false
            }; return true
        }, rowsBefore)
        XCTAssertEqual(model.rows, rowsBefore)
        XCTAssertEqual(messages, before)
        XCTAssertEqual(folded, model.projectedRows(expandedRuns: [])) // independent stable result identities
    }

    func testCommandInlineDeliveryPreservesStreamsAndFiltersModelOnlyRefs() {
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "original-base64")
        let stdout = ContentPart.Artifact(id: "out", size: 12, mediaType: "text/plain")
        let stderr = ContentPart.Artifact(id: "err", size: 13, mediaType: "text/plain")
        let unrelated = ContentPart.Artifact(id: "download", size: 14, mediaType: "application/pdf")
        var hidden = ContentPart.Artifact(id: "hidden", size: 15, mediaType: "text/plain")
        hidden.modelOnly = true
        // A JSON reference alone must never promote a model-only artifact.
        for stderrID in [stderr.id, hidden.id] {
            let messages = [runMessage("command", [call("cmd", name: "run_cmd"), richResult("cmd", [
                .text("{\"stdout\":\"ok\",\"stderr\":\"warn\",\"stdout_artifact\":{\"id\":\"out\"},\"stderr_artifact\":{\"id\":\"\(stderrID)\"}}"),
                .image(image), .artifact(stdout), .artifact(stderr), .artifact(unrelated), .artifact(hidden),
            ])]), runMessage("final", [.text("answer")])]
            let before = messages
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
            let originalRows = model.rows
            let folded = model.projectedRows(expandedRuns: [])
            XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-command"])
            guard let delivery = deliveries(folded).first,
                  case let .tool(command) = delivery.items.first,
                  case let .message(source) = originalRows.first,
                  case let .tool(sourceTool) = source.items.first
            else { return XCTFail("missing command delivery") }
            XCTAssertEqual(command.images, [image])
            XCTAssertTrue(command.artifacts.isEmpty) // inline hides unrelated and unclaimed visual refs
            XCTAssertTrue(command.deliveryArtifacts.isEmpty)
            XCTAssertEqual(command.outputAttachments.map(\.name), stderrID == hidden.id ? ["stdout"] : ["stdout", "stderr"])
            XCTAssertEqual(command.outputAttachments.map(\.artifact), stderrID == hidden.id ? [stdout] : [stdout, stderr])
            XCTAssertFalse(command.outputAttachments.contains { $0.artifact.modelOnly })
            XCTAssertTrue(command.commandOutputFormatted)
            XCTAssertEqual(command.fullOutput, "stdout:\nok\n\nstderr:\nwarn")
            XCTAssertEqual(command, sourceTool)
            XCTAssertEqual(delivery.source, source)
            XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
                if case .process = $0 {
                    return false
                }; return true
            }, originalRows)
            XCTAssertEqual(model.rows, originalRows)
            XCTAssertEqual(messages, before)
        }
    }

    func testWeatherWorkflowDeliversChartWithoutPromotingAutomaticCommandLogs() {
        let chart = ContentPart.Artifact(id: "chart", size: 193_332, mediaType: "image/png")
        let commandSteps = [("inspect", "7 2026-10-22 2026-10-28"),
                            ("plot", "Font check passed; chart saved"), ("cleanup", "cleaned")]
        var messages = [user("u", "weather"), runMessage("skill", [call("skill", name: "read_skill")]),
                        runMessage("skill-result", [result("skill")]),
                        runMessage("glob", [call("glob", name: "glob")]), runMessage("glob-result", [result("glob")])]
        for (id, stdout) in commandSteps {
            let log = ContentPart.Artifact(id: "stdout-\(id)", size: 10, mediaType: "text/plain")
            messages.append(runMessage(id, [call(id, name: "run_cmd")]))
            messages.append(runMessage("\(id)-result", [richResult(id, [
                .text("{\"stdout\":\"\(stdout)\",\"stderr\":\"\",\"stdout_artifact\":{\"id\":\"\(log.id)\"},\"exit_code\":0}"),
                .artifact(log),
            ])]))
            if id == "plot" {
                messages.append(runMessage("present", [call("present", name: "present_image")]))
                messages.append(runMessage("present-result", [richResult("present", [.artifact(chart)])]))
            }
        }
        messages.append(runMessage("final", [.text("Weather table and key findings")]))
        let before = messages
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["u", "process-r", "final", "delivery-present"])
        guard let delivery = deliveries(folded).first, case let .tool(tool) = delivery.items.first
        else { return XCTFail("missing chart") }
        XCTAssertEqual(tool.deliveryArtifacts, [chart])
        for row in model.rows {
            if case let .message(source) = row, case let .tool(command) = source.items.first, command.name == "run_cmd" {
                XCTAssertEqual(command.outputAttachments.count, 1)
                XCTAssertFalse(command.hasDelivery(showsReview: false))
                XCTAssertTrue(command.deliveryArtifacts.isEmpty)
            }
        }
        XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
            if case .process = $0 {
                return false
            }; return true
        }, model.rows)
        let live = TranscriptModel.build(messages: messages, midTurn: true, turnClosures: [successClosure], activeRunId: "r")
        XCTAssertEqual(live.projectedRows(expandedRuns: []), live.rows)
        XCTAssertEqual(messages, before)
    }

    func testClippedCommandDefersOnlyAmbiguousLogsAndPreservesOriginalRefs() {
        let log = ContentPart.Artifact(id: "out", size: 10, mediaType: "text/plain")
        let unknown = ContentPart.Artifact(id: "unknown", size: 12, mediaType: nil)
        let chart = ContentPart.Artifact(id: "chart", size: 20, mediaType: "image/png")
        for output in ["{\"stdout\":\"clipped", "{\"stdout\":\"missing stderr\"}", ""] {
            for artifacts in [[log, unknown], [log, unknown, chart]] {
                let messages = [runMessage("command", [call("cmd", name: "run_cmd"), richResult("cmd", [
                    .text(output),
                ] + artifacts.map { .artifact($0) })]), runMessage("final", [.text("answer")])]
                let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
                guard case let .message(source) = model.rows.first, case let .tool(tool) = source.items.first
                else { return XCTFail("missing command") }
                XCTAssertEqual(tool.artifacts, artifacts)
                XCTAssertEqual(tool.deliveryArtifacts, artifacts.filter { $0.id == chart.id })
                XCTAssertEqual(deliveries(model.projectedRows(expandedRuns: [])).count, artifacts.contains(chart) ? 1 : 0)
                XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
                    if case .process = $0 {
                        return false
                    }; return true
                }, model.rows)
            }
        }
    }

    func testExplicitCommandFileAndFinalLogReferencesRemainDeliverable() {
        let log = ContentPart.Artifact(id: "out", size: 10, mediaType: "text/plain")
        let report = ContentPart.Artifact(id: "report", size: 20, mediaType: "text/plain")
        let messages = [runMessage("command", [call("cmd", name: "run_cmd"), richResult("cmd", [
            .text("{\"stdout\":\"done\",\"stderr\":\"\",\"stdout_artifact\":{\"id\":\"out\"}}"),
            .artifact(log), .artifact(report),
        ])]), runMessage("final", [.text("answer"), .artifact(log)])]
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-command"])
        guard let delivery = deliveries(folded).first, case let .tool(command) = delivery.items.first,
              case let .message(final) = folded[1]
        else { return XCTFail("missing explicit deliverables") }
        XCTAssertEqual(command.deliveryArtifacts, [report])
        XCTAssertEqual(command.outputAttachments.map(\.artifact), [log])
        XCTAssertEqual(final.items, [.markdown("answer"), .artifact(log)])
    }

    func testFinalToolIsProcessButFinalBodyMediaRemainOnlyInFinal() {
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "image")
        let artifact = ContentPart.Artifact(id: "artifact", size: 10, mediaType: "text/plain")
        let model = TranscriptModel.build(messages: [runMessage("final", [
            .text("answer"), .image(image), .artifact(artifact),
            call("write", name: "write"), richResult("write", [.image(image)]),
        ])], midTurn: false, turnClosures: [successClosure])
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-final"])
        guard case let .message(final) = folded[1], let delivery = deliveries(folded).first,
              case .tool = delivery.items.first
        else { return XCTFail("final tool") }
        XCTAssertEqual(final.items, [.markdown("answer"), .image(image), .artifact(artifact)])
        XCTAssertEqual(delivery.items.count, 1)
        XCTAssertTrue(delivery.isFinal)
        XCTAssertTrue(delivery.showsReview)
        XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
            if case .process = $0 {
                return false
            }; return true
        }, model.rows)
    }

    func testDeliveryAnchorsLastFinalInTranscriptOrderAcrossRunsAndSteer() {
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "image")
        let artifact = ContentPart.Artifact(id: "artifact", size: 10, mediaType: "text/plain")
        let messages = [
            user("u", "question"), runMessage("early", [.image(image)]),
            runMessage("f1", [.text("one")]), user("steer", "also check"),
            runMessage("late", [.artifact(artifact)]), runMessage("f2", [.text("two")]),
            runMessage("after-final", [.image(image)]),
            user("next", "next question"), runMessage("p2", [.artifact(artifact)], run: "r2"),
            runMessage("f3", [.text("three")], run: "r2"),
        ]
        let closures = [TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: ["f2", "f1"]),
                        TurnClosure(runId: "r2", outcome: "succeeded", finalAnswerMessageIds: ["f3"])]
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: closures)
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["u", "process-r", "f1", "steer", "f2", "delivery-early", "delivery-late",
                                          "delivery-after-final", "next", "process-r2", "f3", "delivery-p2"])
        XCTAssertEqual(deliveries(folded).map { $0.source.message.metadata?["run_id"] }, ["r", "r", "r", "r2"])
        XCTAssertEqual(model.projectedRows(expandedRuns: ["r", "r2"]).filter {
            if case .process = $0 {
                return false
            }; return true
        }, model.rows)
        XCTAssertEqual(deliveries(model.projectedRows(expandedRuns: ["r"])).map(\.source.message.id), ["p2"])
    }

    func testSameRunSummaryIsOnlyReviewEntranceAndAnchorsBesideResult() {
        let image = ContentPart.ImageContent(mediaType: "image/png", data: "image")
        let messages = [runMessage("write", [call("c1", name: "write"), result("c1")]),
                        runMessage("final", [.text("answer"), call("c2", name: "edit"), richResult("c2", [.image(image)])]),
                        runMessage("later", [.text("process after answer")])]
        for summaries in [[summary("other", ["x"])], [summary("r", [])], []] {
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnSummaries: summaries,
                                              turnClosures: [successClosure])
            XCTAssertEqual(deliveries(model.projectedRows(expandedRuns: [])).filter(\.showsReview).count, 2)
        }
        let model = TranscriptModel.build(messages: messages, midTurn: false, turnSummaries: [summary("r", ["x"])],
                                          turnClosures: [successClosure])
        let folded = model.projectedRows(expandedRuns: [])
        XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-final", "tsm-r"])
        XCTAssertTrue(deliveries(folded).allSatisfy { !$0.showsReview })
        XCTAssertEqual(folded.filter {
            if case .turnSummary = $0 {
                return true
            }; return false
        }.count, 1)
        XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
            if case .process = $0 {
                return false
            }; return true
        }, model.rows)
        XCTAssertEqual(model.toolLocation(callId: "c2")?.rowId, "final")
        XCTAssertEqual(model.toolLocation(callId: "c2")?.processRunId, "r")
    }

    func testDiffOnlyToolDeliveryReferencesWholeSourceWithoutEscapingProse() {
        for diff in ["unsupported diff", String(repeating: "+line\n", count: 1000)] {
            var tool = readTool("diff")
            tool.diff = diff
            let source = TranscriptModel.MessageRowModel(
                message: runMessage("diff-source", [.text("working")]),
                items: [.markdown("working"), .tool(tool)], showActions: false,
            )
            let final = TranscriptModel.MessageRowModel(
                message: runMessage("final", [.text("answer")]), items: [.markdown("answer")], showActions: true,
            )
            let model = TranscriptModel(rows: [.message(source), .message(final)], processes: ["r": ["final"]])
            let folded = model.projectedRows(expandedRuns: [])
            XCTAssertEqual(folded.map(\.id), ["process-r", "final", "delivery-diff-source"])
            guard let delivery = deliveries(folded).first else { return XCTFail("missing diff review") }
            XCTAssertEqual(delivery.source, source)
            XCTAssertEqual(delivery.items, [.tool(tool)])
            XCTAssertTrue(delivery.showsReview)
            XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
                if case .process = $0 {
                    return false
                }; return true
            }, model.rows)
        }
    }

    func testRichToolProjectionDoesNotChangeLiveOrUntrustedClosureRows() {
        let artifact = ContentPart.Artifact(id: "download", size: 10, mediaType: "text/plain")
        let messages = [runMessage("rich", [.text("working"), call("c", name: "write"),
                                            richResult("c", [.artifact(artifact)])]),
                        runMessage("final", [.text("answer")])]
        for model in [
            TranscriptModel.build(messages: messages, midTurn: true, turnClosures: [successClosure]),
            TranscriptModel.build(messages: messages, midTurn: false),
            TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure, successClosure]),
        ] {
            XCTAssertTrue(model.processes.isEmpty)
            XCTAssertEqual(model.projectedRows(expandedRuns: []), model.rows)
            XCTAssertTrue(deliveries(model.projectedRows(expandedRuns: [])).isEmpty)
        }
    }

    func testInternalContinuationKindsDoNotSplitSteerOrSummary() throws {
        for kind in ["plan_reconcile", "goal_continue"] {
            let continuation = try LoomJSON.decoder.decode(Message.self, from: Data("""
            {"id":"continuation","role":"user","status":"final","metadata":{"run_id":"r","kind":"\(kind)"},
            "parts":[{"kind":"text","text":"continue work"}]}
            """.utf8))
            XCTAssertEqual(continuation.metadata?["kind"], kind)
            XCTAssertTrue(continuation.isInternalContinuation)
            var steer = user("steer", "also check")
            steer.metadata = ["run_id": "r", "kind": "steer"]
            let messages = [user("u", "q"), runMessage("work", [.text("work")]),
                            steer, continuation, runMessage("final", [.text("done")])]
            let model = TranscriptModel.build(messages: messages, midTurn: false,
                                              turnSummaries: [summary("r", ["x"])], turnClosures: [successClosure])
            XCTAssertEqual(model.projectedRows(expandedRuns: []).map(\.id), ["u", "process-r", "steer", "final", "tsm-r"])
            XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
                if case .process = $0 {
                    return false
                }
                return true
            }, model.rows)
            for closures in [[], [TurnClosure(runId: "r", outcome: "failed")],
                             [TurnClosure(runId: "r", outcome: "succeeded", finalAnswerMessageIds: ["missing"])]]
            {
                let untrusted = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: closures)
                XCTAssertEqual(untrusted.projectedRows(expandedRuns: []), untrusted.rows)
                XCTAssertTrue(untrusted.rows.contains { $0.id == "continuation" })
            }
            let live = TranscriptModel.build(messages: messages, midTurn: true,
                                             turnClosures: [successClosure], activeRunId: "r")
            XCTAssertEqual(live.projectedRows(expandedRuns: []), live.rows)
            let failed = messages + [runMessage("failed", [call("c"), result("c", status: "error")])]
            let open = TranscriptModel.build(messages: failed, midTurn: false,
                                             turnSummaries: [summary("r", ["x"])], turnClosures: [successClosure])
            XCTAssertTrue(open.processes.isEmpty)
            XCTAssertEqual(open.rows.last?.id, "tsm-r")
            XCTAssertEqual(open.rows.filter {
                if case .turnSummary = $0 {
                    return true
                }; return false
            }.count, 1)
        }
    }

    func testInternalUserAloneCreatesProcessButNotRealTurnBoundary() {
        for kind in ["plan_reconcile", "goal_continue"] {
            var continuation = user("continuation", "continue work")
            continuation.metadata = ["run_id": "r", "kind": kind]
            let messages = [user("u", "question"), continuation, runMessage("final", [.text("answer")])]
            let model = TranscriptModel.build(messages: messages, midTurn: false, turnClosures: [successClosure])
            XCTAssertEqual(model.projectedRows(expandedRuns: []).map(\.id), ["u", "process-r", "final"])
            XCTAssertEqual(model.projectedRows(expandedRuns: ["r"]).filter {
                if case .process = $0 {
                    return false
                }
                return true
            }, model.rows)

            // A trailing synthetic user must not close the assistant action row
            // or move the active history boundary past the current run's tools.
            let activeMessages = [user("u", "question"),
                                  runMessage("tools", [call("c1"), result("c1"), call("c2"), result("c2")]),
                                  runMessage("work", [.text("work")]), continuation]
            let active = TranscriptModel.build(messages: activeMessages, midTurn: true,
                                               turnSummaries: [summary("r", ["x"])])
            guard case let .message(tools) = active.rows[1],
                  case let .message(work) = active.rows[2]
            else { return XCTFail("missing active rows") }
            XCTAssertFalse(tools.items.contains {
                if case .toolGroup = $0 {
                    return true
                }; return false
            })
            XCTAssertFalse(work.showActions)
            XCTAssertEqual(active.rows.map(\.id), ["u", "tools", "work", "continuation", "tsm-r"])

            // Unknown kinds and untagged users remain real user input.
            let userMetadata: [[String: String]?] = [nil, ["run_id": "r"], ["run_id": "r", "kind": "other"]]
            for metadata in userMetadata {
                var realUser = user("real", "actual input")
                realUser.metadata = metadata
                let real = TranscriptModel.build(messages: messages + [realUser], midTurn: false,
                                                 turnClosures: [successClosure])
                XCTAssertEqual(real.projectedRows(expandedRuns: []).last?.id, "real")
            }
        }
    }

    func testProcessStateDefaultsHistoryCollapsedAndBoundsAllRunCollections() {
        var state = TurnProcessState()
        state.reconcile(eligibleRuns: ["history"])
        XCTAssertFalse(state.expandedRuns.contains("history"))
        state.setExpanded(runId: "history", expanded: true)
        state.reconcile(eligibleRuns: ["history"])
        XCTAssertTrue(state.expandedRuns.contains("history"))
        for index in 0 ..< 450 {
            let run = "run-\(index)"
            state.beginRun(run)
            state.holdOpen = true
            state.setExpanded(runId: run, expanded: true)
        }
        XCTAssertEqual(state.retainedRunCount, 400)
        XCTAssertEqual(state.expandedRuns.count, 400)
        XCTAssertEqual(state.observedRuns.count, 400)
        XCTAssertFalse(state.expandedRuns.contains("run-0"))
        XCTAssertTrue(state.expandedRuns.contains("run-449"))
        state.reconcile(eligibleRuns: ["run-449"])
        XCTAssertTrue(state.expandedRuns.contains("run-449"))
        // macOS 14 never reports followingBottom: a live run remains open.
        var legacyPlatform = TurnProcessState()
        legacyPlatform.beginRun("live")
        legacyPlatform.reconcile(eligibleRuns: ["live"])
        XCTAssertTrue(legacyPlatform.expandedRuns.contains("live"))
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

    func testIndexedNextAssistantPreservesNilRunAndTrailingUserBoundaries() {
        let messages = [
            runMessage("first", [.text("first")]),
            user("boundary", "next"),
            assistant("no-run", [.text("legacy")]),
            runMessage("later", [.text("later")]),
            user("tail1", "next"), user("tail2", "next"),
        ]
        let model = TranscriptModel.build(
            messages: messages, midTurn: false, turnSummaries: [summary("r", ["file"])],
        )
        // The nil-run assistant is the immediate next assistant: do not skip it
        // and incorrectly treat the user boundary as a same-run steer.
        XCTAssertEqual(model.rows.map(\.id), ["first", "tsm-r", "boundary", "no-run", "later", "tail1", "tail2"])
        let onlyUsers = (0 ..< 1000).map { user("u\($0)", "next") }
        XCTAssertEqual(TranscriptModel.build(messages: onlyUsers, midTurn: false).rows.map(\.id), onlyUsers.map(\.id))
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
