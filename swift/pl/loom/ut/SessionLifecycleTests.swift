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
import Observation
import XCTest

/// Real REST decoding with controllable snapshot failures and delayed replies.
private final class LifecycleURLProtocol: URLProtocol {
    private final class State: @unchecked Sendable {
        let lock = NSLock()
        var hits = 0
        var status = 200
        var body = LifecycleURLProtocol.snapshot
        var heldHits: Set<Int> = []
        var pending: [Int: LifecycleURLProtocol] = [:]
    }

    private static let state = State()

    static var snapshotHits: Int {
        state.lock.withLock { state.hits }
    }

    static func respond(status: Int = 200, body: Data = snapshot) {
        state.lock.withLock {
            state.status = status
            state.body = body
        }
    }

    static func hold(_ hits: Set<Int>) {
        state.lock.withLock { state.heldHits = hits }
    }

    static func release(_ hit: Int, status: Int = 200, body: Data = snapshot) {
        let pending = state.lock.withLock { state.pending.removeValue(forKey: hit) }
        pending?.finish(status, body)
    }

    static let snapshot = Data((
        #"{"state":"idle","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"# +
            #""messages":[{"id":"a1","role":"assistant","status":"final","parts":[{"kind":"text","text":"done"}]}]}"#
    ).utf8)

    static func reset() {
        state.lock.withLock {
            state.hits = 0
            state.status = 200
            state.body = snapshot
            state.heldHits = []
            state.pending = [:]
        }
    }

    override class func canInit(with _: URLRequest) -> Bool {
        true
    }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest {
        request
    }

    override func startLoading() {
        let path = request.url?.path ?? ""
        if path.hasSuffix("/snapshot") {
            let reply: (Int, Data)? = Self.state.lock.withLock {
                Self.state.hits += 1
                let hit = Self.state.hits
                if Self.state.heldHits.contains(hit) {
                    Self.state.pending[hit] = self
                    return nil
                }
                return (Self.state.status, Self.state.body)
            }
            if let reply {
                finish(reply.0, reply.1)
            }
        } else {
            finish(404, Data(#"{"error":{"code":"not_found","message":"n/a"}}"#.utf8))
        }
    }

    private func finish(_ status: Int, _ body: Data) {
        let response = HTTPURLResponse(
            url: request.url!, statusCode: status, httpVersion: nil, headerFields: nil,
        )!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: body)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

/// Only the transport is simulated; frames run through the production loop.
private final class LifecycleFrames: @unchecked Sendable {
    typealias Stream = AsyncThrowingStream<SSEFrame, Error>
    private let lock = NSLock()
    private var continuations: [Stream.Continuation] = []
    private var cursors: [UInt64] = []
    private var terminationCount = 0

    var attachments: [UInt64] {
        lock.withLock { cursors }
    }

    var terminations: Int {
        lock.withLock { terminationCount }
    }

    func frames(_: String, after: UInt64) -> Stream {
        let (stream, continuation) = Stream.makeStream()
        continuation.onTermination = { [weak self] _ in
            guard let self else { return }
            lock.withLock { self.terminationCount += 1 }
        }
        lock.withLock {
            cursors.append(after)
            continuations.append(continuation)
        }
        return stream
    }

    @discardableResult
    func yield(_ frame: SSEFrame, on index: Int = 0) -> Stream.Continuation.YieldResult {
        let continuation = lock.withLock { continuations[index] }
        return continuation.yield(frame)
    }
}

@MainActor
final class SessionLifecycleTests: XCTestCase {
    private func makeAPI() throws -> (APIClient, URLSession) {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [LifecycleURLProtocol.self]
        let session = URLSession(configuration: config)
        let api = try APIClient(
            baseURL: XCTUnwrap(URL(string: "http://127.0.0.1:9")), token: "", session: session,
        )
        return (api, session)
    }

    private func waitFor(
        _ timeout: TimeInterval = 5,
        _ condition: @MainActor () -> Bool,
    ) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() {
                return
            }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTFail("condition not met within \(timeout)s")
        throw NSError(domain: "SessionLifecycleTests.timeout", code: 1)
    }

    override func setUp() {
        LifecycleURLProtocol.reset()
    }

    private func makeStore(
        _ api: APIClient, frames: LifecycleFrames,
        watchdogTimeout: Duration = .seconds(45), watchdogPollInterval: Duration = .seconds(5),
    ) -> SessionStore {
        SessionStore(
            sessionId: "s", api: api,
            eventFrames: { id, after in frames.frames(id, after: after) },
            watchdogTimeout: watchdogTimeout, watchdogPollInterval: watchdogPollInterval,
        )
    }

    private func event(
        _ sequence: UInt64, _ kind: String, _ payload: String = "{}", runId: String? = nil,
    ) -> SSEFrame {
        let run = runId.map { ",\"run_id\":\"\($0)\"" } ?? ""
        return SSEFrame(content: .event(
            id: sequence, event: kind,
            data: """
            {"version":1,"sequence":\(sequence),"session_id":"s","kind":"\(kind)","durable":true,"payload":\(payload)\(run)}
            """,
        ))
    }

    private let connected = SSEFrame(content: .comment("connected, instance=one"))
    private let snapshotFailure = Data(#"{"error":{"code":"unavailable","message":"snapshot unavailable"}}"#.utf8)

    private func snapshot(sequence: UInt64, state: String = "idle", nextPrompt: Bool = false) -> Data {
        let user = nextPrompt
            ? #",{"id":"u2","role":"user","status":"final","parts":[{"kind":"text","text":"next"}]}"#
            : ""
        return Data("""
        {"state":"\(state)","session_id":"s","model_name":"test","turn_count":2,"event_seq":\(sequence),
        "messages":[{"id":"a1","role":"assistant","status":"final","parts":[{"kind":"text","text":"canonical"}]}\(user)]}
        """.utf8)
    }

    private func processSnapshot(state: String, sequence: Int, closure: Bool, runId: String? = nil) -> Data {
        let run = runId.map { ",\"run_id\":\"\($0)\"" } ?? ""
        return Data("""
        {"state":"\(state)","session_id":"s","model_name":"test","turn_count":1,"event_seq":\(sequence)\(run),
        "messages":[
        {"id":"work","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"working"}]},
        {"id":"final","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"answer"}]}]
        \(closure ? ",\"turn_closures\":[{\"run_id\":\"r\",\"outcome\":\"succeeded\",\"final_answer_message_ids\":[\"final\"]}]" : "")}
        """.utf8)
    }

    private func receiptSnapshot(sequence: Int, closures: String) -> Data {
        Data("""
        {"state":"idle","session_id":"s","model_name":"test","turn_count":2,"event_seq":\(sequence),
        "messages":[
        {"id":"work","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"working"}]},
        {"id":"final","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"answer"}]},
        {"id":"work2","role":"assistant","status":"final","metadata":{"run_id":"r2"},"parts":[{"kind":"text","text":"working again"}]},
        {"id":"final2","role":"assistant","status":"final","metadata":{"run_id":"r2"},"parts":[{"kind":"text","text":"another answer"}]}],
        "turn_closures":[\(closures)]}
        """.utf8)
    }

    func testResolvedReceiptsFoldOnlyAfterTrustedCompletionAndRestoreUnchanged() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        store.setTranscriptFollowingBottom(true)
        frames.yield(event(2, "approval.resolved", #"{"approval_id":"allowed","decision":"allow","actor":"reviewer"}"#, runId: "r"))
        frames.yield(event(3, "question.answered", #"{"question_id":"answered","skipped":false}"#, runId: "r"))
        frames.yield(event(4, "runtime.warning", #"{"message":"keep this warning"}"#, runId: "r"))
        try await waitFor { store.resolvedReceipts.count == 2 && store.notices.count == 1 }
        let originals = store.resolvedReceipts
        XCTAssertTrue(store.isBusy)
        XCTAssertTrue(store.transcript.processes.isEmpty)
        XCTAssertEqual(originals.map(\.id), ["allowed", "answered"])
        XCTAssertEqual(originals.map(\.runId), ["r", "r"])
        XCTAssertEqual(originals.map(\.kind), [
            .approval(allowed: true, actor: "reviewer"), .question(skipped: false, summary: nil),
        ])
        XCTAssertEqual(store.visibleResolvedReceipts, originals)

        // A later echo must not rewrite the original id, outcome, or run ownership.
        frames.yield(event(5, "approval.resolved", #"{"approval_id":"allowed","decision":"deny"}"#, runId: "r2"))
        LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 6, closure: true))
        frames.yield(event(6, "turn.finished", runId: "r"))
        try await waitFor { store.transcript.processes["r"] != nil }
        XCTAssertFalse(store.processState.expandedRuns.contains("r"))
        XCTAssertTrue(store.visibleResolvedReceipts.isEmpty)
        XCTAssertEqual(store.resolvedReceipts, originals)
        XCTAssertEqual(store.notices, ["keep this warning"])

        store.setProcessExpanded(runId: "r", expanded: true)
        XCTAssertEqual(store.visibleResolvedReceipts, originals)
        await store.refresh()
        XCTAssertEqual(store.resolvedReceipts, originals)
        XCTAssertEqual(store.visibleResolvedReceipts, originals)
        store.setProcessExpanded(runId: "r", expanded: false)
        XCTAssertTrue(store.visibleResolvedReceipts.isEmpty)
        XCTAssertEqual(store.resolvedReceipts, originals)
    }

    func testDeniedSkippedUnownedAndOtherRunReceiptsStayConservative() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        store.setTranscriptFollowingBottom(true)
        frames.yield(event(2, "approval.resolved", #"{"approval_id":"denied","decision":"deny"}"#, runId: "r"))
        frames.yield(event(3, "question.answered", #"{"question_id":"skipped","skipped":true}"#, runId: "r"))
        frames.yield(event(4, "approval.resolved", #"{"approval_id":"unowned","decision":"allow"}"#))
        frames.yield(event(5, "question.answered", #"{"question_id":"unowned-answer","skipped":false}"#))
        frames.yield(event(6, "approval.resolved", #"{"approval_id":"other-run","decision":"allow"}"#, runId: "r2"))
        frames.yield(event(7, "question.answered", #"{"question_id":"this-run","skipped":false}"#, runId: "r"))
        try await waitFor { store.resolvedReceipts.count == 6 }
        let originals = store.resolvedReceipts
        XCTAssertNil(originals[2].runId)
        XCTAssertNil(originals[3].runId)
        XCTAssertEqual(store.visibleResolvedReceipts, originals)

        let firstClosure = #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}"#
        LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 8, closures: firstClosure))
        frames.yield(event(8, "turn.finished", runId: "r"))
        try await waitFor { store.transcript.processes["r"] != nil }
        XCTAssertNil(store.transcript.processes["r2"])
        XCTAssertEqual(store.visibleResolvedReceipts, Array(originals.prefix(5)))
        store.setProcessExpanded(runId: "r", expanded: true)
        XCTAssertEqual(store.visibleResolvedReceipts, originals)
        store.setProcessExpanded(runId: "r", expanded: false)

        let secondClosure = #"{"run_id":"r2","outcome":"succeeded","final_answer_message_ids":["final2"]}"#
        LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 8, closures: firstClosure + "," + secondClosure))
        await store.refresh()
        XCTAssertNotNil(store.transcript.processes["r2"])
        XCTAssertEqual(store.visibleResolvedReceipts, Array(originals.prefix(4)))
        store.setProcessExpanded(runId: "r2", expanded: true)
        XCTAssertEqual(store.visibleResolvedReceipts, Array(originals.prefix(5)))
        XCTAssertFalse(store.processState.expandedRuns.contains("r"))
        store.setProcessExpanded(runId: "r", expanded: true)
        XCTAssertEqual(store.visibleResolvedReceipts, originals)
        XCTAssertEqual(store.resolvedReceipts, originals)
    }

    func testInvalidOrFailedClosureCannotHideLocalReceiptsOnSnapshotRefresh() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(event(2, "approval.resolved", #"{"approval_id":"allowed","decision":"allow"}"#, runId: "r"))
        frames.yield(event(3, "question.answered", #"{"question_id":"answered","skipped":false}"#, runId: "r"))
        try await waitFor { store.resolvedReceipts.count == 2 }
        let originals = store.resolvedReceipts
        let valid = #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}"#
        LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 3, closures: valid))
        await store.refresh()
        XCTAssertNotNil(store.transcript.processes["r"])
        XCTAssertTrue(store.visibleResolvedReceipts.isEmpty)

        let invalidClosures = [
            "",
            #"{"run_id":"r","outcome":"failed","final_answer_message_ids":["final"]}"#,
            #"{"run_id":"r","outcome":"cancelled","final_answer_message_ids":["final"]}"#,
            #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["missing"]}"#,
            #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final2"]}"#,
            #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":[]}"#,
            valid + "," + valid,
        ]
        for closures in invalidClosures {
            LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 3, closures: closures))
            await store.refresh()
            XCTAssertNil(store.transcript.processes["r"], closures)
            XCTAssertEqual(store.visibleResolvedReceipts, originals, closures)
            XCTAssertEqual(store.resolvedReceipts, originals, closures)
        }
    }

    func testStoreClosureTransitionRepeatedRevealAndSessionIsolation() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
        await store.refresh()
        store.setTranscriptFollowingBottom(true)
        LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 2, closure: true))
        await store.refresh()
        XCTAssertEqual(store.turnClosures.count, 1)
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "final"])
        store.setProcessExpanded(runId: "r", expanded: true)
        store.pause()
        await store.refresh()
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "work", "final"])
        let other = SessionStore(sessionId: "other", api: api)
        other.setProcessExpanded(runId: "r", expanded: false)
        XCTAssertTrue(store.processState.expandedRuns.contains("r"))
        XCTAssertFalse(other.processState.expandedRuns.contains("r"))
        other.stop()
    }

    func testPreviousRunStartEnvelopeBindsOnlyRealProgressAndTransfersSelectionHold() async throws {
        for (startRun, firstProgress) in [("r", "model.request_started"), ("r", "tool.progress"),
                                          ("", "model.request_started")]
        {
            LifecycleURLProtocol.reset()
            let (api, session) = try makeAPI()
            defer { session.invalidateAndCancel() }
            let frames = LifecycleFrames()
            let store = makeStore(api, frames: frames)
            defer { store.stop() }
            LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 1, closure: true))
            store.start()
            try await waitFor { frames.attachments.count == 1 }
            store.setTranscriptFollowingBottom(true)
            frames.yield(event(2, "turn.started", #"{"prompt":"next","turn_index":2}"#, runId: startRun))
            try await waitFor { store.draft?.turnIndex == 2 }
            XCTAssertNil(store.activeRunId)
            XCTAssertFalse(store.processState.observedRuns.contains("r"))
            store.holdCurrentProcessOpen()
            XCTAssertTrue(store.processState.holdOpen)
            // A high-sequence historical delta must neither bind nor mutate the draft.
            frames.yield(event(3, "model.text_delta", #"{"request_id":"old","delta":"stale"}"#, runId: "r"))
            frames.yield(event(4, firstProgress, #"{"request_id":"new"}"#, runId: "r2"))
            try await waitFor { store.activeRunId == "r2" }
            XCTAssertTrue(store.draft?.isEmpty == true)
            XCTAssertTrue(store.processState.observedRuns.contains("r2"))
            XCTAssertFalse(store.processState.observedRuns.contains("r"))
            XCTAssertTrue(store.processState.holdOpen)
            let closures = #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]},{"run_id":"r2","outcome":"succeeded","final_answer_message_ids":["final2"]}"#
            LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 5, closures: closures))
            frames.yield(event(5, "turn.finished", runId: "r2"))
            try await waitFor { store.draft == nil && store.transcript.processes["r2"] != nil }
            XCTAssertNil(store.activeRunId)
            XCTAssertFalse(store.isBusy)
            XCTAssertTrue(store.processState.expandedRuns.contains("r2"))
            XCTAssertFalse(store.processState.expandedRuns.contains("r"))
        }
    }

    func testClosedCurrentRunningSnapshotAcceptsProgressAndNormalFinish() async throws {
        for previouslyFinished in [false, true] {
            for outcome in ["succeeded", "failed"] {
                LifecycleURLProtocol.reset()
                let (api, session) = try makeAPI()
                defer { session.invalidateAndCancel() }
                let frames = LifecycleFrames()
                let store = makeStore(api, frames: frames)
                defer { store.stop() }
                LifecycleURLProtocol.respond(body: processSnapshot(
                    state: "running", sequence: 1, closure: true, runId: "r",
                ))
                store.start()
                try await waitFor { frames.attachments.count == 1 }
                let original = #"{"closure":{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}}"#
                if previouslyFinished {
                    // Populate finishedRuns through an actual live finish, then
                    // reconcile a busy projection of the same authoritative run.
                    LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 2, closure: true))
                    frames.yield(event(2, "turn.finished", original, runId: "r"))
                    try await waitFor { !store.isBusy && LifecycleURLProtocol.snapshotHits == 2 && store.draft == nil }
                    LifecycleURLProtocol.respond(body: processSnapshot(
                        state: "running", sequence: 3, closure: true, runId: "r",
                    ))
                    await store.refresh()
                }
                XCTAssertTrue(store.isBusy)
                XCTAssertEqual(store.activeRunId, "r")
                XCTAssertEqual(store.turnClosures.count, 1)
                store.setTranscriptFollowingBottom(true)
                store.holdCurrentProcessOpen()
                let generation = store.activityGeneration
                frames.yield(event(4, "model.request_started", #"{"request_id":"current"}"#, runId: "r"))
                frames.yield(event(5, "model.text_delta", #"{"request_id":"current","delta":"tail"}"#, runId: "r"))
                frames.yield(event(6, "tool.progress", #"{"call_id":"current-tool"}"#, runId: "r"))
                frames.yield(event(7, "runtime.warning", #"{"message":"progress barrier"}"#))
                try await waitFor { store.notices.contains("progress barrier") }
                XCTAssertEqual(store.activityGeneration, generation + 1)
                XCTAssertTrue(store.processState.holdOpen)
                XCTAssertTrue(store.processState.observedRuns.contains("r"))
                XCTAssertEqual(store.activeRunId, "r")
                guard case let .text(text) = store.draft?.segments.first else {
                    return XCTFail("current closed run's progress must remain live")
                }
                XCTAssertTrue(text.live)
                XCTAssertEqual(text.text, "tail")

                // Hold reconciliation to observe the finish itself, not an idle
                // snapshot masking a rejected event or an unsealed draft.
                let finishHit = LifecycleURLProtocol.snapshotHits + 1
                LifecycleURLProtocol.hold([finishHit])
                let finish = """
                {"closure":{"run_id":"r","outcome":"\(outcome)","final_answer_message_ids":["final"]}}
                """
                frames.yield(event(8, "turn.finished", finish, runId: "r"))
                try await waitFor { LifecycleURLProtocol.snapshotHits == finishHit }
                XCTAssertFalse(store.isBusy)
                XCTAssertNil(store.activeRunId)
                XCTAssertEqual(store.activityGeneration, generation + 2)
                guard case let .text(sealed) = store.draft?.segments.first else {
                    return XCTFail("finish must seal the draft before reconciliation")
                }
                XCTAssertFalse(sealed.live)
                XCTAssertEqual(sealed.text, "tail")
                LifecycleURLProtocol.release(finishHit, body: processSnapshot(state: "idle", sequence: 8, closure: true))
                try await waitFor { store.draft == nil }
                XCTAssertEqual(store.turnClosures.count, outcome == "failed" ? 2 : 1)
                XCTAssertEqual(store.transcript.processes["r"] == nil, outcome == "failed")

                // Duplicate finish must not reconcile again; conflicting evidence
                // from either finish path must survive a canonical snapshot.
                let conflict = #"{"closure":{"run_id":"r","outcome":"failed","final_answer_message_ids":["final"]}}"#
                let finishedGeneration = store.activityGeneration
                frames.yield(event(9, "turn.finished", finish, runId: "r"))
                frames.yield(event(10, "turn.finished", conflict, runId: "r"))
                frames.yield(event(11, "runtime.warning", #"{"message":"finish barrier"}"#))
                try await waitFor { store.notices.contains("finish barrier") }
                XCTAssertFalse(store.isBusy)
                XCTAssertNil(store.activeRunId)
                XCTAssertNil(store.draft)
                XCTAssertEqual(store.activityGeneration, finishedGeneration)
                XCTAssertEqual(LifecycleURLProtocol.snapshotHits, finishHit)
                XCTAssertEqual(store.turnClosures.count, 2)
                XCTAssertNil(store.transcript.processes["r"])
                LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 11, closure: true))
                await store.refresh()
                XCTAssertEqual(store.turnClosures.count, 2)
                XCTAssertNil(store.transcript.processes["r"])
            }
        }
    }

    func testOldSnapshotClosureFinishReplayPreservesKnownAndUnknownActivity() async throws {
        for identity in ["unknown", "progress", "snapshot"] {
            let knownRun = identity != "unknown"
            LifecycleURLProtocol.reset()
            let (api, session) = try makeAPI()
            defer { session.invalidateAndCancel() }
            let frames = LifecycleFrames()
            let store = makeStore(api, frames: frames)
            defer { store.stop() }
            if identity == "snapshot" {
                LifecycleURLProtocol.respond(body: Data("""
                {"state":"running","session_id":"s","run_id":"r2","event_seq":1,"model_name":"fake","turn_count":2,
                "messages":[
                {"id":"work","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"old"}]},
                {"id":"final","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"answer"}]}],
                "turn_closures":[{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}]}
                """.utf8))
            } else {
                LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 1, closure: true))
            }
            store.start()
            try await waitFor { frames.attachments.count == 1 }
            if identity != "snapshot" {
                frames.yield(event(2, "turn.started", #"{"prompt":"next"}"#, runId: "r"))
            }
            frames.yield(event(3, "model.text_delta", #"{"request_id":"new","delta":"fresh"}"#, runId: knownRun ? "r2" : nil))
            try await waitFor { store.streamRevision == 1 }
            store.holdCurrentProcessOpen()
            let reading = store.processState
            let generation = store.activityGeneration
            let original = #"{"closure":{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}}"#
            frames.yield(event(4, "turn.finished", original, runId: "r"))
            frames.yield(event(5, "turn.finished", original, runId: "r"))
            frames.yield(event(6, "runtime.warning", #"{"message":"barrier"}"#))
            try await waitFor { store.notices.contains("barrier") }
            XCTAssertTrue(store.isBusy)
            XCTAssertEqual(store.activeRunId, knownRun ? "r2" : nil)
            XCTAssertEqual(store.processState, reading)
            XCTAssertEqual(store.activityGeneration, generation)
            XCTAssertEqual(store.turnClosures.count, 1)
            XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 1)
            XCTAssertEqual(store.draft?.segments.count, 1)
            guard case let .text(text) = store.draft?.segments.first else {
                return XCTFail("expected live new draft")
            }
            XCTAssertTrue(text.live)
            XCTAssertEqual(text.text, "fresh")
        }
    }

    func testPreviousUnclosedEnvelopeAndMismatchedFinishCannotCloseNextTurn() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 1, closure: false))
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(event(2, "turn.started", #"{"prompt":"next"}"#, runId: "r"))
        frames.yield(event(3, "turn.finished", runId: "r"))
        frames.yield(event(4, "model.request_started", #"{"request_id":"new"}"#, runId: "r2"))
        frames.yield(event(5, "model.text_delta", #"{"request_id":"new","delta":"fresh"}"#, runId: "r2"))
        // This terminal arrives while the new text is still buffered.
        frames.yield(event(6, "turn.finished", runId: "unrelated"))
        frames.yield(event(7, "runtime.warning", #"{"message":"barrier"}"#))
        try await waitFor { store.notices.contains("barrier") }
        XCTAssertTrue(store.isBusy)
        XCTAssertEqual(store.activeRunId, "r2")
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 1)
        guard case let .text(text) = store.draft?.segments.first else {
            return XCTFail("expected live draft")
        }
        XCTAssertTrue(text.live)
        XCTAssertEqual(text.text, "fresh")
    }

    func testConflictingOldFinishKeepsEvidenceVisibleWithoutClosingNewDraft() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        let oldClosure = #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}"#
        LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 1, closures: oldClosure))
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(event(2, "approval.resolved", #"{"approval_id":"old","decision":"allow"}"#, runId: "r"))
        try await waitFor { store.resolvedReceipts.count == 1 }
        XCTAssertTrue(store.visibleResolvedReceipts.isEmpty)
        frames.yield(event(3, "turn.started", #"{"prompt":"new"}"#, runId: "r"))
        frames.yield(event(4, "model.text_delta", #"{"request_id":"new","delta":"live"}"#, runId: "r2"))
        try await waitFor { store.streamRevision == 1 }
        let reading = store.processState
        let conflict = #"{"closure":{"run_id":"r","outcome":"failed","final_answer_message_ids":["final"]}}"#
        frames.yield(event(5, "turn.finished", conflict, runId: "r"))
        frames.yield(event(6, "turn.finished", conflict, runId: "r"))
        frames.yield(event(7, "runtime.warning", #"{"message":"barrier"}"#))
        try await waitFor { store.notices.contains("barrier") }
        XCTAssertNil(store.transcript.processes["r"])
        XCTAssertEqual(store.turnClosures.count, 2)
        XCTAssertTrue(store.projectedRows.contains { $0.id == "work" })
        XCTAssertEqual(store.processState, reading)
        XCTAssertEqual(store.activeRunId, "r2")
        XCTAssertTrue(store.isBusy)
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 1)
        guard case let .text(text) = store.draft?.segments.first else {
            return XCTFail("expected unsealed new draft")
        }
        XCTAssertTrue(text.live)
        // An ordinary later snapshot must not discard the conflict and hide history.
        LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 7, closures: oldClosure))
        await store.refresh()
        XCTAssertNil(store.transcript.processes["r"])
        XCTAssertEqual(store.turnClosures.count, 2)
    }

    func testMissedTurnStartedSnapshotOwnsRunBeforeAssistantAndProtectsReading() async throws {
        for protection in ["away", "input", "none"] {
            let (api, session) = try makeAPI()
            defer { session.invalidateAndCancel() }
            let store = SessionStore(sessionId: "s", api: api)
            defer { store.stop() }
            LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
            await store.refresh()
            XCTAssertEqual(store.activeRunId, "r")
            store.setTranscriptFollowingBottom(true)
            LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 2, closure: true))
            await store.refresh()
            XCTAssertNil(store.activeRunId)
            XCTAssertFalse(store.processState.expandedRuns.contains("r"))

            // No turn.started and no assistant from r2: only completed r history.
            LifecycleURLProtocol.respond(body: Data("""
            {"state":"running","session_id":"s","run_id":"r2","model_name":"test","turn_count":2,"event_seq":3,
            "messages":[
            {"id":"work","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"working"}]},
            {"id":"final","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"answer"}]}],
            "turn_closures":[{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}]}
            """.utf8))
            await store.refresh()
            XCTAssertEqual(store.activeRunId, "r2")
            XCTAssertTrue(store.processState.observedRuns.contains("r2"))
            XCTAssertFalse(store.messages.contains { $0.metadata?["run_id"] == "r2" })
            if protection == "input" {
                store.holdCurrentProcessOpen()
            } else if protection == "away" {
                store.setTranscriptFollowingBottom(false)
            }

            LifecycleURLProtocol.respond(body: receiptSnapshot(sequence: 4, closures:
                #"{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]},{"run_id":"r2","outcome":"succeeded","final_answer_message_ids":["final2"]}"#))
            await store.refresh()
            XCTAssertNil(store.activeRunId)
            XCTAssertEqual(store.processState.expandedRuns.contains("r2"), protection != "none")
            XCTAssertFalse(store.processState.expandedRuns.contains("r"))
            XCTAssertEqual(store.projectedRows.contains { $0.id == "work2" }, protection != "none")
            store.setTranscriptFollowingBottom(true)
            await store.refresh()
            XCTAssertEqual(store.processState.expandedRuns.contains("r2"), protection != "none")
        }
    }

    func testLegacySnapshotDoesNotObserveClosedHistoryAndClearsTerminalIdentity() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
        await store.refresh()
        XCTAssertEqual(store.activeRunId, "r")
        // A legacy busy snapshot now closes r but has no identity for the next run.
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 2, closure: true))
        await store.refresh()
        XCTAssertNil(store.activeRunId)
        let cold = SessionStore(sessionId: "cold", api: api)
        defer { cold.stop() }
        await cold.refresh()
        XCTAssertNil(cold.activeRunId)
        XCTAssertFalse(cold.processState.observedRuns.contains("r"))

        // Missing run_id can still recover an unclosed assistant run.
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 3, closure: false))
        await store.refresh()
        XCTAssertEqual(store.activeRunId, "r")
        for state in ["idle", "closed", "fatal"] {
            LifecycleURLProtocol.respond(body: Data("""
            {"state":"\(state)","session_id":"s","run_id":"r","model_name":"test","turn_count":1,"event_seq":4}
            """.utf8))
            await store.refresh()
            XCTAssertNil(store.activeRunId, state)
        }
    }

    func testConfirmedHistoryColdLoadCollapsesButManualExpansionSurvivesRefresh() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 1, closure: true))
        await store.refresh()
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "final"])
        store.setProcessExpanded(runId: "r", expanded: true)
        store.pause()
        await store.refresh()
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "work", "final"])
        let reopened = SessionStore(sessionId: "s", api: api)
        defer { reopened.stop() }
        await reopened.refresh()
        XCTAssertEqual(reopened.projectedRows.map(\.id), ["process-r", "final"])
    }

    func testSwitchWhileBusyKeepsCompletedProcessOpen() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
        await store.refresh()
        store.setTranscriptFollowingBottom(true)
        store.pause()
        LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 2, closure: true))
        await store.refresh()
        store.setTranscriptFollowingBottom(true)
        await store.refresh()
        XCTAssertTrue(store.processState.expandedRuns.contains("r"))
    }

    func testStoreInputStartDuringBusyPreventsFoldWithoutInputEnd() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: processSnapshot(state: "running", sequence: 1, closure: false))
        await store.refresh()
        store.setTranscriptFollowingBottom(true)
        // The background monitor holds at mouseDown/keyDown. There need not
        // be a tap's onEnded (e.g. the user is still dragging a selection).
        store.holdCurrentProcessOpen()
        LifecycleURLProtocol.respond(body: processSnapshot(state: "idle", sequence: 2, closure: true))
        await store.refresh()
        XCTAssertTrue(store.processState.expandedRuns.contains("r"))
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "work", "final"])
        // Layout/following updates and duplicate snapshots must not undo the
        // input-start hold while the selection is still in progress.
        store.setTranscriptFollowingBottom(true)
        await store.refresh()
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "work", "final"])
        // The hold is not a permanent expansion policy or a disabled toggle.
        store.setProcessExpanded(runId: "r", expanded: false)
        XCTAssertEqual(store.projectedRows.map(\.id), ["process-r", "final"])
    }

    func testRepeatedToolRevealForcesProcessOpenAfterManualCollapse() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: Data(#"{"state":"idle","session_id":"s","model_name":"m","turn_count":1,"event_seq":1,"turn_closures":[{"run_id":"r","outcome":"succeeded","final_answer_message_ids":["final"]}],"messages":[{"id":"tools","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"tool_call","tool_call":{"id":"c","name":"read_file"}},{"kind":"tool_result","tool_result":{"call_id":"c","status":"success","content":[{"kind":"text","text":"ok"}]}}]},{"id":"final","role":"assistant","status":"final","metadata":{"run_id":"r"},"parts":[{"kind":"text","text":"done"}]}]}"#.utf8))
        await store.refresh()
        for sequence: UInt64 in [1, 2] {
            store.setProcessExpanded(runId: "r", expanded: false)
            XCTAssertFalse(store.projectedRows.contains { $0.id == "tools" })
            store.revealTool(ToolRevealRequest(callId: "c", sequence: sequence))
            XCTAssertTrue(store.projectedRows.contains { $0.id == "tools" })
            XCTAssertEqual(store.transcript.toolLocation(callId: "c")?.scrollId, "chat-tool-c")
        }
    }

    func testInitialSnapshotFailuresNeverAttachWithoutBaseline() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        store.start()
        try await waitFor { LifecycleURLProtocol.snapshotHits >= 2 }
        XCTAssertFalse(store.hasLoaded)
        XCTAssertTrue(frames.attachments.isEmpty)
        XCTAssertNotEqual(store.connection, .live)
        LifecycleURLProtocol.respond()
        try await waitFor(8) { frames.attachments.count == 1 }
        XCTAssertTrue(store.hasLoaded)
        XCTAssertEqual(frames.attachments, [1])
        frames.yield(connected)
        try await waitFor { store.connection == .live }
    }

    func testTurnEndFailureBlocksNextTurnUntilCanonicalSnapshot() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(connected)
        frames.yield(event(2, "turn.started", #"{"prompt":"first","turn_index":1}"#))
        frames.yield(event(3, "model.text_delta", #"{"request_id":"r","delta":"lossy"}"#))
        try await waitFor { store.streamRevision == 1 }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        frames.yield(event(4, "turn.finished"))
        frames.yield(event(5, "turn.started", #"{"prompt":"next","turn_index":2}"#))
        frames.yield(event(6, "model.text_delta", #"{"request_id":"r2","delta":"fresh"}"#))
        try await waitFor { frames.terminations == 1 }
        XCTAssertEqual(frames.attachments, [1])
        try await waitFor { LifecycleURLProtocol.snapshotHits >= 3 }
        XCTAssertEqual(frames.attachments, [1])
        XCTAssertEqual(store.state, .idle)
        XCTAssertFalse(store.messages.contains { $0.copyText == "next" })
        XCTAssertNotEqual(store.connection, .live)
        XCTAssertEqual(store.streamRevision, 1)
        LifecycleURLProtocol.respond(body: snapshot(sequence: 5, state: "running", nextPrompt: true))
        try await waitFor(8) { frames.attachments.count == 2 }
        XCTAssertEqual(frames.attachments, [1, 5])
        XCTAssertNotEqual(store.connection, .live)
        // The broker resumes from the canonical watermark, not the old queue.
        frames.yield(connected, on: 1)
        frames.yield(event(6, "model.text_delta", #"{"request_id":"r2","delta":"fresh"}"#), on: 1)
        try await waitFor { store.streamRevision == 2 }
        XCTAssertEqual(store.messages.filter { $0.copyText == "next" }.count, 1)
        XCTAssertEqual(store.messages.first?.copyText, "canonical")
        XCTAssertTrue(store.isBusy)
        let text = try XCTUnwrap(store.draft?.segments.last)
        guard case let .text(segment) = text else { return XCTFail("expected text") }
        XCTAssertEqual(segment.text, "fresh")
        XCTAssertTrue(segment.live)
        XCTAssertEqual(frames.attachments, [1, 5])
        XCTAssertEqual(frames.terminations, 1)
        XCTAssertEqual(store.connection, .live)
    }

    func testSlowTurnEndReconcilePausesWatchdogThenSilenceStillDisconnects() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(
            api, frames: frames, watchdogTimeout: .milliseconds(300),
            watchdogPollInterval: .milliseconds(20),
        )
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(connected)
        frames.yield(event(2, "turn.started"))
        try await waitFor { store.isBusy && store.connection == .live }
        LifecycleURLProtocol.hold([2])
        frames.yield(event(3, "turn.finished"))
        try await waitFor { LifecycleURLProtocol.snapshotHits == 2 }
        // No frame is consumed during the held snapshot. Waiting through
        // several watchdog periods must not cancel this healthy transport.
        try await Task.sleep(for: .milliseconds(1000))
        XCTAssertEqual(frames.attachments.count, 1)
        XCTAssertEqual(store.connection, .live)
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 2)
        XCTAssertEqual(frames.terminations, 0)
        XCTAssertNotNil(store.draft)
        LifecycleURLProtocol.release(2, body: snapshot(sequence: 3))
        try await waitFor { store.draft == nil && store.connection == .live }
        XCTAssertEqual(store.connection, .live)
        // The expired pre-pause timestamp must not immediately kill the stream.
        try await Task.sleep(for: .milliseconds(100))
        XCTAssertEqual(store.connection, .live)
        XCTAssertEqual(frames.attachments.count, 1)
        // No heartbeats after recovery: normal silence detection is enabled again.
        try await waitFor { store.connection != .live }
        try await waitFor { frames.attachments.count == 2 }
        XCTAssertGreaterThanOrEqual(frames.terminations, 1)
        XCTAssertEqual(frames.attachments, [1, 3])
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 2)
    }

    func testTurnEndFailureClosesStreamBeforeOfflineRetryAndNewStreamDetectsSilence() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(
            api, frames: frames, watchdogTimeout: .milliseconds(300),
            watchdogPollInterval: .milliseconds(20),
        )
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(connected)
        frames.yield(event(2, "turn.started"))
        try await waitFor { store.isBusy && store.connection == .live }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        LifecycleURLProtocol.hold([3])
        frames.yield(event(3, "turn.finished"))
        try await waitFor { frames.terminations == 1 }
        XCTAssertNotNil(store.lastError)
        XCTAssertNotEqual(store.connection, .live)
        try await waitFor { LifecycleURLProtocol.snapshotHits == 3 }
        // The retry request is held beyond the watchdog timeout, but no
        // transport remains open to accumulate frames or be watchdog-cancelled.
        try await Task.sleep(for: .milliseconds(700))
        XCTAssertEqual(frames.terminations, 1)
        XCTAssertEqual(frames.attachments, [1])
        XCTAssertNotNil(store.draft)
        LifecycleURLProtocol.release(3, body: snapshot(sequence: 3))
        try await waitFor { frames.attachments.count == 2 }
        XCTAssertNil(store.draft)
        XCTAssertEqual(frames.attachments, [1, 3])
        XCTAssertNotEqual(store.connection, .live)
        frames.yield(connected, on: 1)
        try await waitFor { store.connection == .live }
        // The new stream still has a normal watchdog: true silence cancels it.
        try await waitFor { frames.terminations == 2 }
        XCTAssertNotEqual(store.connection, .live)
    }

    func testRepeatedTurnEnd503sRetryOfflineWithoutAttachingOrProjectingOldQueue() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(connected)
        frames.yield(event(2, "turn.started"))
        try await waitFor { store.isBusy }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        frames.yield(event(3, "turn.finished"))
        frames.yield(event(4, "turn.started", #"{"prompt":"queued old turn"}"#))
        try await waitFor { frames.terminations == 1 }
        guard case .terminated = frames.yield(event(5, "turn.started")) else {
            return XCTFail("cancelled transport must reject further buffering")
        }
        // Even repeated failures keep only canonical REST retries alive.
        try await waitFor(8) { LifecycleURLProtocol.snapshotHits >= 5 }
        XCTAssertEqual(frames.attachments, [1])
        XCTAssertEqual(frames.terminations, 1)
        XCTAssertEqual(store.state, .idle)
        XCTAssertFalse(store.messages.contains { $0.copyText == "queued old turn" })
        XCTAssertNotEqual(store.connection, .live)
        LifecycleURLProtocol.respond(body: snapshot(sequence: 4, state: "running", nextPrompt: true))
        try await waitFor(15) { frames.attachments.count == 2 }
        XCTAssertEqual(frames.attachments, [1, 4])
        frames.yield(connected, on: 1)
        frames.yield(event(5, "model.text_delta", #"{"request_id":"r","delta":"replayed"}"#), on: 1)
        try await waitFor { store.streamRevision == 1 && store.connection == .live }
        guard case let .text(text) = store.draft?.segments.last else {
            return XCTFail("expected replayed text")
        }
        XCTAssertEqual(text.text, "replayed")
        XCTAssertEqual(frames.terminations, 1)
    }

    func testTurnEndSupersededByManualSnapshotKeepsOriginalStream() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(connected)
        frames.yield(event(2, "turn.started"))
        try await waitFor { store.isBusy }
        LifecycleURLProtocol.hold([2])
        frames.yield(event(3, "turn.finished"))
        try await waitFor { LifecycleURLProtocol.snapshotHits == 2 }
        LifecycleURLProtocol.respond(body: snapshot(sequence: 3))
        let fresh = await store.refresh()
        XCTAssertEqual(fresh, .committed)
        LifecycleURLProtocol.release(2, status: 503, body: snapshotFailure)
        frames.yield(event(4, "turn.started", #"{"prompt":"same stream"}"#))
        try await waitFor { store.messages.last?.copyText == "same stream" }
        XCTAssertEqual(frames.attachments, [1])
        XCTAssertEqual(frames.terminations, 0)
        XCTAssertEqual(store.connection, .live)
        XCTAssertNil(store.lastError)
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 3)
    }

    func testResyncFailureRetriesSnapshotAndResetsRejectedCursor() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(connected)
        frames.yield(event(20, "turn.started"))
        try await waitFor { store.isBusy }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        frames.yield(SSEFrame(content: .event(id: nil, event: "server.resync", data: "{}")))
        try await waitFor { LifecycleURLProtocol.snapshotHits >= 3 }
        XCTAssertEqual(frames.attachments.count, 1)
        XCTAssertNotEqual(store.connection, .live)
        LifecycleURLProtocol.respond(body: snapshot(sequence: 2))
        try await waitFor(8) { frames.attachments.count == 2 }
        XCTAssertEqual(frames.attachments, [1, 2])
        XCTAssertFalse(store.isBusy)
    }

    func testPauseDuringFailedReconcileRestartsWithFreshHandshake() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        store.start()
        try await waitFor { store.lastError != nil }
        store.pause()
        let hits = LifecycleURLProtocol.snapshotHits
        try await Task.sleep(for: .milliseconds(1400))
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, hits)
        XCTAssertTrue(frames.attachments.isEmpty)
        LifecycleURLProtocol.respond()
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        XCTAssertTrue(store.hasLoaded)
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, hits + 1)
    }

    func testQueuedOldFramesCannotWriteOrDrainRestartedLoop() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        // Queue while the MainActor cannot yet handle them, then invalidate the epoch.
        frames.yield(event(100, "turn.started", #"{"prompt":"stale"}"#))
        frames.yield(SSEFrame(content: .event(id: nil, event: "server.draining", data: "{}")))
        frames.yield(SSEFrame(content: .event(id: nil, event: "server.resync", data: "{}")))
        store.pause()
        store.start()
        try await waitFor { frames.attachments.count == 2 }
        frames.yield(connected, on: 1)
        frames.yield(event(2, "turn.started", #"{"prompt":"current"}"#), on: 1)
        try await waitFor { store.isBusy }
        XCTAssertEqual(store.messages.last?.copyText, "current")
        XCTAssertFalse(store.messages.contains { $0.copyText == "stale" })
        XCTAssertEqual(store.connection, .live)
        XCTAssertEqual(frames.attachments, [1, 1])
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 2)
    }

    func testNewManualSnapshotSatisfiesSupersededLoopRefresh() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.hold([1])
        store.start()
        try await waitFor { LifecycleURLProtocol.snapshotHits == 1 }
        let result = await store.refresh()
        XCTAssertEqual(result, .committed)
        LifecycleURLProtocol.release(1, status: 503, body: snapshotFailure)
        try await waitFor { frames.attachments.count == 1 }
        XCTAssertNil(store.lastError)
        XCTAssertTrue(store.hasLoaded)
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 2)
    }

    func testRefreshResultsDistinguishFailureSupersessionAndStop() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        let failed = await store.refresh()
        XCTAssertEqual(failed, .failed)
        XCTAssertFalse(store.hasLoaded)
        store.dismissError()
        LifecycleURLProtocol.respond()
        LifecycleURLProtocol.hold([2])
        let old = Task { await store.refresh() }
        try await waitFor { LifecycleURLProtocol.snapshotHits == 2 }
        let fresh = await store.refresh()
        XCTAssertEqual(fresh, .committed)
        LifecycleURLProtocol.release(2, status: 503, body: snapshotFailure)
        let superseded = await old.value
        XCTAssertEqual(superseded, .superseded)
        XCTAssertNil(store.lastError)
        store.stop()
        let stopped = await store.refresh()
        XCTAssertEqual(stopped, .stopped)
    }

    func testProjectionDuringManualRefreshSupersedesOldSnapshot() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        LifecycleURLProtocol.hold([2])
        let old = Task { await store.refresh() }
        try await waitFor { LifecycleURLProtocol.snapshotHits == 2 }
        frames.yield(event(2, "turn.started", #"{"prompt":"new projection"}"#))
        try await waitFor { store.isBusy }
        LifecycleURLProtocol.release(2)
        let result = await old.value
        XCTAssertEqual(result, .superseded)
        XCTAssertTrue(store.isBusy)
        XCTAssertEqual(store.messages.last?.copyText, "new projection")
    }

    func testBusyPhasesDoNotNotifyHistoryTranscript() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(body: snapshot(sequence: 1, state: "running"))
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        let unchanged = expectation(description: "busy phases leave history projection untouched")
        unchanged.isInverted = true
        withObservationTracking {
            _ = store.transcript
        } onChange: {
            unchanged.fulfill()
        }
        frames.yield(event(2, "run.phase_changed", #"{"phase":"awaiting_approval"}"#))
        frames.yield(event(3, "run.phase_changed", #"{"phase":"executing_tools"}"#))
        frames.yield(event(4, "run.cancel_requested"))
        try await waitFor { store.state == .cancelling }
        await fulfillment(of: [unchanged], timeout: 0.15)
        guard case let .message(tail) = store.transcript.rows.last else {
            return XCTFail("expected history row")
        }
        XCTAssertFalse(tail.showActions)
        store.pause()
    }

    func testUserTurnStartPublishesOnlyFullyUpdatedRenderInputs() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        let updated = expectation(description: "turn start publishes after prompt and draft")
        withObservationTracking {
            _ = store.transcript
        } onChange: {
            MainActor.assumeIsolated {
                XCTAssertTrue(store.isBusy)
                XCTAssertEqual(store.messages.last?.copyText, "user turn")
                XCTAssertEqual(store.draft?.turnIndex, 2)
            }
            updated.fulfill()
        }
        frames.yield(event(2, "turn.started", #"{"prompt":"user turn","turn_index":2}"#))
        await fulfillment(of: [updated], timeout: 2)
        XCTAssertEqual(store.messages.filter { $0.copyText == "user turn" }.count, 1)
        guard case let .message(tail) = store.transcript.rows.last else {
            return XCTFail("expected user row")
        }
        XCTAssertEqual(tail.message.copyText, "user turn")
        XCTAssertFalse(tail.showActions)
    }

    func testPauseInvalidatesInFlightSnapshotWithoutPollutingRestart() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.hold([1])
        let old = Task { await store.refresh() }
        try await waitFor { LifecycleURLProtocol.snapshotHits == 1 }
        store.pause()
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        LifecycleURLProtocol.release(1, body: snapshot(sequence: 100, state: "running"))
        let result = await old.value
        XCTAssertEqual(result, .stopped)
        XCTAssertEqual(store.state, .idle)
        XCTAssertEqual(store.messages.first?.copyText, "done")
        XCTAssertEqual(frames.attachments, [1])
        XCTAssertNil(store.lastError)
    }

    func testStopDuringSnapshotFailureEndsRetries() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        LifecycleURLProtocol.respond(status: 503, body: snapshotFailure)
        store.start()
        try await waitFor { store.lastError != nil }
        store.stop()
        let hits = LifecycleURLProtocol.snapshotHits
        LifecycleURLProtocol.respond()
        try await Task.sleep(for: .milliseconds(1400))
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, hits)
        XCTAssertFalse(store.hasLoaded)
        XCTAssertFalse(store.isStreaming)
        XCTAssertTrue(frames.attachments.isEmpty)
    }

    func testTerminalSnapshotBehindFrameKeepsCursorAndReplaysNextTurn() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(event(2, "turn.started"))
        try await waitFor { store.isBusy }
        LifecycleURLProtocol.respond(body: snapshot(sequence: 3))
        frames.yield(event(4, "turn.finished"))
        frames.yield(event(4, "turn.started", #"{"prompt":"duplicate terminal"}"#))
        frames.yield(event(5, "turn.started", #"{"prompt":"next"}"#))
        try await waitFor { store.messages.last?.copyText == "next" }
        XCTAssertTrue(store.isBusy)
        XCTAssertEqual(store.messages.first?.copyText, "canonical")
        XCTAssertFalse(store.messages.contains { $0.copyText == "duplicate terminal" })
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 2)
    }

    func testModelFailureSealsDraftButCompactionRetryStaysInSameTurn() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let frames = LifecycleFrames()
        let store = makeStore(api, frames: frames)
        defer { store.stop() }
        store.start()
        try await waitFor { frames.attachments.count == 1 }
        frames.yield(event(2, "turn.started", #"{"prompt":"work"}"#))
        frames.yield(event(3, "model.text_delta", #"{"request_id":"r1","delta":"partial"}"#))
        frames.yield(event(4, "model.request_failed"))
        try await waitFor { store.activityGeneration == 2 }
        XCTAssertTrue(store.isBusy)
        guard case let .text(sealed) = store.draft?.segments.first else { return XCTFail("expected sealed text") }
        XCTAssertFalse(sealed.live)
        XCTAssertEqual(sealed.text, "partial")
        frames.yield(event(5, "run.phase_changed", #"{"phase":"compacting"}"#))
        frames.yield(event(6, "context.compacted"))
        frames.yield(event(7, "model.request_started", #"{"request_id":"r2"}"#))
        frames.yield(event(8, "model.text_delta", #"{"request_id":"r2","delta":"retry"}"#))
        try await waitFor { store.streamRevision == 2 }
        XCTAssertTrue(store.isBusy)
        XCTAssertEqual(store.messages.filter { $0.role == .user }.count, 1)
        XCTAssertEqual(store.draft?.segments.count, 2)
        LifecycleURLProtocol.respond(body: snapshot(sequence: 9))
        frames.yield(event(9, "turn.finished"))
        try await waitFor { !store.isBusy && store.draft == nil }
        XCTAssertEqual(store.messages.first?.copyText, "canonical")
    }

    /// pause() must drop the event loop while keeping drafts and rendered
    /// rows; the next start() must re-run the snapshot handshake (second
    /// /snapshot hit) rather than being swallowed as a no-op.
    func testPauseStopsStreamingAndRestartIsLossless() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        defer { store.stop() }

        store.composerDraft = "keep me"
        store.start()
        try await waitFor { store.hasLoaded }
        XCTAssertTrue(store.isStreaming)
        let rowCount = store.transcript.rows.count
        XCTAssertGreaterThan(rowCount, 0)
        XCTAssertEqual(LifecycleURLProtocol.snapshotHits, 1)

        store.pause()
        XCTAssertFalse(store.isStreaming)
        XCTAssertEqual(store.composerDraft, "keep me")
        XCTAssertEqual(store.transcript.rows.count, rowCount)

        store.start()
        try await waitFor { LifecycleURLProtocol.snapshotHits >= 2 }
        XCTAssertTrue(store.isStreaming)
        XCTAssertEqual(store.composerDraft, "keep me")
        XCTAssertEqual(store.transcript.rows.count, rowCount)
    }

    /// The list keeps exactly the selected session streaming; re-vending a
    /// paused store through store(for:) resumes it.
    func testSetActivePausesOnlyBackgroundStores() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let list = SessionListStore(api: api)
        defer { list.stop() }

        let storeA = list.store(for: "a")
        let storeB = list.store(for: "b")
        try await waitFor { storeA.isStreaming && storeB.isStreaming }

        list.setActive("a")
        XCTAssertTrue(storeA.isStreaming)
        XCTAssertFalse(storeB.isStreaming)

        storeB.composerDraft = "b's draft"
        list.setActive(nil)
        XCTAssertFalse(storeA.isStreaming)
        XCTAssertFalse(storeB.isStreaming)

        // Selecting b again resumes its stream; the draft survives.
        let storeB2 = list.store(for: "b")
        XCTAssertTrue(storeB2 === storeB)
        try await waitFor { storeB2.isStreaming }
        XCTAssertEqual(storeB2.composerDraft, "b's draft")
    }

    /// stop() stays terminal: a stopped store must not restart via
    /// store(for:)'s resume nudge.
    func testStopRemainsTerminal() async throws {
        let (api, session) = try makeAPI()
        defer { session.invalidateAndCancel() }
        let store = SessionStore(sessionId: "s", api: api)
        store.start()
        try await waitFor { store.hasLoaded }
        store.stop()
        XCTAssertFalse(store.isStreaming)
        store.start()
        try await Task.sleep(for: .milliseconds(200))
        XCTAssertFalse(store.isStreaming)
    }
}
