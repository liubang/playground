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

/// WebUI-faithful sidebar (shell.css): a bg1 column holding the
/// WORKSPACES bar, per-workspace session groups (caret + name + count,
/// hover reveals new/delete; the hierarchy guide appears on group
/// hover), single-line session rows (status dot · title · relative
/// time, hover swaps the time for actions), and a footer with the
/// brand mark and ghost buttons.
struct SidebarView: View {
    @Bindable var list: SessionListStore
    @Binding var selection: String?
    let onDisconnect: () -> Void
    let onOpenSettings: () -> Void

    @State private var pendingDelete: SessionSummary?
    @State private var pendingDeleteWorkspace: Workspace?
    @State private var collapsedGroups: Set<String> = []

    var body: some View {
        VStack(spacing: 0) {
            VStack(spacing: 8) {
                workspaceBar
                newSessionButton
            }
            .padding(.horizontal, 10)
            .padding(.top, 12)
            .padding(.bottom, 12)

            sessionTree

            footBar
        }
        .background(Theme.bg1)
        .confirmationDialog(
            "Delete this session? Its history is removed from the store.",
            isPresented: .constant(pendingDelete != nil),
            titleVisibility: .visible,
        ) {
            Button("Delete", role: .destructive) {
                if let target = pendingDelete {
                    Task { await list.deleteSession(target.id) }
                    if selection == target.id {
                        selection = nil
                    }
                }
                pendingDelete = nil
            }
            Button("Cancel", role: .cancel) { pendingDelete = nil }
        }
        .confirmationDialog(
            "Delete this workspace and all its sessions? The directory on disk is left untouched.",
            isPresented: .constant(pendingDeleteWorkspace != nil),
            titleVisibility: .visible,
        ) {
            Button("Delete Workspace", role: .destructive) {
                if let target = pendingDeleteWorkspace {
                    if list.sessions(for: target.id).contains(where: { $0.id == selection }) {
                        selection = nil
                    }
                    Task { await list.deleteWorkspace(target.id) }
                }
                pendingDeleteWorkspace = nil
            }
            Button("Cancel", role: .cancel) { pendingDeleteWorkspace = nil }
        }
    }

    // MARK: Top (.ws-bar + .new-session)

    private var workspaceBar: some View {
        HStack {
            // The archived view swaps the bar's title so the listing is
            // unmistakably the read-only history, not the live tree.
            Text(list.showArchived ? "ARCHIVED" : "WORKSPACES")
                .font(.system(size: Theme.textXs, weight: .semibold))
                .foregroundStyle(list.showArchived ? Theme.primary : Theme.muted)
                .tracking(0.5)
            Spacer()
            if !list.showArchived {
                Button(action: addWorkspace) {
                    Image(systemName: "folder.badge.plus")
                        .font(.system(size: 11, weight: .medium))
                }
                .buttonStyle(GhostButtonStyle())
                .help("Add workspace…")
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
            .padding(.vertical, 9)
            .background(Theme.bg2, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("New session (⌘N)")
    }

    // MARK: Session tree (.ws-group / .ws-node / .ws-sessions)

    private var sessionTree: some View {
        ScrollView {
            LazyVStack(spacing: 2) {
                if let error = list.loadError {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.error)
                        .padding(8)
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
            onDeleteWorkspace: workspace.isDefault == true
                ? nil
                : { pendingDeleteWorkspace = workspace },
            archivedView: list.showArchived,
            onArchiveSession: archiveOrUnarchive,
            onDeleteSession: { pendingDelete = $0 },
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
            onDeleteSession: { pendingDelete = $0 },
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
        if collapsedGroups.contains(id) {
            collapsedGroups.remove(id)
        } else {
            collapsedGroups.insert(id)
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
    /// brand mark on the right (WebUI sidebar-foot).
    private var footBar: some View {
        HStack(spacing: 8) {
            Button {
                Task { await list.toggleArchivedView() }
            } label: {
                HStack(spacing: 4) {
                    if list.showArchived {
                        Image(systemName: "arrow.left")
                        Text("Back")
                    } else {
                        Text("Archive")
                    }
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

            Button {
                Task {
                    await list.loadWorkspaces()
                    await list.loadSessions()
                }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(GhostButtonStyle())
            .help("Refresh")
            .disabled(list.isLoading)

            Button(action: onOpenSettings) {
                Image(systemName: "gear")
            }
            .buttonStyle(GhostButtonStyle())
            .help("Settings")

            Button(role: .cancel, action: onDisconnect) {
                Image(systemName: "bolt.horizontal.circle")
            }
            .buttonStyle(GhostButtonStyle())
            .help("Disconnect")

            Text("◆ loom")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(Theme.primary)
        }
        .padding(.horizontal, 10)
        .padding(.top, 8)
        .padding(.bottom, 6)
        .overlay(alignment: .top) {
            Hairline(axis: .horizontal)
        }
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

    @State private var hovered = false

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
                // .ws-sessions: indented; the hierarchy guide appears on
                // group hover (VSCode tree indent-guide interaction).
                HStack(spacing: 0) {
                    Rectangle()
                        .fill(hovered ? Theme.bg2 : Color.clear)
                        .frame(width: 1)
                        .padding(.leading, 9)
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
                                    isChild: item.isChild,
                                    archivedView: archivedView,
                                    onArchive: { onArchiveSession(item.session) },
                                    onDelete: { onDeleteSession(item.session) },
                                )
                                .contentShape(Rectangle())
                                .onTapGesture { selection = item.session.id }
                            }
                        }
                    }
                    .padding(.leading, 8)
                }
                .padding(.bottom, 4)
            }
        }
        .onHover { hovered = $0 }
    }

    /// .ws-node: caret + name + count; hover reveals new/delete.
    private var groupHeader: some View {
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

            Text("\(sessions.count)")
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.muted)

            Spacer(minLength: 2)

            // New/delete entries only show in the active view (the
            // archived view is read-only history).
            if hovered, !archivedView {
                if let onNewSession {
                    Button(action: onNewSession) {
                        Image(systemName: "doc.badge.plus")
                            .font(.system(size: 11, weight: .medium))
                    }
                    .buttonStyle(GroupActionButtonStyle())
                    .help("New session in \(name)")
                }

                if let onDeleteWorkspace {
                    Button(action: onDeleteWorkspace) {
                        Image(systemName: "xmark")
                            .font(.system(size: 10, weight: .medium))
                    }
                    .buttonStyle(GroupActionButtonStyle(danger: true))
                    .help("Delete workspace")
                }
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 5)
        .background(
            hovered ? Theme.bg2 : Color.clear,
            in: RoundedRectangle(cornerRadius: Theme.radiusMd),
        )
        .contentShape(Rectangle())
        .onTapGesture(perform: onToggle)
        .animation(.easeInOut(duration: 0.16), value: isCollapsed)
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
    var isChild: Bool = false
    var archivedView = false
    let onArchive: () -> Void
    let onDelete: () -> Void

    @State private var hovered = false

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
        HStack(spacing: 6) {
            if isChild {
                Image(systemName: "cpu")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .help("Subagent session")
            }

            if archivedView {
                Image(systemName: "archivebox")
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.muted)
            }

            if let statusState {
                PulsingDot(
                    color: statusState == "attn" ? Theme.warning : Theme.success,
                    size: 7,
                )
            }

            Text(session.title?.isEmpty == false ? session.title! : shortId)
                .font(.system(size: Theme.textMd))
                .foregroundStyle(Theme.fg)
                .lineLimit(1)
                .truncationMode(.tail)
                .layoutPriority(1)

            Spacer(minLength: 4)

            // Hover swaps the timestamp for archive + delete (WebUI).
            if hovered {
                HStack(spacing: 2) {
                    Button(action: onArchive) {
                        Image(systemName: archivedView ? "arrow.uturn.left" : "archivebox")
                            .font(.system(size: 11))
                    }
                    .buttonStyle(RowActionButtonStyle())
                    .help(archivedView ? "Unarchive" : "Archive")

                    Button(action: onDelete) {
                        Image(systemName: "trash")
                            .font(.system(size: 11))
                    }
                    .buttonStyle(RowActionButtonStyle(danger: true))
                    .help("Delete session")
                }
            } else {
                Text(relativeTime(session.createdAt))
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .lineLimit(1)
                    .fixedSize()
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(
            (isActive || hovered) ? Theme.bg2 : Color.clear,
            in: UnevenRoundedRectangle(
                topLeadingRadius: isChild ? 0 : Theme.radiusMd,
                bottomLeadingRadius: isChild ? 0 : Theme.radiusMd,
                bottomTrailingRadius: Theme.radiusMd,
                topTrailingRadius: Theme.radiusMd,
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
        .help(tooltip)
        .onHover { hovered = $0 }
    }

    /// shortId: the first 8 chars, like the WebUI's title fallback.
    private var shortId: String {
        String(session.id.prefix(8))
    }

    private var tooltip: String {
        let title = session.title?.isEmpty == false ? session.title! : shortId
        if let model = session.modelName, !model.isEmpty {
            return "\(title) · \(model)"
        }
        return title
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
