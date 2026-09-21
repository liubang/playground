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

/// Golden-wire decoding tests: the JSON literals below mirror what
/// `loom serve` actually puts on the wire (internal/runtimeevent/event.go,
/// internal/app/controller.go).
final class ProtocolDecodingTests: XCTestCase {
    private func decode<T: Decodable>(_: T.Type, _ json: String) throws -> T {
        try LoomJSON.decoder.decode(T.self, from: Data(json.utf8))
    }

    func testRuntimeEventEnvelope() throws {
        let event = try decode(RuntimeEvent.self, """
        {
          "version": 1, "sequence": 1236, "session_id": "sess_abc",
          "run_id": "run_1", "turn": 3, "kind": "model.text_delta",
          "time": "2026-09-19T10:20:30.123456789Z", "durable": false,
          "payload": {"request_id": "req_9", "delta": "hello"}
        }
        """)
        XCTAssertEqual(event.version, 1)
        XCTAssertEqual(event.sequence, 1236)
        XCTAssertEqual(event.sessionId, "sess_abc")
        XCTAssertEqual(event.kind, .modelTextDelta)
        XCTAssertFalse(event.durable)

        let payload = try XCTUnwrap(try event.payload?.decoded(as: ModelDeltaPayload.self, using: LoomJSON.decoder))
        XCTAssertEqual(payload.requestId, "req_9")
        XCTAssertEqual(payload.delta, "hello")
    }

    func testUnknownEventKindDecodesAsUnknown() throws {
        let event = try decode(RuntimeEvent.self, """
        {"version":1,"sequence":1,"session_id":"s","kind":"future.new_kind","durable":false}
        """)
        XCTAssertEqual(event.kind, .unknown)
    }

    func testApprovalRequestedPayload() throws {
        let payload = try decode(ApprovalRequestedPayload.self, """
        {
          "approval_id": "evt_1", "call_id": "call_2", "tool_name": "run_cmd",
          "source": "builtin", "risk": 3, "description": "rm -rf build/",
          "args_hash": "deadbeef", "read_paths": ["a.go"],
          "write_paths": ["b.go"], "arguments": {"cmd": "rm -rf build/"}
        }
        """)
        XCTAssertEqual(payload.approvalId, "evt_1")
        XCTAssertEqual(payload.risk, 3)
        XCTAssertEqual(payload.readPaths, ["a.go"])
        XCTAssertEqual(payload.arguments?["cmd"]?.stringValue, "rm -rf build/")
    }

    func testSnapshotWithPendingRequestsAndFractionalDate() throws {
        let snapshot = try decode(Snapshot.self, """
        {
          "state": "awaiting_approval", "session_id": "sess_abc",
          "model_name": "claude-opus", "provider_name": "anthropic",
          "context_window": 200000, "occupancy": 45000,
          "workspace_root": "/repo", "turn_count": 2,
          "usage": {"turns": 2, "tool_calls": 5, "input_tokens": 10000,
                    "output_tokens": 800, "cached_input_tokens": 9000,
                    "context_tokens": 45000, "reasoning_tokens": 0, "cost_usd": 0.12},
          "messages": [
            {"id": "msg_1", "role": "user", "status": "final",
             "parts": [{"kind": "text", "text": "hi"}],
             "created_at": "2026-09-19T10:00:00.5Z"}
          ],
          "pending_requests": [
            {"kind": "approval", "id": "evt_1",
             "approval": {"approval_id": "evt_1", "call_id": "c", "tool_name": "edit_file",
                          "risk": 2, "description": "edit", "args_hash": "h"}}
          ],
          "event_seq": 42, "timestamp": "2026-09-19T10:20:30Z"
        }
        """)
        XCTAssertEqual(snapshot.state, .awaitingApproval)
        XCTAssertEqual(snapshot.eventSeq, 42)
        XCTAssertEqual(snapshot.occupancy, 45000)
        XCTAssertEqual(snapshot.messages?.count, 1)
        XCTAssertEqual(snapshot.pendingRequests?.count, 1)

        let request = try XCTUnwrap(snapshot.pendingRequests?[0])
        XCTAssertEqual(request.kind, .approval)
        XCTAssertEqual(request.approval?.toolName, "edit_file")

        guard case let .text(text)? = snapshot.messages?.first?.parts.first else {
            return XCTFail("expected text part")
        }
        XCTAssertEqual(text, "hi")
    }

    func testContentPartToolCallAndResult() throws {
        let message = try decode(Message.self, """
        {"id": "m", "role": "assistant", "parts": [
          {"kind": "reasoning", "reasoning": {"text": "hmm", "duration_ms": 1200}},
          {"kind": "tool_call", "tool_call": {"id": "c1", "name": "read_file",
             "arguments": {"path": "main.go"}}},
          {"kind": "tool_result", "tool_result": {"call_id": "c1", "status": "success",
             "content": [{"kind": "text", "text": "package main"}]}}
        ]}
        """)
        XCTAssertEqual(message.parts.count, 3)
        guard case let .reasoning(reasoning) = message.parts[0] else {
            return XCTFail("expected reasoning part")
        }
        XCTAssertEqual(reasoning.text, "hmm")
        guard case let .toolCall(call) = message.parts[1] else {
            return XCTFail("expected tool_call part")
        }
        XCTAssertEqual(call.arguments?["path"]?.stringValue, "main.go")
        guard case let .toolResult(result) = message.parts[2] else {
            return XCTFail("expected tool_result part")
        }
        XCTAssertEqual(result.status, "success")
    }

    /// view_image marks its artifact part model_only (hidden from display
    /// channels); present_image marks it present_only (display-only). The
    /// flags are part-level siblings of the nested artifact payload.
    func testArtifactPartDisplayFlags() throws {
        let message = try decode(Message.self, """
        {"id": "m", "role": "assistant", "parts": [
          {"kind": "artifact_ref",
           "artifact": {"id": "art_1", "size": 42, "media_type": "image/png"},
           "model_only": true},
          {"kind": "artifact_ref",
           "artifact": {"id": "art_2", "size": 7, "media_type": "image/png"},
           "present_only": true},
          {"kind": "artifact_ref",
           "artifact": {"id": "art_3", "size": 1}}
        ]}
        """)
        guard case let .artifact(modelBound) = message.parts[0] else {
            return XCTFail("expected artifact part")
        }
        XCTAssertTrue(modelBound.modelOnly)
        XCTAssertFalse(modelBound.presentOnly)
        XCTAssertEqual(modelBound.id, "art_1")
        XCTAssertEqual(modelBound.mediaType, "image/png")

        guard case let .artifact(presentBound) = message.parts[1] else {
            return XCTFail("expected artifact part")
        }
        XCTAssertFalse(presentBound.modelOnly)
        XCTAssertTrue(presentBound.presentOnly)

        // Older records carry no flags: both default to false.
        guard case let .artifact(legacy) = message.parts[2] else {
            return XCTFail("expected artifact part")
        }
        XCTAssertFalse(legacy.modelOnly)
        XCTAssertFalse(legacy.presentOnly)
    }

    /// tool.completed carries display-bound artifact refs for live
    /// rendering (runtimeevent.ToolCompletedPayload.Artifacts).
    func testToolCompletedPayloadWithArtifacts() throws {
        let payload = try decode(ToolCompletedPayload.self, """
        {
          "call_id": "c1", "tool_name": "present_image", "status": "success",
          "duration_ms": 19, "preview": "image: plot.png · image/png · 7 bytes",
          "artifacts": [{"id": "art_9", "size": 7, "media_type": "image/png"}]
        }
        """)
        XCTAssertEqual(payload.status, .success)
        let artifacts = try XCTUnwrap(payload.artifacts)
        XCTAssertEqual(artifacts.count, 1)
        XCTAssertEqual(artifacts[0].id, "art_9")
        XCTAssertEqual(artifacts[0].mediaType, "image/png")
        // Wire refs never carry part-level flags: they default to false,
        // and the server has already excluded model_only artifacts.
        XCTAssertFalse(artifacts[0].modelOnly)

        let bare = try decode(ToolCompletedPayload.self, """
        {"call_id": "c2", "tool_name": "run_cmd", "status": "success"}
        """)
        XCTAssertNil(bare.artifacts)
    }

    func testSessionSummaryList() throws {
        let list = try decode(SessionListResponse.self, """
        {"sessions": [
          {"id": "sess_1", "created_at": "2026-09-19T08:00:00Z",
           "updated_at": "2026-09-19T09:00:00Z", "workspace_id": "ws_1",
           "state": "idle", "model_name": "claude-opus", "turn_count": 4,
           "title": "fix bloom filter"}
        ], "next_cursor": null}
        """)
        XCTAssertEqual(list.sessions.count, 1)
        XCTAssertEqual(list.sessions[0].title, "fix bloom filter")
        XCTAssertNotNil(list.sessions[0].updatedAt)
    }

    func testErrorModel() throws {
        let body = try decode(APIErrorBody.self, """
        {"error": {"code": "binding_mismatch", "message": "stale binding", "state": "running"}}
        """)
        XCTAssertEqual(body.error.code, "binding_mismatch")
        XCTAssertEqual(body.error.state, "running")
    }

    func testRFC3339WithAndWithoutFraction() {
        XCTAssertNotNil(LoomJSON.parseRFC3339("2026-09-19T10:20:30Z"))
        XCTAssertNotNil(LoomJSON.parseRFC3339("2026-09-19T10:20:30.123456789Z"))
        XCTAssertNil(LoomJSON.parseRFC3339("not a date"))
    }

    func testJSONValueRoundTrip() throws {
        let value = try decode(JSONValue.self, """
        {"a": [1, 2.5, true, null, "x"], "b": {"c": -3}}
        """)
        XCTAssertEqual(value["a"]?.intValue, nil)
        guard case let .array(array)? = value["a"] else {
            return XCTFail("expected array")
        }
        XCTAssertEqual(array[0], .int(1))
        XCTAssertEqual(array[1], .double(2.5))
        XCTAssertEqual(array[2], .bool(true))
        XCTAssertEqual(array[3], .null)
        XCTAssertEqual(array[4], .string("x"))
        XCTAssertEqual(value["b"]?["c"], .int(-3))
    }
}
