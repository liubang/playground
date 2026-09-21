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
import SwiftUI

/// App shell (webui shell.css): a fixed 288px bg1 sidebar with a bg2
/// hairline on its right edge, and the main column (header / transcript
/// / composer / statusbar) — all flat --bg0, no native split-view chrome.
struct RootView: View {
    @Bindable var appState: AppState
    @State private var selection: String?
    /// The singleton image lightbox (WebUI lightbox): any zoomable
    /// image posts .loomZoomImage; the overlay lives at window level
    /// so it covers the sidebar + transcript + composer alike.
    @State private var lightboxImage: NSImage?
    /// Non-nil while the settings panel (config.yaml editor) is up;
    /// a fresh store per open keeps stale state out of the sheet.
    @State private var settingsStore: SettingsStore?
    @AppStorage("loom.sidebarCollapsed") private var sidebarCollapsed = false

    var body: some View {
        Group {
            switch appState.status {
            case let .connected(version):
                if let list = appState.sessionList {
                    shell(list: list, version: version)
                }
            default:
                ConnectView(appState: appState)
            }
        }
        .background(Theme.bg0)
        .overlay {
            if let image = lightboxImage {
                ImageLightboxView(image: image) { lightboxImage = nil }
            }
        }
        .animation(.easeInOut(duration: 0.15), value: lightboxImage != nil)
        .onReceive(NotificationCenter.default.publisher(for: .loomZoomImage)) { note in
            lightboxImage = note.object as? NSImage
        }
        .onReceive(NotificationCenter.default.publisher(for: .loomNewSession)) { _ in
            guard let list = appState.sessionList else { return }
            Task {
                let workspaceId = selection.flatMap { id in
                    list.sessions.first { $0.id == id }?.workspaceId
                }
                if let id = await list.newSession(workspaceId: workspaceId) {
                    selection = id
                }
            }
        }
        .task { appState.start() }
        // WebUI parity: regaining focus refreshes the session list —
        // other clients may have advanced sessions in the meantime.
        .onReceive(NotificationCenter.default.publisher(
            for: NSApplication.didBecomeActiveNotification,
        )) { _ in
            guard let list = appState.sessionList else { return }
            Task { await list.loadSessions() }
        }
        .onReceive(NotificationCenter.default.publisher(
            for: NSApplication.willTerminateNotification,
        )) { _ in
            appState.server.stop()
        }
    }

    private func shell(list: SessionListStore, version: String) -> some View {
        HStack(spacing: 0) {
            // The landing page is a dead end without the sidebar (its
            // only navigation IS the session list, and the sidebar
            // toggle lives in the chat header), so a collapsed sidebar
            // is force-shown while no session is selected.
            if !sidebarCollapsed || selection == nil {
                SidebarView(
                    list: list,
                    selection: $selection,
                    onDisconnect: {
                        appState.disconnect()
                        selection = nil
                    },
                    onOpenSettings: {
                        let store = SettingsStore(api: list.api)
                        // WebUI controller.refreshModelCatalog: saving
                        // config may change the composer's model catalog.
                        store.onConfigSaved = {
                            Task { await list.loadModels() }
                        }
                        settingsStore = store
                    },
                )
                .frame(width: Theme.sidebarWidth)
                .transition(.move(edge: .leading))
                Hairline(axis: .vertical)
            }

            Group {
                if let sessionId = selection {
                    ChatView(
                        store: list.store(for: sessionId),
                        sessionTitle: list.sessions.first { $0.id == sessionId }?.title,
                        version: version,
                        models: list.models,
                        archived: list.showArchived,
                        sidebarCollapsed: $sidebarCollapsed,
                    )
                    .id(sessionId)
                } else {
                    emptyState
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .animation(.easeInOut(duration: 0.18), value: sidebarCollapsed)
        .animation(.easeInOut(duration: 0.18), value: selection == nil)
        .sheet(isPresented: Binding(
            get: { settingsStore != nil },
            set: { if !$0 { settingsStore = nil } },
        )) {
            if let store = settingsStore {
                SettingsView(store: store) { settingsStore = nil }
            }
        }
    }

    /// The WebUI's .empty-state: centered brand + hint.
    private var emptyState: some View {
        VStack(spacing: 8) {
            Text("◆ loom")
                .font(.system(size: 22, weight: .bold))
                .foregroundStyle(Theme.primary)
            Text("Pick a session on the left, or start a new one with ⌘N.")
                .font(.system(size: Theme.textMd))
                .foregroundStyle(Theme.muted)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg0)
    }
}
