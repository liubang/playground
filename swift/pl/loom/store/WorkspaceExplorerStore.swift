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
import Foundation
import ImageIO

/// Right-panel model: the workspace explorer (git changes + file
/// tree/preview) bound to the selected session's workspace, falling
/// back to the default workspace when nothing is selected.
///
/// Every byte comes from the server's workspace-explorer endpoints —
/// deliberately never from the local filesystem, so a remote
/// `loom serve` behaves identically to the embedded one. The surface
/// is read-only because the server's is (workspace-confined, no write
/// or commit entry points).
///
/// Refresh model (WebUI RightPanel parity): git is the source of truth
/// for changes; transcript activity (tool.completed / turn.finished,
/// via SessionStore.onFileActivity) only schedules a coalesced
/// revalidation — git status plus an ETag-conditional reload of every
/// expanded directory, where 304 answers keep the cached tree warm.
@MainActor
@Observable
final class WorkspaceExplorerStore {
    enum Tab: String, CaseIterable, Identifiable {
        case changes = "Changes"
        case files = "Files"

        var id: String {
            rawValue
        }
    }

    /// Drill-down inside the panel: the tabbed list, or one pushed
    /// detail (diff / file preview). One level deep — a detail never
    /// pushes another, it just replaces.
    enum Route: Hashable {
        case list
        case diff(String)
        case preview(String)
    }

    let api: APIClient

    private(set) var workspaceId: String?
    var tab: Tab = .changes
    private(set) var route: Route = .list

    init(api: APIClient) {
        self.api = api
    }

    private struct RequestIdentity: Equatable {
        let workspaceId: String
        let generation: UInt64
        let sequence: UInt64
    }

    private var bindingGeneration: UInt64 = 0
    private var requestSequence: UInt64 = 0

    private func nextRequest() -> RequestIdentity? {
        guard let workspaceId else { return nil }
        requestSequence += 1
        return RequestIdentity(
            workspaceId: workspaceId, generation: bindingGeneration, sequence: requestSequence,
        )
    }

    private func isCurrent(_ request: RequestIdentity, _ active: RequestIdentity?) -> Bool {
        request == active && request.generation == bindingGeneration
            && request.workspaceId == workspaceId
    }

    // MARK: Binding

    /// Points the panel at another workspace. Selection-scoped UI
    /// (route, expansion, search) resets; the directory cache is keyed
    /// per workspace and stays warm for the switch back.
    func bind(workspaceId: String?) {
        guard workspaceId != self.workspaceId else { return }
        bindingGeneration += 1
        self.workspaceId = workspaceId
        gitRequest = nil
        gitLoading = false
        statusByPath = [:]
        dirRequests = [:]
        loadingDirs = []
        activityTask?.cancel()
        activityTask = nil
        resetDetail()
        route = .list
        expanded = []
        searchQuery = ""
        searchResults = []
        searchTruncated = false
        git = nil
        gitError = nil
        guard workspaceId != nil else { return }
        let generation = bindingGeneration
        loadDir("")
        Task { [weak self] in
            guard let self, bindingGeneration == generation else { return }
            await refreshGit()
        }
    }

    // MARK: Git status (changes tab, badge, tree colouring)

    private(set) var git: WorkspaceGitStatus?
    private(set) var gitError: String?
    private(set) var gitLoading = false
    /// path → status letter, derived from the last status fetch; the
    /// file tree colours names from it.
    private(set) var statusByPath: [String: String] = [:]

    var changeCount: Int {
        guard let git, git.isGit else { return 0 }
        return git.files?.count ?? 0
    }

    private var gitRequest: RequestIdentity?

    func refreshGit() async {
        guard let request = nextRequest() else { return }
        gitRequest = request
        gitLoading = true
        defer {
            if isCurrent(request, gitRequest) {
                gitLoading = false
            }
        }
        do {
            let status = try await api.workspaceGitStatus(request.workspaceId)
            guard isCurrent(request, gitRequest) else { return }
            git = status
            statusByPath = Dictionary(
                (status.files ?? []).map { ($0.path, $0.status) },
                uniquingKeysWith: { _, last in last },
            )
            gitError = nil
        } catch {
            guard isCurrent(request, gitRequest) else { return }
            gitError = error.localizedDescription
        }
    }

    private var activityTask: Task<Void, Never>?

    /// SessionStore.onFileActivity hook: tool.completed / turn.finished
    /// may have touched workspace files. Turns fire tool completions in
    /// bursts, so revalidation is coalesced behind a short delay — one
    /// git status + one conditional reload per expanded directory per
    /// burst, not per event.
    func noteFileActivity() {
        guard workspaceId != nil else { return }
        activityTask?.cancel()
        let generation = bindingGeneration
        activityTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(600))
            guard !Task.isCancelled, let self, bindingGeneration == generation else { return }
            await refreshGit()
            guard !Task.isCancelled, bindingGeneration == generation else { return }
            revalidateLoadedDirs()
        }
    }

    /// Window-refocus hook: the user's own edits in another app (or
    /// another loom client) never produce local events.
    func noteAppFocus() {
        guard workspaceId != nil else { return }
        let generation = bindingGeneration
        Task { [weak self] in
            guard let self, bindingGeneration == generation else { return }
            await refreshGit()
        }
    }

    // MARK: File tree (files tab)

    struct DirectoryState: Sendable {
        var entries: [WorkspaceFileEntry]
        var etag: String?
        var truncated: Bool
    }

    /// Directory listings keyed "wsId\0path": warm across workspace
    /// switches, revalidated conditionally (ETag) on file activity.
    private var directories: [String: DirectoryState] = [:]
    private var dirRequests: [String: RequestIdentity] = [:]
    private(set) var loadingDirs: Set<String> = []
    private(set) var expanded: Set<String> = []

    private func dirKey(_ path: String) -> String {
        "\(workspaceId ?? "")\u{0}\(path)"
    }

    func entries(for path: String) -> DirectoryState? {
        directories[dirKey(path)]
    }

    func isLoading(_ path: String) -> Bool {
        loadingDirs.contains(path)
    }

    func toggleDir(_ path: String) {
        if expanded.contains(path) {
            expanded.remove(path)
        } else {
            expanded.insert(path)
            if entries(for: path) == nil {
                loadDir(path)
            }
        }
    }

    /// Fetches a directory listing, conditional on the cached ETag when
    /// one exists. In-flight requests for the same workspace/path are deduped
    /// (revalidation walks overlap free-form toggles).
    func loadDir(_ path: String) {
        let key = dirKey(path)
        guard dirRequests[key] == nil, let request = nextRequest() else { return }
        dirRequests[key] = request
        loadingDirs.insert(path)
        let cachedEtag = directories[key]?.etag
        Task { [weak self] in
            defer {
                if let self, isCurrent(request, dirRequests[key]) {
                    dirRequests.removeValue(forKey: key)
                    loadingDirs.remove(path)
                }
            }
            do {
                guard let self else { return }
                let result = try await api.workspaceFiles(
                    request.workspaceId, path: path, ifNoneMatch: cachedEtag,
                )
                guard isCurrent(request, dirRequests[key]) else { return }
                if case let .modified(response, etag) = result {
                    directories[key] = DirectoryState(
                        entries: response.entries, etag: etag,
                        truncated: response.truncated ?? false,
                    )
                }
            } catch {
                // A directory deleted between expand and fetch collapses
                // quietly; the next file-activity revalidation retries.
            }
        }
    }

    /// ETag-revalidates the root and every expanded directory that has
    /// a cached listing (304 → cache kept, no re-render).
    private func revalidateLoadedDirs() {
        loadDir("")
        for path in expanded where directories[dirKey(path)] != nil {
            loadDir(path)
        }
    }

    struct TreeNode: Identifiable, Hashable {
        let entry: WorkspaceFileEntry
        let depth: Int

        var id: String {
            entry.path
        }
    }

    /// The expanded prefix of the tree flattened for a single lazy
    /// list; expanded-but-unloaded directories contribute their row
    /// (with a spinner) and nothing below until the listing lands.
    var visibleNodes: [TreeNode] {
        var out: [TreeNode] = []
        collect(into: &out, path: "", depth: 0)
        return out
    }

    private func collect(into out: inout [TreeNode], path: String, depth: Int) {
        guard let state = entries(for: path) else { return }
        for entry in state.entries {
            out.append(TreeNode(entry: entry, depth: depth))
            if entry.kind == .dir, expanded.contains(entry.path) {
                collect(into: &out, path: entry.path, depth: depth + 1)
            }
        }
    }

    // MARK: Fuzzy search (files tab)

    var searchQuery = "" {
        didSet { scheduleSearch() }
    }

    private(set) var searchResults: [WorkspaceFileMatch] = []
    private(set) var searchTruncated = false
    private(set) var searching = false
    private var searchRequest: RequestIdentity?
    @ObservationIgnored private var searchTask: Task<Void, Never>?

    deinit {
        searchTask?.cancel()
    }

    private func scheduleSearch() {
        searchRequest = nil
        searchTask?.cancel()
        searchTask = nil
        let q = searchQuery.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty, let request = nextRequest() else {
            searchResults = []
            searchTruncated = false
            searching = false
            return
        }
        searchRequest = request
        searching = true
        searchTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(250))
            guard !Task.isCancelled, let self, isCurrent(request, searchRequest) else { return }
            defer {
                if isCurrent(request, searchRequest) {
                    searching = false
                    searchTask = nil
                }
            }
            do {
                let response = try await api.searchWorkspaceFiles(request.workspaceId, query: q)
                guard !Task.isCancelled, isCurrent(request, searchRequest) else { return }
                searchResults = response.matches
                searchTruncated = response.truncated ?? false
            } catch {
                // Keep the previous results; the tree stays usable.
            }
        }
    }

    /// Search-result action for a directory: expand it (and every
    /// ancestor, loading each level) and leave search so the tree
    /// shows the revealed node.
    func revealInTree(_ match: WorkspaceFileMatch) {
        guard match.kind == .dir else { return }
        var prefix = ""
        for component in match.path.split(separator: "/") {
            prefix = prefix.isEmpty ? String(component) : "\(prefix)/\(component)"
            if expanded.insert(prefix).inserted, entries(for: prefix) == nil {
                loadDir(prefix)
            }
        }
        searchQuery = ""
    }

    // MARK: Detail (drill-down)

    private(set) var preview: WorkspaceFileContent?
    private(set) var previewLoading = false
    private(set) var previewError: String?
    private(set) var diff: WorkspaceGitDiff?
    private(set) var diffLoading = false
    private(set) var diffError: String?

    private var detailRequest: RequestIdentity?
    private var imageRequest: RequestIdentity?

    private func resetDetail() {
        detailRequest = nil
        imageRequest = nil
        preview = nil
        previewError = nil
        previewLoading = false
        diff = nil
        diffError = nil
        diffLoading = false
        previewImageTask?.cancel()
        previewImageTask = nil
        previewImage = nil
        previewImageSize = nil
        previewImageFailed = false
    }

    func openPreview(_ path: String) {
        resetDetail()
        route = .preview(path)
        guard let request = nextRequest() else { return }
        detailRequest = request
        previewLoading = true
        Task { [weak self] in
            guard let self else { return }
            defer {
                if isCurrent(request, detailRequest) {
                    previewLoading = false
                }
            }
            do {
                let content = try await api.workspaceFileContent(request.workspaceId, path: path)
                guard isCurrent(request, detailRequest) else { return }
                preview = content
            } catch {
                guard isCurrent(request, detailRequest) else { return }
                previewError = error.localizedDescription
            }
        }
    }

    func openDiff(_ path: String) {
        resetDetail()
        route = .diff(path)
        guard let request = nextRequest() else { return }
        detailRequest = request
        diffLoading = true
        Task { [weak self] in
            guard let self else { return }
            defer {
                if isCurrent(request, detailRequest) {
                    diffLoading = false
                }
            }
            do {
                let result = try await api.workspaceGitDiff(request.workspaceId, path: path)
                guard isCurrent(request, detailRequest) else { return }
                diff = result
            } catch {
                guard isCurrent(request, detailRequest) else { return }
                diffError = error.localizedDescription
            }
        }
    }

    func back() {
        resetDetail()
        route = .list
    }

    // MARK: Image preview (binary image files)

    private(set) var previewImage: NSImage?
    /// The source file's true pixel size, for the header: the display
    /// bitmap itself is downsampled, so image.size under-reports.
    private(set) var previewImageSize: CGSize?
    private(set) var previewImageFailed = false
    private var previewImageTask: Task<Void, Never>?

    /// Off-actor decode result. CGImage crosses the actor boundary
    /// (immutable, Sendable); the NSImage wrapper is built on main.
    struct DecodedPreview {
        let cgImage: CGImage
        let sourceSize: CGSize
        let decodedCost: Int
    }

    /// Cache entries are keyed by workspace AND path: two workspaces
    /// may share a relative path with entirely different bytes.
    private final class CachedPreview {
        let image: NSImage
        let sourceSize: CGSize
        init(image: NSImage, sourceSize: CGSize) {
            self.image = image
            self.sourceSize = sourceSize
        }
    }

    /// Display images across previews, budgeted by DECODED bytes: the
    /// fetch is the expensive part of opening an image, and
    /// back-navigation is common.
    private enum ImageCache {
        static let shared: NSCache<NSString, CachedPreview> = {
            let cache = NSCache<NSString, CachedPreview>()
            cache.totalCostLimit = 64 * 1024 * 1024
            return cache
        }()
    }

    /// Decodes raw image bytes into a display bitmap OFF the main
    /// actor, downsampled to a ceiling: a 16 MB photo inflates to
    /// ~100 MB of pixels at full size, which both hitches the main
    /// thread at draw time and busts the cache budget. 2560 px covers
    /// panel fit and lightbox zoom on Retina; smaller images pass
    /// through untouched.
    nonisolated static func decodePreview(_ data: Data) -> DecodedPreview? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        var sourceSize = CGSize.zero
        if let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
           let width = props[kCGImagePropertyPixelWidth] as? Int,
           let height = props[kCGImagePropertyPixelHeight] as? Int
        {
            sourceSize = CGSize(width: width, height: height)
        }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: 2560,
            kCGImageSourceCreateThumbnailWithTransform: true,
        ]
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary),
              cgImage.width > 0 else { return nil }
        if sourceSize == .zero {
            sourceSize = CGSize(width: cgImage.width, height: cgImage.height)
        }
        return DecodedPreview(
            cgImage: cgImage, sourceSize: sourceSize,
            decodedCost: cgImage.bytesPerRow * cgImage.height,
        )
    }

    /// Fetches the raw bytes of an image preview (the JSON preview of
    /// a binary file carries no content). Triggered by the preview
    /// view when the path's extension is a raster image type. Rapid
    /// navigation cancels the superseded fetch/decode.
    func loadPreviewImage(_ path: String) {
        guard route == .preview(path), let request = nextRequest() else { return }
        imageRequest = request
        previewImageTask?.cancel()
        previewImageTask = nil
        let key = "\(request.workspaceId)\n\(path)" as NSString
        if let cached = ImageCache.shared.object(forKey: key) {
            previewImage = cached.image
            previewImageSize = cached.sourceSize
            previewImageFailed = false
            return
        }
        previewImage = nil
        previewImageSize = nil
        previewImageFailed = false
        previewImageTask = Task { [weak self] in
            do {
                guard let self else { return }
                let (data, _) = try await api.workspaceFileRaw(request.workspaceId, path: path)
                try Task.checkCancellation()
                guard isCurrent(request, imageRequest), route == .preview(path) else { return }
                let decode = Task.detached(priority: .userInitiated) { Self.decodePreview(data) }
                let decoded = await decode.value
                try Task.checkCancellation()
                guard isCurrent(request, imageRequest), route == .preview(path) else { return }
                guard let decoded else {
                    // Undecodable payload (e.g. an SVG served raw):
                    // the view falls back to its placeholder.
                    previewImageFailed = true
                    return
                }
                let image = NSImage(
                    cgImage: decoded.cgImage,
                    size: NSSize(width: decoded.cgImage.width, height: decoded.cgImage.height),
                )
                ImageCache.shared.setObject(
                    CachedPreview(image: image, sourceSize: decoded.sourceSize),
                    forKey: key, cost: decoded.decodedCost,
                )
                previewImage = image
                previewImageSize = decoded.sourceSize
            } catch {
                guard !Task.isCancelled, let self,
                      isCurrent(request, imageRequest), route == .preview(path) else { return }
                previewImageFailed = true
            }
        }
    }
}
