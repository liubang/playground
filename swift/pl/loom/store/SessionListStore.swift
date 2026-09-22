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

    init(api: APIClient) {
        self.api = api
    }

    func load() async {
        startPolling()
        await withTaskGroup(of: Void.self) { group in
            group.addTask { await self.loadWorkspaces() }
            group.addTask { await self.loadSessions() }
            group.addTask { await self.loadModels() }
        }
    }

    func loadWorkspaces() async {
        do {
            workspaces = try await api.listWorkspaces().workspaces
        } catch {
            loadError = error.localizedDescription
        }
    }

    func loadSessions() async {
        isLoading = true
        defer { isLoading = false }
        do {
            let response = try await api.listSessions(workspaceId: "all", archived: showArchived)
            sessions = response.sessions.sorted { ($0.updatedAt ?? .distantPast) > ($1.updatedAt ?? .distantPast) }
            loadError = nil
        } catch {
            loadError = error.localizedDescription
        }
    }

    /// Coalesced list refresh — turn events (prompt submitted, turn
    /// finished, approval requested/resolved) arrive in bursts, and
    /// each one can change the derived title / state shown in the
    /// sidebar and header (WebUI: those events all call
    /// refreshSessions).
    private var refreshScheduled = false

    func scheduleSessionsRefresh() {
        guard !refreshScheduled else { return }
        refreshScheduled = true
        Task {
            try? await Task.sleep(for: .milliseconds(300))
            refreshScheduled = false
            await loadSessions()
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
                try? await Task.sleep(for: .seconds(15))
                guard !Task.isCancelled else { return }
                await self?.loadSessions()
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
        do {
            let catalog = try await api.metaModels()
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
        stores[sessionId] = store
        store.start()
        return store
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

    func deleteSession(_ sessionId: String) async {
        do {
            stores[sessionId]?.stop()
            stores[sessionId] = nil
            try await api.deleteSession(sessionId)
            await loadSessions()
        } catch {
            loadError = error.localizedDescription
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
            await loadWorkspaces()
            await loadSessions()
        } catch {
            loadError = error.localizedDescription
        }
    }
}
