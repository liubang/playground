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

/// App shell (webui shell.css): a bg1 sidebar (default 288px,
/// drag-resizable via its hairline edge, width persisted) and the main
/// column (header / transcript / composer / statusbar) — all flat
/// --bg0, no native split-view chrome.
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
    @AppStorage("loom.sidebarWidth") private var sidebarWidth = Theme.sidebarWidth

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
            Task { await newSession(in: list) }
        }
        .onReceive(NotificationCenter.default.publisher(for: .loomOpenSettings)) { _ in
            guard let list = appState.sessionList else { return }
            openSettings(list: list)
        }
        .onReceive(NotificationCenter.default.publisher(for: .loomPrevSession)) { _ in
            moveSelection(by: -1)
        }
        .onReceive(NotificationCenter.default.publisher(for: .loomNextSession)) { _ in
            moveSelection(by: 1)
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

    /// Creates a session in the current selection's workspace and
    /// selects it (shared by ⌘N, the sidebar button, and the empty
    /// state's call-to-action).
    private func newSession(in list: SessionListStore) async {
        let workspaceId = selection.flatMap { id in
            list.sessions.first { $0.id == id }?.workspaceId
        }
        if let id = await list.newSession(workspaceId: workspaceId) {
            selection = id
        }
    }

    /// Opens the settings panel (config.yaml editor); a fresh store
    /// per open keeps stale state out of the sheet. Shared by the
    /// sidebar gear and ⌘,.
    private func openSettings(list: SessionListStore) {
        let store = SettingsStore(api: list.api)
        // WebUI controller.refreshModelCatalog: saving config may
        // change the composer's model catalog.
        store.onConfigSaved = {
            Task { await list.loadModels() }
        }
        settingsStore = store
    }

    /// ⌘[ / ⌘]: step through the sidebar's session order (wrapping).
    private func moveSelection(by delta: Int) {
        guard let list = appState.sessionList, !list.sessions.isEmpty else { return }
        let ids = list.sessions.map(\.id)
        let current = selection.flatMap { ids.firstIndex(of: $0) } ?? (delta > 0 ? -1 : 0)
        let next = (current + delta + ids.count) % ids.count
        selection = ids[next]
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
                    onOpenSettings: { openSettings(list: list) },
                )
                .frame(width: sidebarWidth)
                .transition(.move(edge: .leading))
                SidebarDivider(width: $sidebarWidth)
            }

            Group {
                if let sessionId = selection {
                    ChatView(
                        store: list.store(for: sessionId),
                        sessionTitle: list.sessions.first { $0.id == sessionId }?.title,
                        workspaceName: list.workspace(
                            for: list.sessions.first { $0.id == sessionId }?.workspaceId,
                        )?.name,
                        version: version,
                        models: list.models,
                        archived: list.showArchived,
                        sidebarCollapsed: $sidebarCollapsed,
                    )
                    .id(sessionId)
                } else {
                    emptyState(list: list)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .animation(.easeInOut(duration: 0.18), value: sidebarCollapsed)
        .animation(.easeInOut(duration: 0.18), value: selection == nil)
        .sheet(isPresented: Binding(
            get: { settingsStore != nil },
            set: {
                if !$0 {
                    settingsStore = nil
                }
            },
        )) {
            if let store = settingsStore {
                SettingsView(store: store) { settingsStore = nil }
            }
        }
    }

    /// The WebUI's .empty-state: centered brand + hint + the primary
    /// call-to-action (the sidebar is force-shown here, but a visible
    /// button beats discovering ⌘N or the sidebar's New session row).
    private func emptyState(list: SessionListStore) -> some View {
        VStack(spacing: 8) {
            Text("◆ loom")
                .font(.system(size: 22, weight: .bold))
                .foregroundStyle(Theme.primary)
            Text("Pick a session on the left, or start a new one.")
                .font(.system(size: Theme.textMd))
                .foregroundStyle(Theme.muted)
            Button {
                Task { await newSession(in: list) }
            } label: {
                Label("New Session", systemImage: "plus")
            }
            .buttonStyle(PrimaryButtonStyle())
            .padding(.top, 10)
            Text("⌘N")
                .font(Theme.monoXs)
                .foregroundStyle(Theme.muted.opacity(0.7))
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg0)
        .windowDragSurface()
    }
}

/// The sidebar's right edge: a 1pt hairline carrying a 9pt invisible
/// hit strip (overlay, so the layout stays 1pt). Dragging resizes the
/// sidebar within Theme.sidebarMinWidth…sidebarMaxWidth; the resize
/// cursor shows on hover.
private struct SidebarDivider: View {
    @Binding var width: Double
    @GestureState private var dragStart: Double?

    var body: some View {
        Hairline(axis: .vertical)
            .overlay {
                NonDraggableStrip()
                    .frame(width: 9)
                    .contentShape(Rectangle())
                    .onHover { inside in
                        if inside {
                            NSCursor.resizeLeftRight.push()
                        } else {
                            NSCursor.pop()
                        }
                    }
                    .gesture(
                        DragGesture(minimumDistance: 1)
                            .updating($dragStart) { _, state, _ in
                                if state == nil {
                                    state = width
                                }
                            }
                            .onChanged { value in
                                guard let start = dragStart else { return }
                                var transaction = Transaction()
                                transaction.disablesAnimations = true
                                withTransaction(transaction) {
                                    width = min(
                                        Theme.sidebarMaxWidth,
                                        max(Theme.sidebarMinWidth, start + value.translation.width),
                                    )
                                }
                            },
                    )
            }
    }
}

/// The hit strip's AppKit backing. The window is movable by its
/// background (hiddenTitleBar has no grab chrome), and a plain SwiftUI
/// drag region does NOT opt out of that: the window move and the
/// divider's DragGesture fired at once — the window slid while the
/// width changed, shaking the whole window under the cursor. A view
/// with mouseDownCanMoveWindow == false keeps the drag for the
/// gesture alone.
private struct NonDraggableStrip: NSViewRepresentable {
    func makeNSView(context _: Context) -> StripView {
        StripView()
    }

    func updateNSView(_: StripView, context _: Context) {}

    final class StripView: NSView {
        override var mouseDownCanMoveWindow: Bool {
            false
        }
    }
}
