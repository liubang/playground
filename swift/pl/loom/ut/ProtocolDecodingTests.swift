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

private final class PromptURLProtocol: URLProtocol {
    nonisolated(unsafe) static var requests: [URLRequest] = []
    nonisolated(unsafe) static var reply: ((URLRequest) throws -> (Int, Data))?

    override class func canInit(with _: URLRequest) -> Bool {
        true
    }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest {
        request
    }

    override func startLoading() {
        let result: (Int, Data)
        do {
            if request.url?.path.hasSuffix("/prompts") == true {
                Self.requests.append(request)
            }
            result = try Self.reply!(request)
        } catch {
            client?.urlProtocol(self, didFailWithError: error)
            return
        }
        let response = HTTPURLResponse(url: request.url!, statusCode: result.0,
                                       httpVersion: nil, headerFields: nil)!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: result.1)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

/// Deterministic delayed responses exercise MainActor reentrancy in run stats.
private final class RunStatsURLProtocol: URLProtocol {
    private static let lock = NSLock()
    private nonisolated(unsafe) static var pending: [RunStatsURLProtocol] = []
    private nonisolated(unsafe) static var paths: [String] = []
    private nonisolated(unsafe) static var urls: [URL] = []
    private nonisolated(unsafe) static var requested: (() -> Void)?

    static func reset() {
        lock.lock()
        pending.removeAll()
        paths.removeAll()
        urls.removeAll()
        requested = nil
        lock.unlock()
    }

    static func count(_ suffix: String) -> Int {
        lock.lock()
        defer { lock.unlock() }
        return paths.filter { $0.hasSuffix(suffix) }.count
    }

    static func query(_ suffix: String, occurrence: Int = 0) -> [String: String] {
        lock.lock()
        let matches = urls.filter { $0.path.hasSuffix(suffix) }
        let url = matches[occurrence]
        lock.unlock()
        return Dictionary(uniqueKeysWithValues: URLComponents(url: url, resolvingAgainstBaseURL: false)?
            .queryItems?.compactMap { item in item.value.map { (item.name, $0) } } ?? [])
    }

    static func onRequest(_ callback: (() -> Void)?) {
        lock.lock()
        requested = callback
        lock.unlock()
    }

    static func complete(_ suffix: String, occurrence: Int = 0, body: String, status: Int = 200) {
        lock.lock()
        let matches = pending.filter { $0.request.url?.path.hasSuffix(suffix) == true }
        let target = matches[occurrence]
        pending.removeAll { $0 === target }
        lock.unlock()
        target.respond(body, status: status)
    }

    override class func canInit(with _: URLRequest) -> Bool {
        true
    }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest {
        request
    }

    override func startLoading() {
        Self.lock.lock()
        Self.paths.append(request.url!.path)
        Self.urls.append(request.url!)
        Self.pending.append(self)
        let callback = Self.requested
        Self.lock.unlock()
        callback?()
    }

    private func respond(_ body: String, status: Int) {
        let response = HTTPURLResponse(url: request.url!, statusCode: status,
                                       httpVersion: nil, headerFields: nil)!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Data(body.utf8))
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

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

    func testSnapshotCursorAfterTerminalEventAndRejectedReplay() {
        // The server can project idle before publishing turn.finished (seq 42),
        // so a subsequent snapshot with watermark 41 must not replay it.
        XCTAssertEqual(SessionStore.reconciledCursor(previous: 42, snapshot: 41, reset: false), 42)
        XCTAssertEqual(SessionStore.reconciledCursor(previous: 42, snapshot: 45, reset: false), 45)
        // server.resync rejects the old cursor; the next attach must use the
        // snapshot's watermark, including after a new instance resets sequences.
        XCTAssertEqual(SessionStore.reconciledCursor(previous: 42, snapshot: 41, reset: true), 41)
        XCTAssertEqual(SessionStore.reconciledCursor(previous: 42, snapshot: 3, reset: true), 3)
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

    @MainActor
    func testPromptRetryReusesKeyAndPreservesDraft() async throws {
        let protocolMock = PromptURLProtocol.self
        protocolMock.requests = []
        protocolMock.reply = { request in
            if request.url?.path.hasSuffix("/snapshot") == true {
                return (200, Data(#"{"state":"idle","session_id":"s","model_name":"test","turn_count":0,"event_seq":0}"#.utf8))
            }
            if protocolMock.requests.count == 1 {
                throw URLError(.networkConnectionLost) // submission may have succeeded
            }
            return (200, Data(#"{"turn":1,"deduplicated":true}"#.utf8))
        }
        defer { protocolMock.reply = nil; protocolMock.requests = [] }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [protocolMock]
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        let store = try SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        ))
        await store.refresh()
        store.composerDraft = "  hello  "
        let first = try XCTUnwrap(store.sendComposerDraft())
        XCTAssertNil(store.sendComposerDraft()) // in-flight submission is claimed synchronously
        store.composerDraft = "new draft"
        await first.value
        XCTAssertEqual(store.composerDraft, "  hello  \nnew draft")
        XCTAssertFalse(store.sendingPrompt)

        store.composerDraft = "  hello  "
        let retry = try XCTUnwrap(store.sendComposerDraft())
        await retry.value
        XCTAssertEqual(protocolMock.requests.count, 2)
        XCTAssertEqual(protocolMock.requests[0].value(forHTTPHeaderField: "Idempotency-Key"),
                       protocolMock.requests[1].value(forHTTPHeaderField: "Idempotency-Key"))
        XCTAssertEqual(store.composerDraft, "")

        // After an acknowledged retry, an identical message is a NEW intent.
        store.composerDraft = "hello"
        await store.sendComposerDraft()?.value
        XCTAssertEqual(protocolMock.requests.count, 3)
        XCTAssertNotEqual(protocolMock.requests[1].value(forHTTPHeaderField: "Idempotency-Key"),
                          protocolMock.requests[2].value(forHTTPHeaderField: "Idempotency-Key"))
    }

    @MainActor
    func testRejectedPromptGetsNewKey() async throws {
        let protocolMock = PromptURLProtocol.self
        protocolMock.requests = []
        protocolMock.reply = { request in
            if request.url?.path.hasSuffix("/snapshot") == true {
                return (200, Data(#"{"state":"idle","session_id":"s","model_name":"test","turn_count":0,"event_seq":0}"#.utf8))
            }
            if protocolMock.requests.count == 1 {
                return (400, Data(#"{"error":{"code":"invalid_input","message":"rejected"}}"#.utf8))
            }
            return (202, Data(#"{"turn":1}"#.utf8))
        }
        defer { protocolMock.reply = nil; protocolMock.requests = [] }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [protocolMock]
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        let store = try SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        ))
        await store.refresh()
        store.composerDraft = "hello"
        await store.sendComposerDraft()?.value
        XCTAssertEqual(store.composerDraft, "hello")
        await store.sendComposerDraft()?.value
        XCTAssertEqual(protocolMock.requests.count, 2)
        XCTAssertNotEqual(protocolMock.requests[0].value(forHTTPHeaderField: "Idempotency-Key"),
                          protocolMock.requests[1].value(forHTTPHeaderField: "Idempotency-Key"))
    }

    @MainActor
    func testSnapshotRebuildUsesFinalStateMessagesAndSummaries() async throws {
        let protocolMock = PromptURLProtocol.self
        protocolMock.requests = []
        protocolMock.reply = { _ in
            (200, Data(#"{"state":"idle","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"messages":[{"id":"a1","role":"assistant","status":"final","metadata":{"run_id":"a"},"parts":[{"kind":"text","text":"done"}]}],"turn_summaries":[{"run_id":"a","turn":1,"changes":[{"path":"f.txt","created":true,"edits":1}]}]}"#.utf8))
        }
        defer { protocolMock.reply = nil; protocolMock.requests = [] }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [protocolMock]
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        let store = try SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        ))
        await store.refresh()
        XCTAssertEqual(store.transcript.rows.map(\.id), ["a1", "tsm-a"])
        await store.refresh()
        XCTAssertEqual(store.transcript.rows.map(\.id), ["a1", "tsm-a"])
    }

    @MainActor
    func testTurnFailureSnapshotIsContextualAndDismissalSurvivesRefresh() async throws {
        let protocolMock = PromptURLProtocol.self
        protocolMock.requests = []
        protocolMock.reply = { _ in
            (200, Data(#"{"state":"idle","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"last_error":{"message":"provider unavailable"}}"#.utf8))
        }
        defer { protocolMock.reply = nil; protocolMock.requests = [] }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [protocolMock]
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        let store = try SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        ))

        await store.refresh()
        XCTAssertEqual(store.turnFeedback, .failed("provider unavailable"))
        XCTAssertEqual(store.visibleTurnFailure, "provider unavailable")
        XCTAssertNil(store.lastError)

        store.dismissTurnFailure()
        await store.refresh()
        XCTAssertNil(store.visibleTurnFailure)
        XCTAssertEqual(store.turnFeedback, .failed("provider unavailable"))
    }

    @MainActor
    func testContinuePreparesEditableDraftWithoutSubmitting() async throws {
        let protocolMock = PromptURLProtocol.self
        protocolMock.requests = []
        protocolMock.reply = { _ in
            (200, Data(#"{"state":"idle","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"last_error":{"message":"request interrupted"}}"#.utf8))
        }
        defer { protocolMock.reply = nil; protocolMock.requests = [] }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [protocolMock]
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        let store = try SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        ))
        await store.refresh()

        store.composerDraft = "my unfinished draft"
        XCTAssertFalse(store.prepareContinuation())
        XCTAssertEqual(store.composerDraft, "my unfinished draft")
        store.composerDraft = ""
        let focusRequest = store.composerFocusRequest
        XCTAssertTrue(store.prepareContinuation())
        XCTAssertTrue(store.composerDraft.contains("do not repeat completed actions"))
        XCTAssertNotEqual(store.composerFocusRequest, focusRequest)
        XCTAssertTrue(protocolMock.requests.isEmpty)
        XCTAssertFalse(store.prepareContinuation())
        XCTAssertFalse(store.canRetryLastTurn)
    }

    @MainActor
    func testRetryLastTurnSubmitsOnlyOnClickAndPreservesDraft() async throws {
        let protocolMock = PromptURLProtocol.self
        protocolMock.requests = []
        protocolMock.reply = { request in
            if request.url?.path.hasSuffix("/snapshot") == true {
                return (200, Data(#"{"state":"idle","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"last_error":{"message":"provider unavailable"},"messages":[{"id":"u1","role":"user","status":"final","parts":[{"kind":"text","text":"previous prompt"}]}]}"#.utf8))
            }
            return (202, Data(#"{"turn":2}"#.utf8))
        }
        defer { protocolMock.reply = nil; protocolMock.requests = [] }
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [protocolMock]
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        let store = try SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        ))
        await store.refresh()
        store.composerDraft = "unsent text"
        XCTAssertNil(store.retryLastTurn())
        XCTAssertEqual(store.composerDraft, "unsent text")
        XCTAssertTrue(protocolMock.requests.isEmpty)

        store.composerDraft = ""
        await store.retryLastTurn()?.value
        XCTAssertEqual(protocolMock.requests.count, 1)
        XCTAssertNil(store.turnFeedback)
        XCTAssertEqual(store.composerDraft, "")
        XCTAssertNil(store.retryLastTurn())
    }

    @MainActor
    private func sessionListStore() throws -> (SessionListStore, URLSession) {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [RunStatsURLProtocol.self]
        let session = URLSession(configuration: config)
        return try (SessionListStore(api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        )), session)
    }

    @MainActor
    func testSessionListLoadsAllPagesWithCursorAndDeduplicatesBeforeSorting() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try sessionListStore()
        defer { session.invalidateAndCancel() }
        let path = "/v1/sessions"
        var load: Task<Void, Never>!
        await waitForRunRequest { load = Task { await store.loadSessions() } }
        XCTAssertTrue(store.isLoading)
        XCTAssertEqual(RunStatsURLProtocol.query(path), ["limit": "200", "workspace_id": "all"])
        let second = expectation(description: "second session page")
        RunStatsURLProtocol.onRequest { second.fulfill() }
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"older","updated_at":"2026-09-19T08:00:00Z","title":"first"},{"id":"duplicate","updated_at":"2026-09-19T09:00:00Z","title":"original"}],"next_cursor":"next /+?"}"#)
        await fulfillment(of: [second], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
        XCTAssertTrue(store.isLoading)
        XCTAssertEqual(RunStatsURLProtocol.query(path, occurrence: 1),
                       ["limit": "200", "workspace_id": "all", "cursor": "next /+?"])
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"duplicate","updated_at":"2026-09-19T11:00:00Z","title":"later"},{"id":"newer","updated_at":"2026-09-19T10:00:00Z","title":"new"}],"next_cursor":""}"#)
        await load.value
        XCTAssertEqual(store.sessions.map(\.id), ["newer", "duplicate", "older"])
        XCTAssertEqual(store.sessions.first { $0.id == "duplicate" }?.title, "original")
        XCTAssertNil(store.loadError)
        XCTAssertFalse(store.isLoading)
        XCTAssertEqual(RunStatsURLProtocol.count(path), 2)
    }

    @MainActor
    func testSessionListEmptyCursorStopsAfterFirstPage() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try sessionListStore()
        defer { session.invalidateAndCancel() }
        let path = "/v1/sessions"
        var load: Task<Void, Never>!
        await waitForRunRequest { load = Task { await store.loadSessions() } }
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"session-1"}],"next_cursor":""}"#)
        await load.value
        XCTAssertEqual(store.sessions.map(\.id), ["session-1"])
        XCTAssertEqual(RunStatsURLProtocol.count(path), 1)
        XCTAssertNil(store.loadError)
        XCTAssertFalse(store.isLoading)
    }

    @MainActor
    func testSessionListRepeatedCursorDoesNotPublishPartialPages() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try sessionListStore()
        defer { session.invalidateAndCancel() }
        let path = "/v1/sessions"
        var load: Task<Void, Never>!
        await waitForRunRequest { load = Task { await store.loadSessions() } }
        let second = expectation(description: "repeated cursor page")
        RunStatsURLProtocol.onRequest { second.fulfill() }
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"partial"}],"next_cursor":"same"}"#)
        await fulfillment(of: [second], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"another"}],"next_cursor":"same"}"#)
        await load.value
        XCTAssertTrue(store.sessions.isEmpty)
        XCTAssertNotNil(store.loadError)
        XCTAssertFalse(store.isLoading)
        XCTAssertEqual(RunStatsURLProtocol.count(path), 2)
    }

    @MainActor
    func testSessionListToggleRejectsOlderActiveResponse() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try sessionListStore()
        defer { session.invalidateAndCancel() }
        let path = "/v1/sessions"
        var active: Task<Void, Never>!
        await waitForRunRequest { active = Task { await store.loadSessions() } }
        var archived: Task<Void, Never>!
        await waitForRunRequest { archived = Task { await store.toggleArchivedView() } }
        XCTAssertTrue(store.showArchived)
        XCTAssertNil(RunStatsURLProtocol.query(path)["archived"])
        XCTAssertEqual(RunStatsURLProtocol.query(path, occurrence: 1)["archived"], "1")
        RunStatsURLProtocol.complete(path, occurrence: 1,
                                     body: #"{"sessions":[{"id":"archived"}],"next_cursor":null}"#)
        await archived.value
        RunStatsURLProtocol.complete(path,
                                     body: #"{"sessions":[{"id":"active"}],"next_cursor":null}"#)
        await active.value
        XCTAssertTrue(store.showArchived)
        XCTAssertEqual(store.sessions.map(\.id), ["archived"])
        XCTAssertNil(store.loadError)
        XCTAssertFalse(store.isLoading)
    }

    @MainActor
    func testSessionListRefreshDuringPendingLoadRequestsAnotherFetch() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try sessionListStore()
        defer { session.invalidateAndCancel() }
        let path = "/v1/sessions"
        let first = expectation(description: "first refresh request")
        RunStatsURLProtocol.onRequest { first.fulfill() }
        store.scheduleSessionsRefresh()
        await fulfillment(of: [first], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
        store.scheduleSessionsRefresh()
        let second = expectation(description: "follow-up refresh request")
        RunStatsURLProtocol.onRequest { second.fulfill() }
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"old"}],"next_cursor":""}"#)
        await fulfillment(of: [second], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
        RunStatsURLProtocol.complete(path, body: #"{"sessions":[{"id":"new"}],"next_cursor":""}"#)
        // Wait for the second response to be applied before inspecting the list.
        let applied = expectation(description: "follow-up response applied")
        Task {
            while store.isLoading {
                await Task.yield()
            }
            applied.fulfill()
        }
        await fulfillment(of: [applied], timeout: 5)
        XCTAssertEqual(store.sessions.map(\.id), ["new"])
        XCTAssertEqual(RunStatsURLProtocol.count(path), 2)
        store.stop()
    }

    @MainActor
    func testSessionListStopRejectsPendingResponse() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try sessionListStore()
        defer { session.invalidateAndCancel() }
        let path = "/v1/sessions"
        var load: Task<Void, Never>!
        await waitForRunRequest { load = Task { await store.loadSessions() } }
        store.stop()
        RunStatsURLProtocol.complete(path,
                                     body: #"{"sessions":[{"id":"late"}],"next_cursor":null}"#)
        await load.value
        XCTAssertTrue(store.sessions.isEmpty)
        XCTAssertNil(store.loadError)
        await store.loadSessions()
        XCTAssertEqual(RunStatsURLProtocol.count(path), 1)
    }

    @MainActor
    private func runStatsStore() throws -> (SessionStore, URLSession) {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [RunStatsURLProtocol.self]
        let session = URLSession(configuration: config)
        return try (SessionStore(sessionId: "s", api: APIClient(
            baseURL: XCTUnwrap(URL(string: "http://localhost")), token: "", session: session,
        )), session)
    }

    @MainActor
    private func waitForRunRequest(_ action: () -> Void) async {
        let arrived = expectation(description: "run stats HTTP request")
        RunStatsURLProtocol.onRequest { arrived.fulfill() }
        action()
        await fulfillment(of: [arrived], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
    }

    @MainActor
    func testFailedApprovalRestoresPendingAfterConcurrentSnapshot() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try runStatsStore()
        defer { session.invalidateAndCancel() }
        let snapshotPath = "/v1/sessions/s/snapshot"
        let approvalPath = "/v1/sessions/s/approvals/original"
        let original = #"{"kind":"approval","id":"original","approval":{"approval_id":"original","call_id":"call","tool_name":"run_cmd","risk":2,"description":"original","args_hash":"hash"}}"#
        let other = #"{"kind":"approval","id":"other","approval":{"approval_id":"other","call_id":"call2","tool_name":"edit_file","risk":1,"description":"other","args_hash":"hash2"}}"#

        var initial: Task<Void, Never>!
        await waitForRunRequest { initial = Task { await store.refresh() } }
        RunStatsURLProtocol.complete(snapshotPath, body: #"{"state":"awaiting_approval","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"pending_requests":[\#(original)]}"#)
        await initial.value
        let approval = try XCTUnwrap(store.pendingApprovals.first)
        XCTAssertEqual(approval.approvalId, "original")

        var resolving: Task<Void, Never>!
        await waitForRunRequest {
            resolving = Task { await store.resolveApproval(approval, decision: .allow) }
        }
        XCTAssertTrue(store.pendingApprovals.isEmpty)
        var concurrent: Task<Void, Never>!
        await waitForRunRequest { concurrent = Task { await store.refresh() } }
        RunStatsURLProtocol.complete(snapshotPath, body: #"{"state":"awaiting_approval","session_id":"s","model_name":"test","turn_count":1,"event_seq":2,"pending_requests":[\#(other)]}"#)
        await concurrent.value
        XCTAssertEqual(store.pendingApprovals.map(\.approvalId), ["other"])

        let retrySnapshot = expectation(description: "approval failure triggers refresh")
        RunStatsURLProtocol.onRequest { retrySnapshot.fulfill() }
        RunStatsURLProtocol.complete(approvalPath,
                                     body: #"{"error":{"code":"binding_mismatch","message":"stale binding"}}"#,
                                     status: 409)
        await fulfillment(of: [retrySnapshot], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
        XCTAssertEqual(Set(store.pendingApprovals.map(\.approvalId)), ["original", "other"])
        RunStatsURLProtocol.complete(snapshotPath,
                                     body: #"{"error":{"code":"unavailable","message":"offline"}}"#,
                                     status: 503)
        await resolving.value
        XCTAssertEqual(Set(store.pendingApprovals.map(\.approvalId)), ["original", "other"])
        XCTAssertEqual(RunStatsURLProtocol.count(approvalPath), 1)
        XCTAssertEqual(RunStatsURLProtocol.count(snapshotPath), 3)
    }

    @MainActor
    func testFailedQuestionRestoresPendingAfterConcurrentSnapshot() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try runStatsStore()
        defer { session.invalidateAndCancel() }
        let snapshotPath = "/v1/sessions/s/snapshot"
        let questionPath = "/v1/sessions/s/questions/original"
        let original = #"{"kind":"question","id":"original","question":{"id":"original","text":"Choose?","options":[]}}"#
        let other = #"{"kind":"question","id":"other","question":{"id":"other","text":"Other?","options":[]}}"#

        var initial: Task<Void, Never>!
        await waitForRunRequest { initial = Task { await store.refresh() } }
        RunStatsURLProtocol.complete(snapshotPath, body: #"{"state":"running","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"pending_requests":[\#(original)]}"#)
        await initial.value
        let question = try XCTUnwrap(store.pendingQuestions.first)
        XCTAssertEqual(question.id, "original")

        var answering: Task<Void, Never>!
        await waitForRunRequest {
            answering = Task { await store.answerQuestion(question, selected: [], customText: nil, skipped: true) }
        }
        XCTAssertTrue(store.pendingQuestions.isEmpty)
        var concurrent: Task<Void, Never>!
        await waitForRunRequest { concurrent = Task { await store.refresh() } }
        RunStatsURLProtocol.complete(snapshotPath, body: #"{"state":"running","session_id":"s","model_name":"test","turn_count":1,"event_seq":2,"pending_requests":[\#(other)]}"#)
        await concurrent.value
        XCTAssertEqual(store.pendingQuestions.map(\.id), ["other"])

        let retrySnapshot = expectation(description: "question failure triggers refresh")
        RunStatsURLProtocol.onRequest { retrySnapshot.fulfill() }
        RunStatsURLProtocol.complete(questionPath,
                                     body: #"{"error":{"code":"binding_mismatch","message":"stale question"}}"#,
                                     status: 409)
        await fulfillment(of: [retrySnapshot], timeout: 5)
        RunStatsURLProtocol.onRequest(nil)
        XCTAssertEqual(Set(store.pendingQuestions.map(\.id)), ["original", "other"])
        RunStatsURLProtocol.complete(snapshotPath,
                                     body: #"{"error":{"code":"unavailable","message":"offline"}}"#,
                                     status: 503)
        await answering.value
        XCTAssertEqual(Set(store.pendingQuestions.map(\.id)), ["original", "other"])
        XCTAssertEqual(RunStatsURLProtocol.count(questionPath), 1)
        XCTAssertEqual(RunStatsURLProtocol.count(snapshotPath), 3)
    }

    @MainActor
    func testEmptyAndFailedRunStatsAreCachedUntilForcedRefresh() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try runStatsStore()
        defer { session.invalidateAndCancel() }
        let path = "/runs/empty/changes"
        var first: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { first = Task { await store.runStats(runId: "empty") } }
        RunStatsURLProtocol.complete(path, body: #"{"entries":[]}"#)
        let firstResult = await first.value
        let cachedEmpty = await store.runStats(runId: "empty")
        XCTAssertNil(firstResult)
        XCTAssertNil(cachedEmpty)
        XCTAssertEqual(RunStatsURLProtocol.count(path), 1)

        var forced: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { forced = Task { await store.runStats(runId: "empty", forceRefresh: true) } }
        RunStatsURLProtocol.complete(path, body: #"{"error":{"code":"not_found","message":"missing"}}"#, status: 404)
        let forcedResult = await forced.value
        let cachedFailure = await store.runStats(runId: "empty")
        XCTAssertNil(forcedResult)
        XCTAssertNil(cachedFailure)
        XCTAssertEqual(RunStatsURLProtocol.count(path), 2)
    }

    @MainActor
    func testRevertInvalidatesAllRunStatsAndRejectsOlderResponses() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try runStatsStore()
        defer { session.invalidateAndCancel() }
        let a = "/runs/a/changes"
        let b = "/runs/b/changes"
        let old = #"{"entries":[{"path":"old","before_size":1,"after_size":2,"added":1,"removed":0}]}"#
        let fresh = #"{"entries":[{"path":"fresh","before_size":1,"after_size":1,"added":0,"removed":0}]}"#
        var cached: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { cached = Task { await store.runStats(runId: "b") } }
        RunStatsURLProtocol.complete(b, body: old)
        let cachedResult = await cached.value
        XCTAssertEqual(cachedResult?.first?.path, "old")

        var stale: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { stale = Task { await store.runStats(runId: "a") } }
        var revert: Task<(note: String, warn: Bool), Never>!
        await waitForRunRequest { revert = Task { await store.revertRun(runId: "a") } }
        RunStatsURLProtocol.complete("/runs/a/revert", body: #"{"restored":[],"deleted":[]}"#)
        let reverted = await revert.value
        XCTAssertFalse(reverted.warn)
        RunStatsURLProtocol.complete(a, body: old)
        let staleResult = await stale.value
        XCTAssertNil(staleResult)

        var refetched: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { refetched = Task { await store.runStats(runId: "b") } }
        RunStatsURLProtocol.complete(b, body: fresh)
        let refetchedResult = await refetched.value
        XCTAssertEqual(refetchedResult?.first?.path, "fresh")
        var after: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { after = Task { await store.runStats(runId: "a") } }
        RunStatsURLProtocol.complete(a, body: fresh)
        let afterResult = await after.value
        XCTAssertEqual(afterResult?.first?.path, "fresh")
        XCTAssertEqual(RunStatsURLProtocol.count(b), 2)
        XCTAssertEqual(RunStatsURLProtocol.count(a), 2)
    }

    @MainActor
    func testOlderForcedRefreshCannotOverwriteNewerRunStats() async throws {
        RunStatsURLProtocol.reset()
        defer { RunStatsURLProtocol.reset() }
        let (store, session) = try runStatsStore()
        defer { session.invalidateAndCancel() }
        let path = "/runs/a/changes"
        var older: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { older = Task { await store.runStats(runId: "a") } }
        var newer: Task<[RunFileStat]?, Never>!
        await waitForRunRequest { newer = Task { await store.runStats(runId: "a", forceRefresh: true) } }
        RunStatsURLProtocol.complete(path, occurrence: 1, body: #"{"entries":[{"path":"new","before_size":0,"after_size":1,"added":1,"removed":0}]}"#)
        let newerResult = await newer.value
        XCTAssertEqual(newerResult?.first?.path, "new")
        RunStatsURLProtocol.complete(path, body: #"{"entries":[{"path":"old","before_size":0,"after_size":1,"added":1,"removed":0}]}"#)
        let olderResult = await older.value
        let cachedResult = await store.runStats(runId: "a")
        XCTAssertNil(olderResult)
        XCTAssertEqual(cachedResult?.first?.path, "new")
        XCTAssertEqual(RunStatsURLProtocol.count(path), 2)
    }

    func testMazeProjectionDecodesMainDetoursAndNullableToolEnd() throws {
        let maze = try decode(MazeData.self, """
        {
          "tmax": 48.5,
          "lanes": [{
            "key": "main", "session_id": "session-1", "model": "model-a",
            "stats": {"steps": 2, "tools": 2, "rz": 1, "in_tok": 12,
                      "rz_tok": 3, "out_tok": 8, "t": 48.5, "main": 1, "detours": 1},
            "main": [{"step": 1, "turn": 1, "s": 0, "e": 10, "tools": [],
                       "rz": 0, "v": "answer", "live": false}],
            "detours": [{"step": 2, "turn": 1, "s": 11, "e": 48.5,
                         "rz": 1, "rz_txt": "Thinking", "rz_ms": 300,
                         "v": "retry", "why": "Repeated call", "attach": 1,
                         "sub": true, "label": "Delegate", "retries": 2,
                         "tools": [{"name": "delegate_task", "args": "inspect",
                                    "s": 12, "e": null, "dur": 0,
                                    "res": "", "v": "pending", "call_id": "call-1",
                                    "child_id": "child-1"}]}]
          }]
        }
        """)
        let lane = try XCTUnwrap(maze.lanes.first)
        XCTAssertEqual(lane.sessionId, "session-1")
        XCTAssertEqual(lane.stats.inTok, 12)
        XCTAssertEqual(lane.main.first?.v, .answer)
        let branch = try XCTUnwrap(lane.detours.first)
        XCTAssertEqual(branch.attach, 1)
        XCTAssertEqual(branch.rzTxt, "Thinking")
        XCTAssertEqual(branch.v, .retry)
        XCTAssertEqual(branch.tools.first?.v, .pending)
        XCTAssertNil(branch.tools.first?.e)
        XCTAssertEqual(branch.tools.first?.childId, "child-1")
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
