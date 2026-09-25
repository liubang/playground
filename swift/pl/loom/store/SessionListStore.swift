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

import Foundation

/// Sidebar model: workspace tree + session list (the WebUI loads all
/// sessions once and groups them client-side per workspace), the model
/// catalog for the composer picker, plus per-session SessionStore
/// caching so switching between sessions keeps their live SSE
/// projections warm.
@MainActor
@Observable
final class SessionListStore {
    let api: APIClient

    private(set) var workspaces: [Workspace] = []
    private(set) var sessions: [SessionSummary] = []
    private(set) var isLoading = false
    /// Sidebar view switch (WebUI showArchived): the archived view is a
    /// read-only history listing; rows offer unarchive instead.
    private(set) var showArchived = false
    var loadError: String?

    /// Composer model picker catalog (GET /v1/meta/models).
    private(set) var models: [MetaModels.ModelInfo] = []
    private(set) var defaultModelRef: String?

    /// Live stores keyed by session id; created lazily on selection and
    /// reused across switches so a session keeps streaming in background.
    private var stores: [String: SessionStore] = [:]
    private var stopped = false
    private var sessionsRequest = 0
    private var refreshTask: Task<Void, Never>?
    private var refreshAgain = false

    func stop() {
        stopped = true
        sessionsRequest += 1
        pollTask?.cancel()
        pollTask = nil
        refreshTask?.cancel()
        refreshTask = nil
        refreshScheduled = false
        refreshAgain = false
        for store in stores.values {
            store.stop()
        }
        stores.removeAll()
    }

    init(api: APIClient) {
        self.api = api
    }

    func load() async {
        guard !stopped else { return }
        startPolling()
        await withTaskGroup(of: Void.self) { group in
            group.addTask { await self.loadWorkspaces() }
            group.addTask { await self.loadSessions() }
            group.addTask { await self.loadModels() }
        }
    }

    func loadWorkspaces() async {
        guard !stopped else { return }
        do {
            let response = try await api.listWorkspaces()
            guard !stopped, !Task.isCancelled else { return }
            workspaces = response.workspaces
        } catch {
            if !stopped, !Task.isCancelled {
                loadError = error.localizedDescription
            }
        }
    }

    func loadSessions() async {
        guard !stopped else { return }
        sessionsRequest += 1
        let request = sessionsRequest
        let archived = showArchived
        isLoading = true
        defer {
            if request == sessionsRequest {
                isLoading = false
            }
        }
        do {
            var all: [SessionSummary] = []
            var cursor: String?
            var seenCursors = Set<String>()
            repeat {
                let response = try await api.listSessions(
                    workspaceId: "all", archived: archived, cursor: cursor,
                )
                guard !stopped, request == sessionsRequest, !Task.isCancelled else { return }
                all.append(contentsOf: response.sessions)
                // The server uses an empty string (not null) for the last page.
                cursor = response.nextCursor.flatMap { $0.isEmpty ? nil : $0 }
                if let cursor, !seenCursors.insert(cursor).inserted {
                    throw SessionListError.repeatedCursor
                }
            } while cursor != nil
            var seenIds = Set<String>()
            sessions = all.filter { seenIds.insert($0.id).inserted }
                .sorted { ($0.updatedAt ?? .distantPast) > ($1.updatedAt ?? .distantPast) }
            loadError = nil
        } catch {
            if !stopped, request == sessionsRequest, !Task.isCancelled {
                loadError = error.localizedDescription
            }
        }
    }

    private enum SessionListError: LocalizedError {
        case repeatedCursor

        var errorDescription: String? {
            "Session pagination returned a repeated cursor"
        }
    }

    /// Coalesced list refresh — turn events (prompt submitted, turn
    /// finished, approval requested/resolved) arrive in bursts, and
    /// each one can change the derived title / state shown in the
    /// sidebar and header (WebUI: those events all call
    /// refreshSessions).
    private var refreshScheduled = false

    func scheduleSessionsRefresh() {
        guard !stopped else { return }
        if refreshScheduled {
            refreshAgain = true
            return
        }
        refreshScheduled = true
        refreshTask = Task { [weak self] in
            do {
                try await Task.sleep(for: .milliseconds(300))
                guard let self, !Task.isCancelled, !stopped else { return }
                // Changes during the debounce are included in this request;
                // only changes after it starts require another fetch.
                refreshAgain = false
                await loadSessions()
                let again = refreshAgain
                refreshScheduled = false
                refreshTask = nil
                if again {
                    scheduleSessionsRefresh()
                }
            } catch {
                self?.refreshScheduled = false
                self?.refreshTask = nil
            }
        }
    }

    /// WebUI parity: a 15s polling backstop picks up changes made by
    /// OTHER clients (TUI / WebUI on the same server) that no local
    /// event can observe.
    private var pollTask: Task<Void, Never>?

    private func startPolling() {
        guard pollTask == nil else { return }
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(15)) } catch { return }
                guard !Task.isCancelled, let self, !self.stopped else { return }
                await loadSessions()
            }
        }
    }

    /// WebUI toggleArchivedView: swap the listing between active and
    /// archived sessions.
    func toggleArchivedView() async {
        showArchived.toggle()
        await loadSessions()
    }

    /// Restores an archived session back to the active listing.
    func unarchiveSession(_ sessionId: String) async {
        do {
            try await api.archiveSession(sessionId, archived: false)
            await loadSessions()
        } catch {
            loadError = error.localizedDescription
        }
    }

    func loadModels() async {
        guard !stopped else { return }
        do {
            let catalog = try await api.metaModels()
            guard !stopped, !Task.isCancelled else { return }
            models = catalog.models
            defaultModelRef = catalog.default
            // Live stores read the default ref for their picker
            // checkmark fallback (WebUI applySnapshotMeta).
            for store in stores.values {
                store.defaultModelRef = defaultModelRef
            }
        } catch {
            // The picker simply stays on the session's current model.
        }
    }

    // MARK: Grouping (WebUI sidebar: one group per workspace)

    /// Sessions belonging to a workspace, newest first.
    func sessions(for workspaceId: String) -> [SessionSummary] {
        sessions.filter { $0.workspaceId == workspaceId }
    }

    func workspace(for id: String?) -> Workspace? {
        guard let id else { return nil }
        return workspaces.first { $0.id == id }
    }

    // MARK: Stores

    func store(for sessionId: String) -> SessionStore {
        if let existing = stores[sessionId] {
            // A backgrounded store is paused (setActive); selecting it
            // again resumes the snapshot+stream handshake. No-op while
            // already running.
            existing.start()
            return existing
        }
        let workspaceId = sessions.first { $0.id == sessionId }?.workspaceId
        let store = SessionStore(sessionId: sessionId, workspaceId: workspaceId, api: api)
        store.defaultModelRef = defaultModelRef
        // The store's turn activity (first prompt → derived title,
        // turn end → state) feeds the list the sidebar renders.
        store.onTurnActivity = { [weak self] in
            self?.scheduleSessionsRefresh()
        }
        if !stopped {
            stores[sessionId] = store
            store.start()
        }
        return store
    }

    /// Keeps only the selected session's store streaming. Background
    /// stores keep their UI state (drafts, transcript rows) but pause
    /// their event loops: every started store otherwise pins a stream
    /// connection and a server-side subscriber for the rest of the app's
    /// lifetime, so a long clicking session accumulates one live stream
    /// per visited session.
    func setActive(_ sessionId: String?) {
        for (id, store) in stores where id != sessionId {
            store.pause()
        }
    }

    // MARK: Session commands

    @discardableResult
    func newSession(workspaceId: String? = nil) async -> String? {
        do {
            let response = try await api.createSession(workspaceId: workspaceId)
            await loadSessions()
            return response.sessionId
        } catch {
            loadError = error.localizedDescription
            return nil
        }
    }

    @discardableResult
    func deleteSession(_ sessionId: String) async -> Bool {
        do {
            try await api.deleteSession(sessionId)
            stores[sessionId]?.stop()
            stores[sessionId] = nil
            await loadSessions()
            return true
        } catch {
            loadError = error.localizedDescription
            return false
        }
    }

    /// Archives a session (it leaves the default listing; the WebUI's
    /// sidebar shows it only in the archived view).
    func archiveSession(_ sessionId: String) async {
        do {
            try await api.archiveSession(sessionId, archived: true)
            await loadSessions()
        } catch {
            loadError = error.localizedDescription
        }
    }

    // MARK: Workspace commands

    @discardableResult
    func addWorkspace(rootPath: String, name: String) async -> Workspace? {
        do {
            let workspace = try await api.registerWorkspace(rootPath: rootPath, name: name)
            await loadWorkspaces()
            return workspace
        } catch {
            loadError = error.localizedDescription
            return nil
        }
    }

    func deleteWorkspace(_ workspaceId: String) async {
        do {
            try await api.deleteWorkspace(workspaceId)
            for (id, store) in stores where store.workspaceId == workspaceId {
                store.stop()
                stores[id] = nil
            }
            await loadWorkspaces()
            await loadSessions()
        } catch {
            loadError = error.localizedDescription
        }
    }
}
