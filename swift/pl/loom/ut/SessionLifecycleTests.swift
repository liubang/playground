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

/// Answers every /snapshot with a fixed idle snapshot and counts hits, so
/// tests can assert that a paused store re-runs the snapshot+stream
/// handshake on restart. Everything else gets a 404 (approval-mode and
/// other best-effort loads tolerate errors; event-stream attempts point at
/// 127.0.0.1:9 and fail fast outside the mock).
private final class LifecycleURLProtocol: URLProtocol {
    nonisolated(unsafe) static var snapshotHits = 0

    static let snapshot = Data((
        #"{"state":"idle","session_id":"s","model_name":"test","turn_count":1,"event_seq":1,"# +
            #""messages":[{"id":"a1","role":"assistant","status":"final","parts":[{"kind":"text","text":"done"}]}]}"#
    ).utf8)

    static func reset() { snapshotHits = 0 }

    override class func canInit(with _: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        let path = request.url?.path ?? ""
        if path.hasSuffix("/snapshot") {
            Self.snapshotHits += 1
            finish(200, Self.snapshot)
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
            if condition() { return }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTFail("condition not met within \(timeout)s")
    }

    override func setUp() {
        LifecycleURLProtocol.reset()
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
