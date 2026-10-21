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

/// Header-capturing transport (the PromptURLProtocol pattern): the
/// explorer's conditional requests live in headers (If-None-Match /
/// ETag), which the other stubs do not model.
private final class ExplorerURLProtocol: URLProtocol {
    static let lock = NSLock()
    nonisolated(unsafe) static var requests: [URLRequest] = []
    nonisolated(unsafe) static var reply: ((URLRequest) -> (Int, [String: String], String))?
    private nonisolated(unsafe) static var delayed = false
    private nonisolated(unsafe) static var pending: [Int: ExplorerURLProtocol] = [:]
    private nonisolated(unsafe) static var stopped: Set<Int> = []
    private let responseLock = NSRecursiveLock()
    private var responseStopped = false

    static func stoppedIDs() -> Set<Int> {
        lock.lock()
        defer { lock.unlock() }
        return stopped
    }

    static func requestIDs(to suffix: String) -> [Int] {
        lock.lock()
        defer { lock.unlock() }
        return requests.indices.filter { requests[$0].url?.path.hasSuffix(suffix) == true }
    }

    static func delayResponses() {
        lock.lock()
        delayed = true
        lock.unlock()
    }

    static func pendingIDs(
        to suffix: String, workspace: String? = nil, raw: Bool? = nil,
    ) -> [Int] {
        lock.lock()
        defer { lock.unlock() }
        return pending.keys.sorted().filter { id in
            guard let url = pending[id]?.request.url else { return false }
            if let workspace, !url.path.hasPrefix("/v1/workspaces/\(workspace)/") {
                return false
            }
            let isRaw = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                .queryItems?.contains { $0.name == "raw" && $0.value == "1" } == true
            return url.path.hasSuffix(suffix) && (raw == nil || raw == isRaw)
        }
    }

    static func complete(
        _ id: Int, status: Int = 200, headers: [String: String] = [:], body: String,
    ) {
        complete(id, status: status, headers: headers, data: Data(body.utf8))
    }

    static func complete(
        _ id: Int, status: Int = 200, headers: [String: String] = [:], data: Data,
    ) {
        lock.lock()
        let transport = pending.removeValue(forKey: id)
        lock.unlock()
        guard let transport else { return }
        transport.deliver(status: status, headers: headers, data: data)
    }

    static func reset() {
        lock.lock()
        requests = []
        reply = nil
        delayed = false
        let abandoned = Array(pending.values)
        pending = [:]
        stopped = []
        lock.unlock()
        for transport in abandoned {
            transport.client?.urlProtocol(transport, didFailWithError: URLError(.cancelled))
        }
    }

    static func requests(to suffix: String) -> [URLRequest] {
        lock.lock()
        defer { lock.unlock() }
        return requests.filter { $0.url?.path.hasSuffix(suffix) == true }
    }

    override class func canInit(with _: URLRequest) -> Bool {
        true
    }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest {
        request
    }

    override func startLoading() {
        Self.lock.lock()
        let id = Self.requests.count
        Self.requests.append(request)
        let delayed = Self.delayed
        let reply = Self.reply
        if delayed {
            Self.pending[id] = self
        }
        Self.lock.unlock()
        if delayed {
            return
        }
        guard let reply else {
            client?.urlProtocol(self, didFailWithError: URLError(.unknown))
            return
        }
        let (status, headers, body) = reply(request)
        deliver(status: status, headers: headers, data: Data(body.utf8))
    }

    private func deliver(status: Int, headers: [String: String], data: Data) {
        responseLock.lock()
        defer { responseLock.unlock() }
        guard !responseStopped else { return }
        responseStopped = true
        let response = HTTPURLResponse(
            url: request.url!, statusCode: status, httpVersion: nil, headerFields: headers,
        )!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {
        responseLock.lock()
        defer { responseLock.unlock() }
        responseStopped = true
        Self.lock.lock()
        for (id, transport) in Self.pending where transport === self {
            Self.stopped.insert(id)
        }
        Self.pending = Self.pending.filter { $0.value !== self }
        Self.lock.unlock()
    }
}

/// Golden-wire tests for the workspace-explorer surface: the JSON
/// literals mirror go/pl/loom/internal/server/handlers_workspace_explorer.go.
final class WorkspaceExplorerTests: XCTestCase {
    override func setUp() {
        super.setUp()
        ExplorerURLProtocol.reset()
    }

    override func tearDown() {
        ExplorerURLProtocol.reset()
        super.tearDown()
    }

    private func decode<T: Decodable>(_: T.Type, _ json: String) throws -> T {
        try LoomJSON.decoder.decode(T.self, from: Data(json.utf8))
    }

    private func makeClient() -> APIClient {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [ExplorerURLProtocol.self]
        return APIClient(
            baseURL: URL(string: "http://loom.test")!, token: "t",
            session: URLSession(configuration: config),
        )
    }

    @MainActor
    private func eventually(
        timeout: Duration = .seconds(5), file: StaticString = #filePath, line: UInt = #line,
        condition: @MainActor () -> Bool,
    ) async {
        let deadline = ContinuousClock.now + timeout
        while !condition() {
            if ContinuousClock.now > deadline {
                XCTFail("condition not met within \(timeout)", file: file, line: line)
                return
            }
            try? await Task.sleep(for: .milliseconds(10))
        }
    }

    // MARK: Wire decoding

    func testFileListDecoding() throws {
        let listing = try decode(WorkspaceFileListResponse.self, """
        {"path":"src","entries":[
          {"name":"cmd","path":"src/cmd","kind":"dir"},
          {"name":"main.go","path":"src/main.go","kind":"file","size":1234,"mod_time":"2026-09-26T08:30:00Z"}
        ],"truncated":false}
        """)
        XCTAssertEqual(listing.path, "src")
        XCTAssertEqual(listing.entries.count, 2)
        XCTAssertEqual(listing.entries[0].kind, .dir)
        XCTAssertEqual(listing.entries[0].id, "src/cmd")
        XCTAssertEqual(listing.entries[1].size, 1234)
        XCTAssertNotNil(listing.entries[1].modTime)
        XCTAssertEqual(listing.truncated, false)
    }

    func testFileContentDecoding() throws {
        let text = try decode(WorkspaceFileContent.self, """
        {"path":"a.txt","size":11,"truncated":false,"binary":false,"content":"hello world"}
        """)
        XCTAssertEqual(text.content, "hello world")
        XCTAssertEqual(text.binary, false)

        let binary = try decode(WorkspaceFileContent.self, """
        {"path":"bin.dat","size":4096,"truncated":false,"binary":true}
        """)
        XCTAssertEqual(binary.binary, true)
        XCTAssertNil(binary.content)
    }

    func testFileSearchDecoding() throws {
        let response = try decode(WorkspaceFileSearchResponse.self, """
        {"query":"bloom","matches":[
          {"path":"cpp/pl/bloom","name":"bloom","kind":"dir"},
          {"path":"cpp/pl/bloom/filter.h","name":"filter.h","kind":"file"}
        ],"truncated":true}
        """)
        XCTAssertEqual(response.matches.count, 2)
        XCTAssertEqual(response.matches[0].kind, .dir)
        XCTAssertEqual(response.truncated, true)
    }

    func testGitStatusDecoding() throws {
        let status = try decode(WorkspaceGitStatus.self, """
        {"is_git":true,"branch":"main","files":[
          {"path":"a.swift","status":"M","staged":false,"unstaged":true,"adds":3,"dels":1,"no_stat":false},
          {"path":"b.md","status":"U","staged":false,"unstaged":true,"adds":0,"dels":0,"no_stat":true}
        ],"adds":3,"dels":1}
        """)
        XCTAssertTrue(status.isGit)
        XCTAssertEqual(status.branch, "main")
        XCTAssertEqual(status.files?.count, 2)
        XCTAssertEqual(status.files?[1].noStat, true)
        XCTAssertEqual(status.adds, 3)
    }

    /// Non-git workspaces answer a bare is_git=false — every other
    /// field must tolerate absence.
    func testGitStatusNotARepo() throws {
        let status = try decode(WorkspaceGitStatus.self, #"{"is_git":false}"#)
        XCTAssertFalse(status.isGit)
        XCTAssertNil(status.branch)
        XCTAssertNil(status.files)
    }

    func testGitDiffDecoding() throws {
        let diff = try decode(WorkspaceGitDiff.self, """
        {"path":"new.txt","diff":"diff --no-index...","untracked":true}
        """)
        XCTAssertEqual(diff.untracked, true)
        XCTAssertNil(diff.isDir)

        let dir = try decode(WorkspaceGitDiff.self, #"{"path":"scratch","is_dir":true}"#)
        XCTAssertEqual(dir.isDir, true)
        XCTAssertNil(dir.diff)
    }

    // MARK: APIClient conditional listing (ETag / 304)

    func testWorkspaceFilesConditionalRequest() async throws {
        ExplorerURLProtocol.reply = { request in
            XCTAssertEqual(
                URLComponents(url: request.url!, resolvingAgainstBaseURL: false)?
                    .queryItems?.first { $0.name == "path" }?.value,
                "",
            )
            if request.value(forHTTPHeaderField: "If-None-Match") == #"W/"tag1""# {
                return (304, [:], "")
            }
            return (200, ["ETag": #"W/"tag1""#], #"{"path":"","entries":[],"truncated":false}"#)
        }
        let client = makeClient()

        let first = try await client.workspaceFiles("ws1", path: "")
        guard case let .modified(_, etag) = first else {
            XCTFail("expected .modified, got \(first)")
            return
        }
        XCTAssertEqual(etag, #"W/"tag1""#)

        let second = try await client.workspaceFiles("ws1", path: "", ifNoneMatch: etag)
        guard case .notModified = second else {
            XCTFail("expected .notModified, got \(second)")
            return
        }
    }

    // MARK: APIClient raw image channel

    func testWorkspaceFileRaw() async throws {
        ExplorerURLProtocol.reply = { request in
            let query = URLComponents(url: request.url!, resolvingAgainstBaseURL: false)?
                .queryItems ?? []
            XCTAssertEqual(query.first { $0.name == "path" }?.value, "img/pic.png")
            XCTAssertEqual(query.first { $0.name == "raw" }?.value, "1")
            return (200, ["Content-Type": "image/png"], "PNG-BYTES")
        }
        let (data, mediaType) = try await makeClient().workspaceFileRaw("ws1", path: "img/pic.png")
        XCTAssertEqual(String(decoding: data, as: UTF8.self), "PNG-BYTES")
        XCTAssertEqual(mediaType, "image/png")
    }

    // MARK: Store: lazy tree + flattening

    @MainActor
    func testTreeLazyLoadAndVisibleNodes() async {
        ExplorerURLProtocol.reply = { request in
            let path = URLComponents(url: request.url!, resolvingAgainstBaseURL: false)?
                .queryItems?.first { $0.name == "path" }?.value ?? ""
            switch (request.url!.path, path) {
            case ("/v1/workspaces/ws1/git/status", _):
                return (200, [:], #"{"is_git":false}"#)
            case ("/v1/workspaces/ws1/files", ""):
                return (200, [:], """
                {"path":"","entries":[
                  {"name":"src","path":"src","kind":"dir"},
                  {"name":"README.md","path":"README.md","kind":"file","size":10}
                ],"truncated":false}
                """)
            case ("/v1/workspaces/ws1/files", "src"):
                return (200, [:], """
                {"path":"src","entries":[
                  {"name":"main.go","path":"src/main.go","kind":"file","size":5}
                ],"truncated":false}
                """)
            default:
                return (404, [:], #"{"error":{"code":"not_found","message":"nope"}}"#)
            }
        }
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "ws1")
        await eventually { store.entries(for: "") != nil }

        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["src", "README.md"])
        // A collapsed directory costs no request beyond the root.
        XCTAssertEqual(ExplorerURLProtocol.requests(to: "/files").count, 1)

        store.toggleDir("src")
        await eventually { store.entries(for: "src") != nil }
        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["src", "src/main.go", "README.md"])
        XCTAssertEqual(store.visibleNodes.map(\.depth), [0, 1, 0])

        // Collapsing prunes the subtree from the flattened list.
        store.toggleDir("src")
        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["src", "README.md"])
    }

    @MainActor
    func testRevealInTreeExpandsAncestors() async {
        ExplorerURLProtocol.reply = { _ in
            (200, [:], #"{"path":"","entries":[],"truncated":false}"#)
        }
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "ws1")
        await eventually { store.entries(for: "") != nil }

        store.searchQuery = "deep"
        store.revealInTree(WorkspaceFileMatch(path: "a/b/c", name: "c", kind: .dir))
        XCTAssertEqual(store.expanded, ["a", "a/b", "a/b/c"])
        XCTAssertEqual(store.searchQuery, "")
    }

    @MainActor
    func testChangeCountFollowsGitStatus() async {
        ExplorerURLProtocol.reply = { request in
            if request.url!.path.hasSuffix("/git/status") {
                return (200, [:], """
                {"is_git":true,"branch":"main","files":[
                  {"path":"a","status":"M","adds":1,"dels":0},
                  {"path":"b","status":"U","no_stat":true}
                ],"adds":1,"dels":0}
                """)
            }
            return (200, [:], #"{"path":"","entries":[],"truncated":false}"#)
        }
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "ws1")
        await eventually { store.git != nil }

        XCTAssertEqual(store.changeCount, 2)
        XCTAssertEqual(store.statusByPath, ["a": "M", "b": "U"])
    }

    /// A revalidation answered 304 keeps the cached entries (no
    /// clobbering with an empty payload).
    @MainActor
    func testRevalidationKeepsCacheOnNotModified() async {
        ExplorerURLProtocol.reply = { request in
            if request.url!.path.hasSuffix("/git/status") {
                return (200, [:], #"{"is_git":false}"#)
            }
            if request.value(forHTTPHeaderField: "If-None-Match") == #"W/"root""# {
                return (304, [:], "")
            }
            return (200, ["ETag": #"W/"root""#], """
            {"path":"","entries":[{"name":"f.txt","path":"f.txt","kind":"file"}],"truncated":false}
            """)
        }
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "ws1")
        await eventually { store.entries(for: "") != nil }
        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["f.txt"])

        // noteFileActivity coalesces behind 600ms; wait it out, then
        // the root revalidation should have hit the 304 path.
        store.noteFileActivity()
        await eventually(timeout: .seconds(3)) {
            ExplorerURLProtocol.requests(to: "/files").count >= 2
        }
        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["f.txt"])
    }

    // MARK: Controlled response ordering

    private enum OrderingFailure: Error {
        case timedOut
    }

    @MainActor
    private func pending(
        _ suffix: String, workspace: String? = nil, count: Int = 1, raw: Bool? = nil,
        file: StaticString = #filePath, line: UInt = #line,
    ) async throws -> [Int] {
        let deadline = ContinuousClock.now + .seconds(5)
        while true {
            let ids = ExplorerURLProtocol.pendingIDs(to: suffix, workspace: workspace, raw: raw)
            if ids.count == count {
                return ids
            }
            if ContinuousClock.now > deadline {
                XCTFail(
                    "pending \(workspace ?? "*")\(suffix): expected \(count), got \(ids)",
                    file: file, line: line,
                )
                throw OrderingFailure.timedOut
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    @MainActor
    private func waitForDetail(
        file: StaticString = #filePath, line: UInt = #line,
        condition: @MainActor () -> Bool,
    ) async throws {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline {
                XCTFail("detail did not settle within 5 seconds", file: file, line: line)
                throw OrderingFailure.timedOut
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    private let failure = #"{"error":{"code":"not_found","message":"missing"}}"#

    private func detailBody(diff: Bool, value: String) -> String {
        if diff {
            return "{\"path\":\"same.txt\",\"diff\":\"\(value)\"}"
        }
        return "{\"path\":\"same.txt\",\"size\":1,\"binary\":false,\"content\":\"\(value)\"}"
    }

    @MainActor
    private func open(_ store: WorkspaceExplorerStore, diff: Bool) {
        if diff {
            store.openDiff("same.txt")
        } else {
            store.openPreview("same.txt")
        }
    }

    @MainActor
    private func value(_ store: WorkspaceExplorerStore, diff: Bool) -> String? {
        diff ? store.diff?.diff : store.preview?.content
    }

    @MainActor
    private func loading(_ store: WorkspaceExplorerStore, diff: Bool) -> Bool {
        diff ? store.diffLoading : store.previewLoading
    }

    @MainActor
    private func detailError(_ store: WorkspaceExplorerStore, diff: Bool) -> String? {
        diff ? store.diffError : store.previewError
    }

    @MainActor
    private func checkDetailOrdering(
        diff: Bool, switchWorkspace: Bool, back: Bool, aba: Bool = false,
    ) async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        defer {
            store.bind(workspaceId: nil)
            ExplorerURLProtocol.reset()
        }
        let endpoint = diff ? "/git/diff" : "/file"
        store.bind(workspaceId: "a")
        open(store, diff: diff)
        let old = try await pending(endpoint, workspace: "a")
        if switchWorkspace {
            store.bind(workspaceId: "b")
        }
        if aba {
            store.bind(workspaceId: "a")
        }
        if back {
            store.back()
        }
        let workspace = try XCTUnwrap(store.workspaceId)
        open(store, diff: diff)
        let ids = try await pending(endpoint, workspace: workspace, count: workspace == "a" ? 2 : 1)
        let newest = try XCTUnwrap(ids.first { $0 != old[0] })
        ExplorerURLProtocol.complete(newest, body: detailBody(diff: diff, value: "new"))
        try await waitForDetail { self.value(store, diff: diff) == "new" }
        XCTAssertFalse(loading(store, diff: diff))

        ExplorerURLProtocol.complete(old[0], body: detailBody(diff: diff, value: "old"))
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertEqual(value(store, diff: diff), "new")
        XCTAssertNil(detailError(store, diff: diff))
        XCTAssertFalse(loading(store, diff: diff))
        // Exercise obsolete failures separately from a current failure/retry.
        open(store, diff: diff)
        let obsolete = try await pending(endpoint, workspace: workspace)
        if back {
            store.back()
        }
        open(store, diff: diff)
        let current = try await pending(endpoint, workspace: workspace, count: 2)
        let currentID = try XCTUnwrap(current.first { $0 != obsolete[0] })
        ExplorerURLProtocol.complete(obsolete[0], status: 404, body: failure)
        try await Task.sleep(for: .milliseconds(20))
        XCTAssertTrue(loading(store, diff: diff))
        XCTAssertNil(value(store, diff: diff))
        XCTAssertNil(detailError(store, diff: diff))
        ExplorerURLProtocol.complete(currentID, body: detailBody(diff: diff, value: "current"))
        try await waitForDetail { self.value(store, diff: diff) == "current" }
        XCTAssertNil(detailError(store, diff: diff))
        open(store, diff: diff)
        let failed = try await pending(endpoint, workspace: workspace)
        ExplorerURLProtocol.complete(failed[0], status: 404, body: failure)
        try await waitForDetail { self.detailError(store, diff: diff) != nil }
        XCTAssertNil(value(store, diff: diff))
        XCTAssertFalse(loading(store, diff: diff))
        open(store, diff: diff)
        let retry = try await pending(endpoint, workspace: workspace)
        ExplorerURLProtocol.complete(retry[0], body: detailBody(diff: diff, value: "retry"))
        try await waitForDetail { self.value(store, diff: diff) == "retry" }
        XCTAssertNil(detailError(store, diff: diff))
    }

    @MainActor
    func testPreviewAndDiffAcrossWorkspacesWithSamePath() async throws {
        for diff in [false, true] {
            try await checkDetailOrdering(diff: diff, switchWorkspace: true, back: false)
        }
    }

    @MainActor
    func testRepeatedSamePathLatestWinsAndRetries() async throws {
        for diff in [false, true] {
            try await checkDetailOrdering(diff: diff, switchWorkspace: false, back: false)
        }
    }

    @MainActor
    func testBackAndReopenInvalidatesPreviewAndDiff() async throws {
        for diff in [false, true] {
            try await checkDetailOrdering(diff: diff, switchWorkspace: false, back: true)
        }
    }

    @MainActor
    func testABAPreviewAndDiffIgnoreOriginalBinding() async throws {
        for diff in [false, true] {
            try await checkDetailOrdering(diff: diff, switchWorkspace: true, back: false, aba: true)
        }
    }

    @MainActor
    func testABARootRequestsDoNotBlockOrClearNewRequest() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "a")
        let oldRoot = try await pending("/files", workspace: "a")
        let oldGit = try await pending("/git/status", workspace: "a")
        XCTAssertTrue(store.gitLoading)
        store.bind(workspaceId: "b")
        let bRoot = try await pending("/files", workspace: "b")
        let bGit = try await pending("/git/status", workspace: "b")
        store.bind(workspaceId: "a")
        let aRoots = try await pending("/files", workspace: "a", count: 2)
        let newRoot = try XCTUnwrap(aRoots.first { $0 != oldRoot[0] })
        _ = try await pending("/git/status", workspace: "a", count: 2)
        let refresh = Task { await store.refreshGit() }
        let aGit = try await pending("/git/status", workspace: "a", count: 3)
        ExplorerURLProtocol.complete(oldRoot[0], body: #"{"path":"","entries":[{"name":"old","path":"old","kind":"file"}]}"#)
        ExplorerURLProtocol.complete(bRoot[0], status: 404, body: failure)
        ExplorerURLProtocol.complete(oldGit[0], status: 404, body: failure)
        ExplorerURLProtocol.complete(bGit[0], status: 404, body: failure)
        try ExplorerURLProtocol.complete(XCTUnwrap(aGit.last), body: #"{"is_git":false}"#)
        await refresh.value
        XCTAssertNil(store.gitError)
        XCTAssertNil(store.entries(for: ""))
        XCTAssertTrue(store.isLoading(""))
        store.loadDir("")
        XCTAssertEqual(ExplorerURLProtocol.requests(to: "/files").count, 3)
        ExplorerURLProtocol.complete(newRoot, headers: ["ETag": "root-a"], body: #"{"path":"","entries":[{"name":"new","path":"new","kind":"file"}]}"#)
        await eventually { !store.isLoading("") }
        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["new"])
        store.bind(workspaceId: "b")
        _ = try await pending("/files", workspace: "b")
        store.bind(workspaceId: "a")
        let revalidate = try await pending("/files", workspace: "a")
        XCTAssertEqual(ExplorerURLProtocol.requests(to: "/files").last?.value(forHTTPHeaderField: "If-None-Match"), "root-a")
        ExplorerURLProtocol.complete(revalidate[0], status: 304, body: "")
        await eventually { !store.isLoading("") }
        XCTAssertEqual(store.visibleNodes.map(\.entry.path), ["new"])
        store.bind(workspaceId: nil)
    }

    @MainActor
    func testGitOldFailureCannotClearCurrentLoading() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "a")
        let initial = try await pending("/git/status")
        ExplorerURLProtocol.complete(initial[0], body: #"{"is_git":true,"files":[{"path":"same.txt","status":"M"}]}"#)
        await eventually { !store.gitLoading }
        XCTAssertEqual(store.statusByPath, ["same.txt": "M"])
        let old = Task { await store.refreshGit() }
        let oldIDs = try await pending("/git/status")
        store.bind(workspaceId: "b")
        XCTAssertTrue(store.statusByPath.isEmpty)
        _ = try await pending("/git/status", workspace: "b")
        ExplorerURLProtocol.complete(oldIDs[0], status: 404, body: failure)
        await old.value
        XCTAssertTrue(store.gitLoading)
        XCTAssertNil(store.gitError)
        store.bind(workspaceId: nil)
        XCTAssertFalse(store.gitLoading)
        XCTAssertFalse(store.previewLoading)
        XCTAssertFalse(store.diffLoading)
        XCTAssertFalse(store.searching)
        XCTAssertTrue(store.loadingDirs.isEmpty)
    }

    @MainActor
    func testDirectoryFailureCanRetry() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "a")
        let first = try await pending("/files")
        ExplorerURLProtocol.complete(first[0], status: 404, body: failure)
        await eventually { !store.isLoading("") }
        store.loadDir("")
        let retry = try await pending("/files")
        ExplorerURLProtocol.complete(retry[0], body: #"{"path":"","entries":[]}"#)
        await eventually { store.entries(for: "") != nil && !store.isLoading("") }
        store.bind(workspaceId: nil)
    }

    @MainActor
    func testSearchSameQueryAcrossABAAndRetry() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "a")
        store.searchQuery = "same"
        let old = try await pending("/files/search", workspace: "a")
        store.bind(workspaceId: "b")
        store.searchQuery = "same"
        let bSearch = try await pending("/files/search", workspace: "b")
        store.bind(workspaceId: "a")
        store.searchQuery = "same"
        try await waitForDetail {
            ExplorerURLProtocol.stoppedIDs().isSuperset(of: [old[0], bSearch[0]])
        }
        let newest = try await pending("/files/search", workspace: "a")[0]
        XCTAssertNotEqual(newest, old[0])
        XCTAssertEqual(ExplorerURLProtocol.requestIDs(to: "/files/search"), [old[0], bSearch[0], newest])
        ExplorerURLProtocol.complete(old[0], body: #"{"query":"same","matches":[{"path":"old","name":"old","kind":"file"}],"truncated":true}"#)
        ExplorerURLProtocol.complete(bSearch[0], status: 404, body: failure)
        XCTAssertTrue(store.searching)
        ExplorerURLProtocol.complete(newest, body: #"{"query":"same","matches":[{"path":"new","name":"new","kind":"file"}]}"#)
        try await waitForDetail { !store.searching }
        XCTAssertEqual(store.searchResults.map(\.path), ["new"])
        XCTAssertFalse(store.searchTruncated)
        ExplorerURLProtocol.complete(old[0], status: 404, body: failure)
        XCTAssertEqual(store.searchResults.map(\.path), ["new"])
        XCTAssertFalse(store.searching)
        store.searchQuery = "same"
        let retry = try await pending("/files/search", workspace: "a")
        ExplorerURLProtocol.complete(retry[0], status: 404, body: failure)
        await eventually { !store.searching }
        XCTAssertEqual(store.searchResults.map(\.path), ["new"])
        store.bind(workspaceId: nil)
        XCTAssertTrue(store.searchResults.isEmpty)
        XCTAssertFalse(store.searchTruncated)
    }

    @MainActor
    func testSearchQueryChangeCancelsIssuedRequestAndKeepsCurrentLoading() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        defer { store.bind(workspaceId: nil) }
        store.bind(workspaceId: "a")
        store.searchQuery = "old"
        let old = try await pending("/files/search")[0]
        store.searchQuery = "new"
        XCTAssertTrue(store.searching)
        try await waitForDetail { ExplorerURLProtocol.stoppedIDs().contains(old) }
        let newest = try await pending("/files/search")[0]
        XCTAssertNotEqual(newest, old)
        XCTAssertEqual(ExplorerURLProtocol.requestIDs(to: "/files/search"), [old, newest])
        let newestURL = try XCTUnwrap(ExplorerURLProtocol.requests(to: "/files/search").last?.url)
        XCTAssertEqual(
            URLComponents(url: newestURL, resolvingAgainstBaseURL: false)?
                .queryItems?.first { $0.name == "q" }?.value,
            "new",
        )
        // A stopped transport is absent from pending; completion must be a no-op.
        ExplorerURLProtocol.complete(old, body: #"{"query":"old","matches":[{"path":"old","name":"old","kind":"file"}],"truncated":true}"#)
        XCTAssertTrue(store.searching)
        XCTAssertTrue(store.searchResults.isEmpty)
        XCTAssertFalse(store.searchTruncated)
        ExplorerURLProtocol.complete(newest, body: #"{"query":"new","matches":[{"path":"new","name":"new","kind":"file"}]}"#)
        try await waitForDetail { !store.searching }
        XCTAssertEqual(store.searchResults.map(\.path), ["new"])
        XCTAssertFalse(store.searchTruncated)
    }

    @MainActor
    func testSearchClearCancelsIssuedRequestWithoutStartingAnother() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        defer { store.bind(workspaceId: nil) }
        store.bind(workspaceId: "a")
        store.searchQuery = "old"
        let old = try await pending("/files/search")[0]
        store.searchQuery = ""
        XCTAssertFalse(store.searching)
        try await waitForDetail { ExplorerURLProtocol.stoppedIDs().contains(old) }
        ExplorerURLProtocol.complete(old, status: 404, body: failure)
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertEqual(ExplorerURLProtocol.requestIDs(to: "/files/search"), [old])
        XCTAssertTrue(ExplorerURLProtocol.pendingIDs(to: "/files/search").isEmpty)
        XCTAssertFalse(store.searching)
        XCTAssertTrue(store.searchResults.isEmpty)
        XCTAssertFalse(store.searchTruncated)
        store.searchQuery = "retry"
        let retry = try await pending("/files/search")[0]
        XCTAssertTrue(store.searching)
        ExplorerURLProtocol.complete(retry, body: #"{"query":"retry","matches":[]}"#)
        try await waitForDetail { !store.searching }
    }

    @MainActor
    func testSearchBindCancelsIssuedRequestAndIsolatesNewWorkspace() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        defer { store.bind(workspaceId: nil) }
        store.bind(workspaceId: "a")
        store.searchQuery = "same"
        let old = try await pending("/files/search", workspace: "a")[0]
        store.bind(workspaceId: "b")
        XCTAssertEqual(store.searchQuery, "")
        XCTAssertFalse(store.searching)
        try await waitForDetail { ExplorerURLProtocol.stoppedIDs().contains(old) }
        XCTAssertTrue(ExplorerURLProtocol.pendingIDs(to: "/files/search").isEmpty)
        store.searchQuery = "same"
        let newest = try await pending("/files/search", workspace: "b")[0]
        ExplorerURLProtocol.complete(old, status: 404, body: failure)
        XCTAssertTrue(store.searching)
        XCTAssertTrue(store.searchResults.isEmpty)
        ExplorerURLProtocol.complete(newest, body: #"{"query":"same","matches":[{"path":"b","name":"b","kind":"file"}]}"#)
        try await waitForDetail { !store.searching }
        XCTAssertEqual(store.searchResults.map(\.path), ["b"])
        XCTAssertFalse(store.searchTruncated)
    }

    @MainActor
    func testSearchCancelledBeforeDebounceDoesNotIssueRequest() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        defer { store.bind(workspaceId: nil) }
        store.bind(workspaceId: "a")
        // All mutations happen in the same actor turn, before a debounce task can run.
        store.searchQuery = "old"
        store.searchQuery = "replacement"
        store.searchQuery = ""
        store.searchQuery = "before-bind"
        store.bind(workspaceId: "b")
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertTrue(ExplorerURLProtocol.requestIDs(to: "/files/search").isEmpty)
        XCTAssertFalse(store.searching)
        XCTAssertTrue(store.searchResults.isEmpty)
        store.searchQuery = "latest"
        let latest = try await pending("/files/search", workspace: "b")[0]
        XCTAssertEqual(ExplorerURLProtocol.requestIDs(to: "/files/search"), [latest])
        XCTAssertTrue(store.searching)
        ExplorerURLProtocol.complete(latest, body: #"{"query":"latest","matches":[]}"#)
        try await waitForDetail { !store.searching }
    }

    @MainActor
    func testBindClearsDetailErrorsAndImageFailure() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        store.bind(workspaceId: "a")
        store.openPreview("same.txt")
        let preview = try await pending("/file")
        ExplorerURLProtocol.complete(preview[0], status: 404, body: failure)
        await eventually { store.previewError != nil }
        store.bind(workspaceId: "b")
        XCTAssertNil(store.previewError)
        store.openDiff("same.txt")
        let diff = try await pending("/git/diff")
        ExplorerURLProtocol.complete(diff[0], status: 404, body: failure)
        await eventually { store.diffError != nil }
        store.bind(workspaceId: "a")
        XCTAssertNil(store.diffError)
        store.openPreview("bad.png")
        store.loadPreviewImage("bad.png")
        let raw = try await pending("/file", raw: true)[0]
        ExplorerURLProtocol.complete(raw, data: Data("invalid".utf8))
        await eventually { store.previewImageFailed }
        store.bind(workspaceId: nil)
        XCTAssertFalse(store.previewImageFailed)
        XCTAssertNil(store.previewImage)
        XCTAssertNil(store.previewImageSize)
    }

    @MainActor
    func testImagePathGuardAndCachedNavigation() async throws {
        ExplorerURLProtocol.delayResponses()
        let store = WorkspaceExplorerStore(api: makeClient())
        let workspace = UUID().uuidString
        store.bind(workspaceId: workspace)
        store.openPreview("cached.png")
        store.loadPreviewImage("wrong.png")
        XCTAssertTrue(ExplorerURLProtocol.pendingIDs(to: "/file", raw: true).isEmpty)
        store.loadPreviewImage("cached.png")
        let first = try await pending("/file", raw: true)[0]
        ExplorerURLProtocol.complete(first, data: makePNG(pixelsWide: 10, pixelsHigh: 20))
        await eventually { store.previewImageSize == CGSize(width: 10, height: 20) }
        let cached = store.previewImage
        store.openPreview("new.png")
        store.loadPreviewImage("new.png")
        let obsolete = try await pending("/file", raw: true)[0]
        store.back()
        store.openPreview("cached.png")
        store.loadPreviewImage("cached.png")
        XCTAssertTrue(store.previewImage === cached)
        XCTAssertEqual(store.previewImageSize, CGSize(width: 10, height: 20))
        ExplorerURLProtocol.complete(obsolete, data: Data("invalid".utf8))
        store.bind(workspaceId: "other-\(workspace)")
        store.openPreview("cached.png")
        store.loadPreviewImage("cached.png")
        let other = try await pending("/file", raw: true)[0]
        ExplorerURLProtocol.complete(other, data: makePNG(pixelsWide: 30, pixelsHigh: 40))
        await eventually { store.previewImageSize == CGSize(width: 30, height: 40) }
        XCTAssertFalse(store.previewImageFailed)
        store.bind(workspaceId: workspace)
        store.openPreview("cached.png")
        store.loadPreviewImage("cached.png")
        XCTAssertTrue(store.previewImage === cached)
        store.bind(workspaceId: nil)
    }

    // MARK: Image decode (off-actor, downsampled)

    private func makePNG(pixelsWide: Int, pixelsHigh: Int) -> Data {
        let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: pixelsWide, pixelsHigh: pixelsHigh,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0,
        )!
        return rep.representation(using: .png, properties: [:])!
    }

    func testDecodePreviewDownsamplesLargeImages() {
        let decoded = WorkspaceExplorerStore.decodePreview(makePNG(pixelsWide: 4000, pixelsHigh: 3000))
        XCTAssertNotNil(decoded)
        // The header reports the SOURCE pixels while the display
        // bitmap is capped at the 2560px ceiling (aspect preserved).
        XCTAssertEqual(decoded?.sourceSize, CGSize(width: 4000, height: 3000))
        XCTAssertEqual(decoded?.cgImage.width, 2560)
        XCTAssertEqual(decoded?.cgImage.height, 1920)
    }

    func testDecodePreviewKeepsSmallImagesIntact() {
        let decoded = WorkspaceExplorerStore.decodePreview(makePNG(pixelsWide: 10, pixelsHigh: 20))
        XCTAssertEqual(decoded?.cgImage.width, 10)
        XCTAssertEqual(decoded?.cgImage.height, 20)
    }

    func testDecodePreviewRejectsGarbage() {
        XCTAssertNil(WorkspaceExplorerStore.decodePreview(Data("not an image".utf8)))
    }
}
