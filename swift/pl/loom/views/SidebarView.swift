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

/// Sidebar (shell.css, tuned for native): a bg1 column holding the
/// WORKSPACES bar, per-workspace session groups (caret + name + count
/// badge; new/delete fade into a fixed trailing slot on hover; a faint
/// hierarchy guide is always on; collapse state persists), compact
/// single-line session rows (a fixed leading slot keeps every title on
/// the same axis; the trailing timestamp swaps for actions on hover),
/// and a footer with the Archive toggle and ghost buttons. The active
/// row reads at a glance: bg3 fill + primary accent bar + medium
/// title, versus hover's faint bg2 wash. ⌘-click marks rows for the
/// batch bar (archive/delete) pinned above the footer.
struct SidebarView: View {
    @Bindable var list: SessionListStore
    @Binding var selection: String?
    let onDisconnect: () -> Void
    let onOpenSettings: () -> Void

    /// Collapse state, persisted as a JSON string through AppStorage —
    /// the canonical SwiftUI path (a @State + manual UserDefaults
    /// round-trip proved unreliable across relaunches).
    @AppStorage("loom.collapsedGroups") private var collapsedGroupsJSON = "[]"
    /// Batch selection (⌘-click toggles rows; the batch bar above the
    /// footer acts on the set). Sidebar-local: `selection` stays the
    /// single session shown in the main column.
    @State private var markedSessions: Set<String> = []
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var collapsedGroups: Set<String> {
        (try? JSONDecoder().decode(Set<String>.self, from: Data(collapsedGroupsJSON.utf8))) ?? []
    }

    var body: some View {
        VStack(spacing: 0) {
            workspaceBar
                .frame(height: 28)
                .padding(.horizontal, 10)
                .padding(.top, 12)
                .padding(.bottom, 4)

            newSessionButton
                .padding(.horizontal, 10)
                .padding(.bottom, 10)

            Hairline(axis: .horizontal)

            sessionTree

            if !markedSessions.isEmpty {
                batchBar
            }

            footBar
        }
        .background(Theme.bg1)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.15), value: markedSessions.isEmpty)
        // Prune marks for sessions that vanished (deleted elsewhere,
        // archived out of the listing); switching views clears them.
        .onChange(of: list.sessions.map(\.id)) { _, ids in
            markedSessions.formIntersection(ids)
        }
        .onChange(of: list.showArchived) { _, _ in
            markedSessions.removeAll()
        }
    }

    // MARK: Delete confirmations (window-level dialog via ConfirmCenter)

    private func postConfirm(_ request: ConfirmRequest) {
        ConfirmCenter.shared.ask(request)
    }

    private func confirmDelete(_ target: SessionSummary) {
        postConfirm(ConfirmRequest(
            title: "Delete this session?",
            message: "Its history is removed from the store.",
            confirmTitle: "Delete",
        ) {
            Task {
                guard await list.deleteSession(target.id) else { return }
                if selection == target.id {
                    selection = nil
                }
                markedSessions.remove(target.id)
            }
        })
    }

    private func confirmBatchDelete() {
        postConfirm(ConfirmRequest(
            title: "Delete the selected sessions?",
            message: "Their history is removed from the store.",
            confirmTitle: "Delete \(markedSessions.count) Sessions",
        ) {
            Task { await batchDelete() }
        })
    }

    private func confirmDeleteWorkspace(_ target: Workspace) {
        postConfirm(ConfirmRequest(
            title: "Delete this workspace and all its sessions?",
            message: "The directory on disk is left untouched.",
            confirmTitle: "Delete Workspace",
        ) {
            if list.sessions(for: target.id).contains(where: { $0.id == selection }) {
                selection = nil
            }
            Task { await list.deleteWorkspace(target.id) }
        })
    }

    // MARK: Top (.ws-bar + .new-session)

    private var workspaceBar: some View {
        HStack {
            // The archived view swaps the bar's title so the listing is
            // unmistakably the read-only history, not the live tree.
            Text(list.showArchived ? "ARCHIVED" : "WORKSPACES")
                .font(.system(size: Theme.textXs, weight: .semibold))
                .foregroundStyle(list.showArchived ? Theme.primary : Theme.muted)
                .tracking(0.8)
            Spacer()
            if !list.showArchived {
                GhostButton(action: addWorkspace) {
                    Image(systemName: "folder.badge.plus")
                        .font(.system(size: 11, weight: .medium))
                }
                .help("Add workspace…")
                .accessibilityLabel("Add workspace")
            }
        }
        .padding(.horizontal, 4)
    }

    /// .new-session: full-width bg2 button, bg3 on hover.
    private var newSessionButton: some View {
        Button {
            Task {
                let workspaceId = selection.flatMap { id in
                    list.sessions.first { $0.id == id }?.workspaceId
                }
                if let id = await list.newSession(workspaceId: workspaceId) {
                    selection = id
                }
            }
        } label: {
            HStack(spacing: 6) {
                Image(systemName: "plus")
                    .font(.system(size: 12, weight: .medium))
                Text("New session")
                    .font(.system(size: Theme.textMd, weight: .medium))
                Spacer()
            }
            .foregroundStyle(Theme.fg)
            .padding(.horizontal, 12)
            .padding(.vertical, 9) // .new-session: 9px 12px, radius-md
            .background(Theme.bg2, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("New session (⌘N)")
    }

    // MARK: Session tree (.ws-group / .ws-node / .ws-sessions)

    private var sessionTree: some View {
        ScrollView {
            // spacing 0: the inter-group rhythm lives on WorkspaceGroup
            // itself (8pt bottom), so collapsed runs still read as
            // sections instead of one undifferentiated list.
            LazyVStack(spacing: 0) {
                if let error = list.loadError {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.error)
                        .padding(8)
                }
                if list.isLoading, list.workspaces.isEmpty, list.sessions.isEmpty {
                    ProgressView("Loading sessions…")
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 20)
                }
                ForEach(list.workspaces) { workspace in
                    workspaceGroup(for: workspace)
                }
                // Dangling sessions whose workspace was deleted render
                // under "Deleted workspace" groups, one per orphaned
                // workspace id (WebUI Sidebar: ordered = workspace ids
                // ++ orphan buckets; no new/delete actions there).
                ForEach(orphanGroups, id: \.id) { group in
                    orphanGroup(group)
                }
            }
            .padding(.horizontal, 10)
        }
        .frame(maxHeight: .infinity)
    }

    /// A registered workspace's group (broken out of the tree builder —
    /// the full call-site expression blew the type-checker's budget).
    private func workspaceGroup(for workspace: Workspace) -> some View {
        WorkspaceGroup(
            name: workspace.name,
            sessions: list.sessions(for: workspace.id),
            selection: $selection,
            isCollapsed: collapsedGroups.contains(workspace.id),
            onToggle: { toggleGroup(workspace.id) },
            onNewSession: {
                Task {
                    if let id = await list.newSession(workspaceId: workspace.id) {
                        selection = id
                    }
                }
            },
            // The default workspace is deletable too: the server
            // re-pins the default to the newest remaining workspace.
            onDeleteWorkspace: { confirmDeleteWorkspace(workspace) },
            archivedView: list.showArchived,
            onArchiveSession: archiveOrUnarchive,
            onDeleteSession: { confirmDelete($0) },
            marked: markedSessions,
            onToggleMark: toggleMark,
            onMarkAll: { mark in markAll(list.sessions(for: workspace.id), mark) },
            onClearMarks: { markedSessions.removeAll() },
        )
    }

    private func orphanGroup(_ group: (id: String, sessions: [SessionSummary])) -> some View {
        WorkspaceGroup(
            name: "Deleted workspace",
            sessions: group.sessions,
            selection: $selection,
            isCollapsed: collapsedGroups.contains(group.id),
            onToggle: { toggleGroup(group.id) },
            onNewSession: nil,
            onDeleteWorkspace: nil,
            archivedView: list.showArchived,
            onArchiveSession: archiveOrUnarchive,
            onDeleteSession: { confirmDelete($0) },
            marked: markedSessions,
            onToggleMark: toggleMark,
            onMarkAll: { mark in markAll(group.sessions, mark) },
            onClearMarks: { markedSessions.removeAll() },
        )
    }

    /// Sessions whose workspace id is not in the registry, bucketed by
    /// that id (the bucket order follows first appearance).
    private var orphanGroups: [(id: String, sessions: [SessionSummary])] {
        let known = Set(list.workspaces.map(\.id))
        var order: [String] = []
        var buckets: [String: [SessionSummary]] = [:]
        for session in list.sessions {
            let key = session.workspaceId ?? ""
            guard !known.contains(key), !key.isEmpty else { continue }
            if buckets[key] == nil {
                order.append(key)
            }
            buckets[key, default: []].append(session)
        }
        return order.map { (id: $0, sessions: buckets[$0] ?? []) }
    }

    private func toggleGroup(_ id: String) {
        var groups = collapsedGroups
        if groups.contains(id) {
            groups.remove(id)
        } else {
            groups.insert(id)
        }
        collapsedGroupsJSON = String(
            decoding: (try? JSONEncoder().encode(groups)) ?? Data("[]".utf8),
            as: UTF8.self,
        )
    }

    // MARK: Batch selection

    private func toggleMark(_ session: SessionSummary) {
        if markedSessions.contains(session.id) {
            markedSessions.remove(session.id)
        } else {
            markedSessions.insert(session.id)
        }
    }

    private func markAll(_ sessions: [SessionSummary], _ mark: Bool) {
        let ids = sessions.map(\.id)
        if mark {
            markedSessions.formUnion(ids)
        } else {
            markedSessions.subtract(ids)
        }
    }

    private func batchArchive() async {
        let ids = markedSessions
        markedSessions.removeAll()
        for id in ids {
            if list.showArchived {
                await list.unarchiveSession(id)
            } else {
                await list.archiveSession(id)
            }
        }
    }

    private func batchDelete() async {
        let ids = markedSessions
        for id in ids {
            guard await list.deleteSession(id) else { continue }
            markedSessions.remove(id)
            if selection == id {
                selection = nil
            }
        }
    }

    /// Row archive action: archives in the active view, unarchives in
    /// the archived (read-only history) view.
    private func archiveOrUnarchive(_ session: SessionSummary) {
        Task {
            if list.showArchived {
                await list.unarchiveSession(session.id)
            } else {
                await list.archiveSession(session.id)
            }
        }
    }

    private func addWorkspace() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Add Workspace"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let path = url.path
        let name = url.lastPathComponent
        Task { await list.addWorkspace(rootPath: path, name: name) }
    }

    // MARK: Footer (.sidebar-foot)

    /// .sidebar-foot: the Archive/Back view toggle on the left, the
    /// refresh/settings/disconnect ghost buttons on the right. (The
    /// brand mark moved out — it was the brightest element in the
    /// footer and did nothing; it still fronts the empty state.)
    private var footBar: some View {
        HStack(spacing: 8) {
            Button {
                Task { await list.toggleArchivedView() }
            } label: {
                HStack(spacing: 4) {
                    Image(systemName: list.showArchived ? "arrow.left" : "archivebox")
                    Text(list.showArchived ? "Back" : "Archive")
                }
                .font(.system(size: Theme.textXs))
                .foregroundStyle(list.showArchived ? Theme.primary : Theme.muted)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help(list.showArchived ? "Back to sessions" : "View archived sessions")

            Spacer()

            GhostButton(size: 13) {
                Task {
                    await list.loadWorkspaces()
                    await list.loadSessions()
                }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .help("Refresh")
            .accessibilityLabel("Refresh sessions")
            .disabled(list.isLoading)

            GhostButton(size: 13, action: onOpenSettings) {
                Image(systemName: "gear")
            }
            .help("Settings")
            .accessibilityLabel("Settings")

            GhostButton(size: 13, action: onDisconnect) {
                // network.slash: this severs the server connection,
                // it does not quit the app (power read as "quit").
                Image(systemName: "network.slash")
            }
            .help("Disconnect")
            .accessibilityLabel("Disconnect")
        }
        .padding(.horizontal, 10)
        .padding(.top, 8)
        .padding(.bottom, 6)
        .overlay(alignment: .top) {
            Hairline(axis: .horizontal)
        }
    }

    // MARK: Batch bar (visible while marks exist)

    /// Pinned above the footer whenever sessions are marked: count +
    /// archive/delete/clear for the batch.
    private var batchBar: some View {
        HStack(spacing: 10) {
            Text("\(markedSessions.count) selected")
                .font(.system(size: Theme.textXs, weight: .medium))
                .foregroundStyle(Theme.fg)

            Spacer()

            Button(list.showArchived ? "Unarchive" : "Archive") {
                Task { await batchArchive() }
            }
            .buttonStyle(BatchActionStyle())

            Button("Delete") { confirmBatchDelete() }
                .buttonStyle(BatchActionStyle(danger: true))

            GhostButton(size: 11) { markedSessions.removeAll() } label: {
                Image(systemName: "xmark")
            }
            .help("Clear selection")
            .accessibilityLabel("Clear selection")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Theme.bg2)
        .overlay(alignment: .top) {
            Hairline(axis: .horizontal)
        }
        .transition(.move(edge: .bottom).combined(with: .opacity))
    }
}

/// The batch bar's text actions: xs medium, primary (danger = error),
/// dimmed on press.
private struct BatchActionStyle: ButtonStyle {
    var danger = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Theme.textXs, weight: .medium))
            .foregroundStyle(danger ? Theme.error : Theme.primary)
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .contentShape(Rectangle())
            .opacity(configuration.isPressed ? 0.6 : 1)
    }
}

// MARK: - Workspace group (.ws-group)

private struct WorkspaceGroup: View {
    let name: String
    let sessions: [SessionSummary]
    @Binding var selection: String?
    let isCollapsed: Bool
    let onToggle: () -> Void
    let onNewSession: (() -> Void)?
    let onDeleteWorkspace: (() -> Void)?
    /// Read-only history listing: group actions hide, row archive
    /// becomes unarchive (WebUI archivedView).
    var archivedView = false
    let onArchiveSession: (SessionSummary) -> Void
    let onDeleteSession: (SessionSummary) -> Void
    /// Batch selection: the marked set, ⌘-click toggler, group-level
    /// mark-all, and the plain-click mark reset.
    let marked: Set<String>
    let onToggleMark: (SessionSummary) -> Void
    let onMarkAll: (Bool) -> Void
    let onClearMarks: () -> Void

    @State private var hovered = false
    @FocusState private var headerFocused: Bool
    @FocusState private var focusedAction: GroupAction?
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private enum GroupAction: Hashable { case newSession, deleteWorkspace }

    private var hasActive: Bool {
        sessions.contains { $0.id == selection }
    }

    /// In-group hierarchy (WebUI orderedItems): sub-agent sessions
    /// render indented immediately under their parent; a session whose
    /// parent is outside the group falls back to top level.
    private var orderedItems: [(session: SessionSummary, isChild: Bool)] {
        let allIds = Set(sessions.map(\.id))
        var childrenOf: [String: [SessionSummary]] = [:]
        var tops: [SessionSummary] = []
        for session in sessions {
            if let parent = session.parentSessionId, allIds.contains(parent) {
                childrenOf[parent, default: []].append(session)
            } else {
                tops.append(session)
            }
        }
        var items: [(session: SessionSummary, isChild: Bool)] = []
        for top in tops {
            items.append((session: top, isChild: false))
            for child in childrenOf[top.id] ?? [] {
                items.append((session: child, isChild: true))
            }
        }
        return items
    }

    var body: some View {
        VStack(spacing: 1) {
            groupHeader

            if !isCollapsed {
                // .ws-sessions: indented; an always-on faint guide
                // (bg2, brightening to bg3 on group hover) keeps the
                // hierarchy readable at rest.
                HStack(spacing: 0) {
                    // 11.5pt: centers the guide on the group header's
                    // chevron column (chevron center = 10 tree padding
                    // + 6 header padding + 6 half-slot).
                    Rectangle()
                        .fill(hovered ? Theme.bg3 : Theme.bg2)
                        .frame(width: 1)
                        .padding(.leading, 11.5)
                    VStack(spacing: 1) {
                        if sessions.isEmpty {
                            Text("No sessions")
                                .font(.system(size: Theme.textXs))
                                .foregroundStyle(Theme.muted)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.horizontal, 10)
                                .padding(.vertical, 4)
                        } else {
                            ForEach(orderedItems, id: \.session.id) { item in
                                SessionRow(
                                    session: item.session,
                                    isActive: selection == item.session.id,
                                    isMarked: marked.contains(item.session.id),
                                    isChild: item.isChild,
                                    archivedView: archivedView,
                                    onArchive: { onArchiveSession(item.session) },
                                    onDelete: { onDeleteSession(item.session) },
                                    onToggleMark: { onToggleMark(item.session) },
                                    onSelect: {
                                        selection = item.session.id
                                        onClearMarks()
                                    },
                                )
                            }
                        }
                    }
                    .padding(.leading, 8)
                }
            }
        }
        // The 8pt group rhythm: sections read as sections even when a
        // run of groups is collapsed.
        .padding(.bottom, 8)
        .onHover { hovered = $0 }
    }

    /// .ws-node: caret + name + count; hover reveals new/delete.
    private var groupHeader: some View {
        HStack(spacing: 0) {
            Button(action: onToggle) {
                headerLabel
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .focused($headerFocused)
            .accessibilityLabel("\(name), \(sessions.count) sessions")
            .accessibilityValue(isCollapsed ? "Collapsed" : "Expanded")
            .accessibilityHint("Toggle workspace group")
            .accessibilityAction(named: "Select all sessions") { onMarkAll(true) }
            .accessibilityAction(named: "Deselect all sessions") { onMarkAll(false) }
            .contextMenu {
                if !archivedView, !sessions.isEmpty {
                    Button("Select All Sessions") { onMarkAll(true) }
                    Button("Deselect All") { onMarkAll(false) }
                }
            }

            if !archivedView {
                HStack(spacing: 2) {
                    if let onNewSession {
                        Button(action: onNewSession) {
                            Image(systemName: "plus")
                                .font(.system(size: 11, weight: .medium))
                        }
                        .buttonStyle(GroupActionButtonStyle())
                        .help("New session in \(name)")
                        .accessibilityLabel("New session in \(name)")
                        .focused($focusedAction, equals: .newSession)
                    }
                    if let onDeleteWorkspace {
                        Button(action: onDeleteWorkspace) {
                            Image(systemName: "xmark")
                                .font(.system(size: 10, weight: .medium))
                        }
                        .buttonStyle(GroupActionButtonStyle(danger: true))
                        .help("Delete workspace \(name)")
                        .accessibilityLabel("Delete workspace \(name)")
                        .focused($focusedAction, equals: .deleteWorkspace)
                    }
                }
                .opacity(showActions ? 1 : 0)
                .accessibilityHidden(!showActions)
                .animation(reduceMotion ? nil : .easeInOut(duration: 0.12), value: showActions)
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 5)
        .background(
            hovered ? Theme.bg2 : Color.clear,
            in: RoundedRectangle(cornerRadius: Theme.radiusMd),
        )
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.16), value: isCollapsed)
    }

    private var showActions: Bool {
        hovered || headerFocused || focusedAction != nil
    }

    private var headerLabel: some View {
        HStack(spacing: 6) {
            Image(systemName: "chevron.right")
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(Theme.muted)
                .rotationEffect(.degrees(isCollapsed ? 0 : 90))
                .frame(width: 12)

            Text(name)
                .font(.system(size: Theme.textMd, weight: .semibold))
                .foregroundStyle(hasActive ? Theme.primary : Theme.fg)
                .lineLimit(1)
                .truncationMode(.tail)
                .layoutPriority(1)

            // Count as a capsule (macOS sidebar idiom); it steps up a
            // shade while the header itself is hover-highlighted.
            Text("\(sessions.count)")
                .font(.system(size: Theme.textXs).monospacedDigit())
                .foregroundStyle(Theme.muted)
                .padding(.horizontal, 6)
                .padding(.vertical, 1)
                .background(hovered ? Theme.bg3 : Theme.bg2, in: Capsule())

            Spacer(minLength: 2)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// The tiny hover actions on .ws-node rows.
private struct GroupActionButtonStyle: ButtonStyle {
    var danger = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundStyle(
                danger
                    ? (configuration.isPressed ? Theme.error : Theme.error.opacity(0.8))
                    : (configuration.isPressed ? Theme.fg : Theme.muted),
            )
            .padding(.horizontal, 3)
            .padding(.vertical, 1)
            .background(
                configuration.isPressed ? Theme.bg1 : Color.clear,
                in: RoundedRectangle(cornerRadius: 4),
            )
            .contentShape(Rectangle())
    }
}

// MARK: - Session row (.sess-item)

private struct SessionRow: View {
    let session: SessionSummary
    let isActive: Bool
    var isMarked = false
    var isChild: Bool = false
    var archivedView = false
    let onArchive: () -> Void
    let onDelete: () -> Void
    let onToggleMark: () -> Void
    let onSelect: () -> Void

    @State private var hovered = false
    @FocusState private var rowFocused: Bool
    @FocusState private var focusedAction: RowAction?

    private enum RowAction: Hashable { case archive, delete }

    private var showActions: Bool {
        hovered || rowFocused || focusedAction != nil
    }

    /// Live status dot: awaiting_approval gets an amber breathing light
    /// (needs attention the most), running/cancelling get green; other
    /// states show nothing, keeping the list quiet (WebUI SessionItem).
    private var statusState: String? {
        switch session.state {
        case "awaiting_approval": "attn"
        case "running", "cancelling": "run"
        default: nil
        }
    }

    var body: some View {
        Button(action: onSelect) {
            rowLabel
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        // A command-modified left click wins over the Button's normal
        // activation. Handle it as a gesture instead of inspecting
        // NSApp.currentEvent in the action: Button actions may run after
        // the mouse event has already left the event queue. Keyboard and
        // VoiceOver activation still go through onSelect.
        .highPriorityGesture(
            TapGesture().modifiers(.command).onEnded { _ in onToggleMark() },
            including: .gesture,
        )
        .focused($rowFocused)
        .accessibilityLabel(title)
        .accessibilityValue(isMarked ? "Marked" : (isActive ? "Selected" : ""))
        .accessibilityHint("Open session; use the actions menu to select, archive or delete")
        .accessibilityAction(named: isMarked ? "Deselect" : "Select") { onToggleMark() }
        .accessibilityAction(named: archivedView ? "Unarchive" : "Archive") { onArchive() }
        .accessibilityAction(named: "Delete session") { onDelete() }
        .contextMenu {
            Button(isMarked ? "Deselect" : "Select", action: onToggleMark)
            Divider()
            Button(archivedView ? "Unarchive" : "Archive", action: onArchive)
            Divider()
            Button("Delete Session", role: .destructive, action: onDelete)
        }
        .overlay(alignment: .trailing) {
            if showActions {
                HStack(spacing: 2) {
                    Button(action: onArchive) {
                        Image(systemName: archivedView ? "arrow.uturn.left" : "archivebox")
                            .font(.system(size: 11))
                    }
                    .buttonStyle(RowActionButtonStyle())
                    .help(archivedView ? "Unarchive" : "Archive")
                    .accessibilityLabel(archivedView ? "Unarchive \(title)" : "Archive \(title)")
                    .focused($focusedAction, equals: .archive)

                    Button(action: onDelete) {
                        Image(systemName: "trash")
                            .font(.system(size: 11))
                    }
                    .buttonStyle(RowActionButtonStyle(danger: true))
                    .help("Delete session")
                    .accessibilityLabel("Delete \(title)")
                    .focused($focusedAction, equals: .delete)
                }
                .padding(.trailing, 10)
            }
        }
        .help(tooltip)
        .onHover { hovered = $0 }
    }

    private var rowLabel: some View {
        HStack(spacing: 6) {
            // Fixed-width leading slot: the live-status pulse, else the
            // subagent glyph, else nothing — reserving the width keeps
            // every row's title on the same vertical axis. (The
            // archived glyph was dropped: in the archived view every
            // row is archived by definition — pure noise.)
            Group {
                if isMarked {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.primary)
                } else if let statusState {
                    PulsingDot(
                        color: statusState == "attn" ? Theme.warning : Theme.success,
                        size: 7,
                    )
                } else if isChild {
                    // Branch glyph for a subagent-derived session (cpu
                    // read as "processor", not "child").
                    Image(systemName: "arrow.turn.down.right")
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.muted)
                }
            }
            .frame(width: 10)

            // Middle truncation: sibling sessions often share a long
            // prompt prefix and only differ at the tail.
            Text(title)
                .font(.system(size: Theme.textMd, weight: isActive ? .medium : .regular))
                .foregroundStyle(Theme.fg)
                .lineLimit(1)
                .truncationMode(.middle)
                .layoutPriority(1)

            Spacer(minLength: 4)

            // Fixed-width trailing slot: the compact timestamp; hover
            // swaps in archive + delete. Reserving the width keeps the
            // title from re-truncating on hover.
            // The list is sorted by updatedAt — show the same
            // timestamp the ordering is based on.
            Text(relativeTime(session.updatedAt ?? session.createdAt))
                .font(.system(size: Theme.textXs).monospacedDigit())
                .foregroundStyle(Theme.muted)
                .lineLimit(1)
                .opacity(showActions ? 0 : 1)
                .accessibilityHidden(true)
                .frame(width: 44, alignment: .trailing)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        // Active = bg3: the bg1→bg2 step is only ~3% luminance and
        // the selection wash was nearly invisible on its own. Marked
        // (batch-selected) rows get the steady bg2.
        .background(
            isMarked ? Theme.bg2 : (isActive ? Theme.bg3 : (hovered ? Theme.bg2.opacity(0.55) : Color.clear)),
            in: UnevenRoundedRectangle(
                topLeadingRadius: isChild ? 0 : Theme.radiusSm,
                bottomLeadingRadius: isChild ? 0 : Theme.radiusSm,
                bottomTrailingRadius: Theme.radiusSm,
                topTrailingRadius: Theme.radiusSm,
            ),
        )
        // .sess-item.is-child: 12px indent + 1px left hierarchy guide.
        .overlay(alignment: .leading) {
            if isChild {
                Rectangle()
                    .fill(Theme.bg2)
                    .frame(width: 1)
            }
        }
        .padding(.leading, isChild ? 12 : 0)
    }

    /// shortId: the first 8 chars, like the WebUI's title fallback.
    private var shortId: String {
        String(session.id.prefix(8))
    }

    private var title: String {
        session.title?.isEmpty == false ? session.title! : shortId
    }

    private var tooltip: String {
        var extras: [String] = []
        if isChild {
            extras.append("subagent")
        }
        if let model = session.modelName, !model.isEmpty {
            extras.append(model)
        }
        return extras.isEmpty ? title : "\(title) · \(extras.joined(separator: " · "))"
    }
}

/// The bare hover-action buttons on .sess-item rows (muted glyph, bg1
/// wash on press, red for the destructive one).
private struct RowActionButtonStyle: ButtonStyle {
    var danger = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .foregroundStyle(
                danger
                    ? (configuration.isPressed ? Theme.error : Theme.muted)
                    : (configuration.isPressed ? Theme.fg : Theme.muted),
            )
            .padding(.horizontal, 4)
            .padding(.vertical, 2)
            .background(
                configuration.isPressed ? Theme.bg1 : Color.clear,
                in: RoundedRectangle(cornerRadius: 4),
            )
            .contentShape(Rectangle())
    }
}
