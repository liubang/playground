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

    static func reset() {
        lock.lock()
        requests = []
        reply = nil
        lock.unlock()
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
        guard let reply = Self.reply else {
            client?.urlProtocol(self, didFailWithError: URLError(.unknown))
            return
        }
        let (status, headers, body) = reply(request)
        Self.lock.lock()
        Self.requests.append(request)
        Self.lock.unlock()
        let response = HTTPURLResponse(
            url: request.url!, statusCode: status, httpVersion: nil, headerFields: headers,
        )!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: Data(body.utf8))
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
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
