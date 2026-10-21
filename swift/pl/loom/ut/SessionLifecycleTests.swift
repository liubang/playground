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

    private func event(_ sequence: UInt64, _ kind: String, _ payload: String = "{}") -> SSEFrame {
        SSEFrame(content: .event(
            id: sequence, event: kind,
            data: """
            {"version":1,"sequence":\(sequence),"session_id":"s","kind":"\(kind)","durable":true,"payload":\(payload)}
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
