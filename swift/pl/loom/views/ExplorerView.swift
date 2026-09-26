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

// The workspace explorer (right panel): git working-tree changes with
// inline diffs, and a lazy file tree with search + preview. Read-only
// by design — the review surface for what the agent (or the user)
// changed, never a file manager.
//
// Layout idiom: an inspector column. The toolbar-height tab strip sits
// in the window's toolbar row (RootView), the body below switches
// between the tabbed list and a pushed drill-down detail (diff /
// preview) — a narrow panel reads better with one level of navigation
// than with accordion-stacked inline expansions.

// MARK: - Toolbar strip (window toolbar row)

/// Changes/Files tabs + the change-count badge + the close button,
/// framed exactly at toolbar height so RootView can append it to the
/// split toolbar when the panel is open.
struct ExplorerTabStrip: View {
    @Bindable var store: WorkspaceExplorerStore
    let onClose: () -> Void

    var body: some View {
        HStack(spacing: 2) {
            ForEach(WorkspaceExplorerStore.Tab.allCases) { tab in
                Button {
                    store.back()
                    store.tab = tab
                } label: {
                    HStack(spacing: 4) {
                        Text(tab.rawValue)
                        if tab == .changes, store.changeCount > 0 {
                            Text("\(store.changeCount)")
                                .font(.system(size: 9, weight: .bold, design: .rounded))
                                .foregroundStyle(Theme.onAccent)
                                .padding(.horizontal, 4)
                                .padding(.vertical, 1)
                                .background(Theme.warning, in: Capsule())
                        }
                    }
                    .font(.system(
                        size: Theme.textXs,
                        weight: store.tab == tab ? .semibold : .regular,
                    ))
                    .foregroundStyle(store.tab == tab ? Theme.fg : Theme.muted)
                    .padding(.horizontal, 8)
                    .frame(height: Theme.toolbarHeight)
                    .contentShape(Rectangle())
                    .overlay(alignment: .bottom) {
                        if store.tab == tab {
                            Rectangle().fill(Theme.primary).frame(height: 2)
                        }
                    }
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(store.tab == tab ? .isSelected : [])
            }
            Spacer(minLength: 0)
            GhostButton(size: 11, action: onClose) {
                Image(systemName: "xmark")
            }
            .help("Close workspace panel (⌥⌘0)")
            .accessibilityLabel("Close workspace panel")
            .padding(.trailing, 6)
        }
        .frame(height: Theme.toolbarHeight)
        .background(Theme.bg1)
        .windowDragSurface()
    }
}

// MARK: - Panel body

struct WorkspaceExplorerView: View {
    @Bindable var store: WorkspaceExplorerStore

    var body: some View {
        Group {
            switch store.route {
            case .list:
                listContent
            case let .diff(path):
                ExplorerDiffDetail(store: store, path: path)
            case let .preview(path):
                ExplorerPreviewDetail(store: store, path: path)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg0)
    }

    @ViewBuilder
    private var listContent: some View {
        switch store.tab {
        case .changes:
            ChangesListView(store: store)
        case .files:
            FilesListView(store: store)
        }
    }
}

// MARK: - Changes tab

private struct ChangesListView: View {
    let store: WorkspaceExplorerStore

    var body: some View {
        Group {
            if store.workspaceId == nil {
                ExplorerPlaceholder(
                    systemImage: "folder", title: "No workspace",
                    detail: "Select a session to inspect its workspace.",
                )
            } else if let git = store.git, !git.isGit {
                ExplorerPlaceholder(
                    systemImage: "arrow.triangle.branch", title: "Not a git repository",
                    detail: "Changes tracks the git working tree; this workspace has none.",
                )
            } else if let git = store.git {
                if git.files?.isEmpty ?? true {
                    ExplorerPlaceholder(
                        systemImage: "checkmark.circle", title: "Working tree clean",
                        detail: git.branch.map { "on \($0)" },
                    )
                } else {
                    changesList(git)
                }
            } else if let error = store.gitError {
                ExplorerPlaceholder(
                    systemImage: "exclamationmark.triangle", title: "Could not load changes",
                    detail: error,
                )
            } else {
                ProgressView()
                    .controlSize(.small)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func changesList(_ git: WorkspaceGitStatus) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: "arrow.triangle.branch")
                    .font(.system(size: 10))
                    .foregroundStyle(Theme.purple)
                Text(git.branch ?? "HEAD")
                    .font(Theme.monoXs)
                    .foregroundStyle(Theme.fg)
                    .lineLimit(1)
                Spacer(minLength: 6)
                Text("+\(git.adds ?? 0)")
                    .foregroundStyle(Theme.success)
                Text("−\(git.dels ?? 0)")
                    .foregroundStyle(Theme.error)
                GhostButton(size: 10) {
                    Task { await store.refreshGit() }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .frame(minWidth: 22, minHeight: 20)
                .help("Refresh")
                .accessibilityLabel("Refresh changes")
            }
            .font(Theme.monoXs)
            .padding(.leading, 12)
            .padding(.trailing, 6)
            .frame(height: 30)
            Hairline(axis: .horizontal)
            ScrollView {
                LazyVStack(spacing: 1) {
                    ForEach(git.files ?? []) { file in
                        ChangeRow(file: file) { store.openDiff(file.path) }
                    }
                }
                .padding(.horizontal, 6)
                .padding(.vertical, 6)
            }
        }
    }
}

/// One changed file: status-letter badge, name over directory, merged
/// +/− counts. Tapping pushes the diff detail.
private struct ChangeRow: View {
    let file: WorkspaceGitFile
    let action: () -> Void
    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Text(file.status)
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .foregroundStyle(statusColor)
                    .frame(width: 16, height: 16)
                    .background(statusColor.opacity(0.15), in: RoundedRectangle(cornerRadius: 4))
                VStack(alignment: .leading, spacing: 1) {
                    Text(fileName)
                        .font(.system(size: Theme.textSm, weight: .medium))
                        .foregroundStyle(Theme.fg)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if !dirName.isEmpty {
                        Text(dirName)
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(Theme.muted)
                            .lineLimit(1)
                            .truncationMode(.head)
                    }
                }
                Spacer(minLength: 6)
                if !(file.noStat ?? false) {
                    HStack(spacing: 4) {
                        Text("+\(file.adds ?? 0)").foregroundStyle(Theme.success)
                        Text("−\(file.dels ?? 0)").foregroundStyle(Theme.error)
                    }
                    .font(Theme.monoXs)
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .contentShape(Rectangle())
            .background(
                hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .accessibilityLabel("\(file.path), \(file.status)")
    }

    private var fileName: String {
        (file.path as NSString).lastPathComponent
    }

    private var dirName: String {
        (file.path as NSString).deletingLastPathComponent
    }

    private var statusColor: Color {
        explorerGitColor(file.status)
    }
}

// MARK: - Files tab

private struct FilesListView: View {
    @Bindable var store: WorkspaceExplorerStore

    var body: some View {
        VStack(spacing: 0) {
            ExplorerSearchField(text: $store.searchQuery)
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
            Hairline(axis: .horizontal)
            if searching {
                searchResultsList
            } else {
                treeList
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var searching: Bool {
        !store.searchQuery.trimmingCharacters(in: .whitespaces).isEmpty
    }

    // MARK: Tree

    @ViewBuilder
    private var treeList: some View {
        if store.workspaceId == nil {
            ExplorerPlaceholder(
                systemImage: "folder", title: "No workspace",
                detail: "Select a session to browse its workspace.",
            )
        } else if let root = store.entries(for: "") {
            if root.entries.isEmpty {
                ExplorerPlaceholder(systemImage: "folder", title: "Empty workspace")
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(store.visibleNodes) { node in
                            TreeRow(store: store, node: node)
                        }
                    }
                    .padding(.horizontal, 6)
                    .padding(.vertical, 4)
                }
            }
        } else {
            ProgressView()
                .controlSize(.small)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    // MARK: Search results

    @ViewBuilder
    private var searchResultsList: some View {
        if store.searchResults.isEmpty, !store.searching {
            ExplorerPlaceholder(
                systemImage: "magnifyingglass", title: "No matches",
                detail: store.searchQuery,
            )
        } else {
            ScrollView {
                LazyVStack(spacing: 1) {
                    ForEach(store.searchResults) { match in
                        SearchResultRow(store: store, match: match)
                    }
                    if store.searchTruncated {
                        Text("Showing the first 50 matches")
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(Theme.muted)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .padding(.vertical, 8)
                    }
                }
                .padding(.horizontal, 6)
                .padding(.vertical, 4)
            }
        }
    }
}

/// One tree row: disclosure chevron + type icon + git-coloured name.
/// Directories toggle (lazy-loading on first expand); files push the
/// preview detail.
private struct TreeRow: View {
    let store: WorkspaceExplorerStore
    let node: WorkspaceExplorerStore.TreeNode
    @State private var hovered = false

    var body: some View {
        Button {
            if node.entry.kind == .dir {
                store.toggleDir(node.entry.path)
            } else {
                store.openPreview(node.entry.path)
            }
        } label: {
            HStack(spacing: 5) {
                if node.entry.kind == .dir {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundStyle(Theme.muted)
                        .rotationEffect(.degrees(store.expanded.contains(node.entry.path) ? 90 : 0))
                        .frame(width: 10)
                } else {
                    Color.clear.frame(width: 10, height: 1)
                }
                Image(systemName: explorerFileIcon(name: node.entry.name, kind: node.entry.kind))
                    .font(.system(size: 11))
                    .foregroundStyle(node.entry.kind == .dir ? Theme.primary : Theme.muted)
                    .frame(width: 15)
                Text(node.entry.name)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(nameColor)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer(minLength: 4)
                if node.entry.kind == .dir, store.isLoading(node.entry.path) {
                    ProgressView()
                        .controlSize(.mini)
                        .frame(width: 10, height: 10)
                }
            }
            .padding(.leading, 4 + CGFloat(node.depth) * 13)
            .padding(.trailing, 6)
            .frame(height: 25)
            .contentShape(Rectangle())
            .background(
                hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }

    /// Git working-tree colouring (VS Code idiom): modified warns,
    /// added/untracked succeeds, deleted errors — quiet otherwise.
    private var nameColor: Color {
        guard let letter = store.statusByPath[node.entry.path] else {
            return Theme.fg
        }
        return explorerGitColor(letter)
    }
}

/// One fuzzy-search hit. Files open the preview; directories reveal
/// themselves in the tree (ancestors expanded) and leave search.
private struct SearchResultRow: View {
    @Bindable var store: WorkspaceExplorerStore
    let match: WorkspaceFileMatch
    @State private var hovered = false

    var body: some View {
        Button {
            if match.kind == .dir {
                store.revealInTree(match)
            } else {
                store.openPreview(match.path)
            }
        } label: {
            HStack(spacing: 6) {
                Image(systemName: explorerFileIcon(name: match.name, kind: match.kind))
                    .font(.system(size: 11))
                    .foregroundStyle(match.kind == .dir ? Theme.primary : Theme.muted)
                    .frame(width: 15)
                VStack(alignment: .leading, spacing: 1) {
                    Text(match.name)
                        .font(.system(size: Theme.textSm, weight: .medium))
                        .foregroundStyle(Theme.fg)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Text(match.path)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                        .lineLimit(1)
                        .truncationMode(.head)
                }
                Spacer(minLength: 4)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .contentShape(Rectangle())
            .background(
                hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }
}

/// The files-tab search box: MiniSearchField styling, but fluid width
/// (the panel is user-resizable, so a fixed-width field would drift).
private struct ExplorerSearchField: View {
    @Binding var text: String
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 5) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 10))
                .foregroundStyle(Theme.muted)
            TextField("Search files", text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.fg)
                .focused($focused)
            if !text.isEmpty {
                Button { text = "" } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.muted)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Clear search")
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusSm)
                .strokeBorder(focused ? Theme.primary : Theme.bg2, lineWidth: 1),
        )
    }
}

// MARK: - Diff detail (drill-down)

private struct ExplorerDiffDetail: View {
    let store: WorkspaceExplorerStore
    let path: String

    var body: some View {
        VStack(spacing: 0) {
            ExplorerDetailHeader(
                title: (path as NSString).lastPathComponent,
                subtitle: path,
                onBack: { store.back() },
            ) {
                if let diff = store.diff {
                    if diff.untracked == true {
                        ExplorerPill(text: "untracked", color: Theme.success)
                    }
                    if diff.truncated == true {
                        ExplorerPill(text: "truncated", color: Theme.warning)
                    }
                }
            }
            Hairline(axis: .horizontal)
            content
        }
    }

    @ViewBuilder
    private var content: some View {
        if store.diffLoading {
            ProgressView()
                .controlSize(.small)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if let error = store.diffError {
            ExplorerPlaceholder(
                systemImage: "exclamationmark.triangle", title: "Could not load diff",
                detail: error,
            )
        } else if let diff = store.diff {
            if diff.isDir == true {
                ExplorerPlaceholder(
                    systemImage: "folder", title: "Untracked directory",
                    detail: "Expand it in the Files tab to see what's inside.",
                )
            } else if (diff.diff ?? "").isEmpty {
                ExplorerPlaceholder(systemImage: "doc.text", title: "No textual diff")
            } else {
                diffBody(DiffParseCache.parse(diff.diff ?? ""))
            }
        }
    }

    private func diffBody(_ parsed: ParsedDiff) -> some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Text("\(parsed.lines.count) lines")
                    .foregroundStyle(Theme.muted)
                Spacer(minLength: 6)
                Text("+\(parsed.adds)").foregroundStyle(Theme.success)
                Text("−\(parsed.dels)").foregroundStyle(Theme.error)
            }
            .font(Theme.monoXs)
            .padding(.horizontal, 12)
            .frame(height: 26)
            Hairline(axis: .horizontal)
            ScrollView(.horizontal, showsIndicators: false) {
                ScrollView(.vertical) {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(parsed.lines.indices, id: \.self) { index in
                            DiffLineView(line: parsed.lines[index])
                        }
                    }
                    .padding(.vertical, 6)
                }
            }
        }
    }
}

// MARK: - File preview detail (drill-down)

private struct ExplorerPreviewDetail: View {
    let store: WorkspaceExplorerStore
    let path: String
    /// Markdown previews render by default (reading docs is the
    /// common intent); the header toggle falls back to highlighted
    /// source. App-level preference, remembered across files.
    @AppStorage("loom.explorer.renderMarkdown") private var renderMarkdown = true

    private var isMarkdown: Bool {
        let ext = (path as NSString).pathExtension.lowercased()
        return ext == "md" || ext == "markdown"
    }

    var body: some View {
        VStack(spacing: 0) {
            ExplorerDetailHeader(
                title: (path as NSString).lastPathComponent,
                subtitle: path,
                onBack: { store.back() },
            ) {
                if isMarkdown {
                    PreviewModeToggle(rendered: $renderMarkdown)
                }
                if let size = store.previewImageSize {
                    Text("\(Int(size.width)) × \(Int(size.height))")
                        .font(Theme.monoXs)
                        .foregroundStyle(Theme.muted)
                        .fixedSize()
                }
                if let preview = store.preview {
                    Text(explorerFormatBytes(preview.size))
                        .font(Theme.monoXs)
                        .foregroundStyle(Theme.muted)
                        .fixedSize()
                }
                GhostButton(size: 11) {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(path, forType: .string)
                    ToastCenter.shared.post("Path copied", info: true)
                } label: {
                    Image(systemName: "doc.on.doc")
                }
                .frame(minWidth: 24, minHeight: 22)
                .help("Copy workspace-relative path")
                .accessibilityLabel("Copy path")
            }
            Hairline(axis: .horizontal)
            content
        }
    }

    @ViewBuilder
    private var content: some View {
        if store.previewLoading {
            ProgressView()
                .controlSize(.small)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if let error = store.previewError {
            ExplorerPlaceholder(
                systemImage: "exclamationmark.triangle", title: "Could not read file",
                detail: error,
            )
        } else if let preview = store.preview {
            if preview.binary == true {
                if explorerIsImagePath(preview.path) {
                    imageBody(preview)
                } else {
                    ExplorerPlaceholder(
                        systemImage: "doc.questionmark", title: "Binary file",
                        detail: explorerFormatBytes(preview.size),
                    )
                }
            } else {
                textBody(preview)
            }
        }
    }

    /// Raster image preview: raw bytes via the store (cached), fit to
    /// the panel, click to zoom in the window-level lightbox (the
    /// transcript's InlineImage idiom).
    @ViewBuilder
    private func imageBody(_ preview: WorkspaceFileContent) -> some View {
        if let image = store.previewImage {
            Button {
                NotificationCenter.default.post(name: .loomZoomImage, object: image)
            } label: {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding(12)
            }
            .buttonStyle(.plain)
            .help("Click to enlarge")
            .accessibilityLabel("\(preview.path), click to enlarge")
        } else if store.previewImageFailed {
            ExplorerPlaceholder(
                systemImage: "photo", title: "Could not load image",
                detail: explorerFormatBytes(preview.size),
            )
        } else {
            ProgressView()
                .controlSize(.small)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .task(id: preview.path) { store.loadPreviewImage(preview.path) }
        }
    }

    private func textBody(_ preview: WorkspaceFileContent) -> some View {
        VStack(spacing: 0) {
            if preview.truncated == true {
                Text("Showing the first 256 KB — the file is \(explorerFormatBytes(preview.size))")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.warning)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 5)
                    .background(Theme.warning.opacity(0.08))
                Hairline(axis: .horizontal)
            }
            if isMarkdown, renderMarkdown {
                ScrollView(.vertical) {
                    MarkdownText(source: preview.content ?? "")
                        .textSelection(.enabled)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 10)
                }
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    ScrollView(.vertical) {
                        Text(PreviewHighlight.attributed(
                            preview.content ?? "", path: preview.path,
                        ))
                        .textSelection(.enabled)
                        .lineSpacing(4)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                    }
                }
            }
        }
    }
}

/// Rendered/source mode switch for previews with two representations
/// (markdown today; the same control shape fits an image preview
/// later). Two compact segments in a bordered bg1 capsule.
private struct PreviewModeToggle: View {
    @Binding var rendered: Bool

    var body: some View {
        HStack(spacing: 0) {
            segment(isRendered: true, systemImage: "eye", help: "Rendered")
            segment(
                isRendered: false,
                systemImage: "chevron.left.forwardslash.chevron.right", help: "Source",
            )
        }
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusSm)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }

    private func segment(isRendered value: Bool, systemImage: String, help: String) -> some View {
        Button {
            rendered = value
        } label: {
            Image(systemName: systemImage)
                .font(.system(size: 10))
                .foregroundStyle(rendered == value ? Theme.primary : Theme.muted)
                .frame(width: 26, height: 20)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
        .accessibilityLabel(help)
        .accessibilityAddTraits(rendered == value ? .isSelected : [])
    }
}

// MARK: - Shared detail chrome

/// Drill-down header: back chevron, name over the workspace-relative
/// path, trailing accessories (pills, size, actions).
private struct ExplorerDetailHeader<Trailing: View>: View {
    let title: String
    let subtitle: String
    let onBack: () -> Void
    @ViewBuilder let trailing: Trailing

    var body: some View {
        HStack(spacing: 4) {
            GhostButton(size: 12, action: onBack) {
                Image(systemName: "chevron.left")
            }
            .frame(minWidth: 26, minHeight: 24)
            .help("Back")
            .accessibilityLabel("Back")
            VStack(alignment: .leading, spacing: 0) {
                Text(title)
                    .font(.system(size: Theme.textSm, weight: .semibold))
                    .foregroundStyle(Theme.fg)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(subtitle)
                    .font(.system(size: Theme.textXs, design: .monospaced))
                    .foregroundStyle(Theme.muted)
                    .lineLimit(1)
                    .truncationMode(.head)
            }
            Spacer(minLength: 6)
            trailing
        }
        .padding(.leading, 4)
        .padding(.trailing, 10)
        .padding(.vertical, 5)
    }
}

/// Tiny status capsule (untracked / truncated markers in detail headers).
private struct ExplorerPill: View {
    let text: String
    let color: Color

    var body: some View {
        Text(text)
            .font(.system(size: 9, weight: .semibold))
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(color.opacity(0.12), in: Capsule())
            .overlay(Capsule().strokeBorder(color.opacity(0.4), lineWidth: 1))
    }
}

/// Centered icon + title + hint used by every empty/loading-error
/// state in the panel.
private struct ExplorerPlaceholder: View {
    let systemImage: String
    let title: String
    var detail: String?

    init(systemImage: String, title: String, detail: String? = nil) {
        self.systemImage = systemImage
        self.title = title
        self.detail = detail
    }

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: systemImage)
                .font(.system(size: 22))
                .foregroundStyle(Theme.muted.opacity(0.5))
            Text(title)
                .font(.system(size: Theme.textMd, weight: .medium))
                .foregroundStyle(Theme.muted)
            if let detail {
                Text(detail)
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted.opacity(0.7))
                    .multilineTextAlignment(.center)
                    .lineLimit(3)
            }
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Helpers

/// Git status letter → change colour (VS Code idiom).
private func explorerGitColor(_ letter: String) -> Color {
    switch letter {
    case "M": Theme.warning
    case "A", "U": Theme.success
    case "D": Theme.error
    case "R", "T": Theme.purple
    default: Theme.muted
    }
}

/// SF Symbol per file kind/extension — quiet monochrome glyphs, so the
/// tree stays typographic rather than turning into Finder.
private func explorerFileIcon(name: String, kind: WorkspaceFileEntry.Kind) -> String {
    if kind == .dir {
        return "folder"
    }
    return switch (name as NSString).pathExtension.lowercased() {
    case "swift": "swift"
    case "png", "jpg", "jpeg", "gif", "webp", "svg", "icns", "heic": "photo"
    case "md", "markdown", "adoc": "doc.richtext"
    case "json", "yaml", "yml", "toml", "xml", "proto": "curlybraces"
    case "zip", "gz", "tgz", "tar", "xz", "bz2", "jar": "doc.zipper"
    case "lock", "sum", "mod": "lock.doc"
    default: "doc.text"
    }
}

private let explorerByteFormatter: ByteCountFormatter = {
    let formatter = ByteCountFormatter()
    formatter.countStyle = .file
    return formatter
}()

private func explorerFormatBytes(_ size: Int64) -> String {
    explorerByteFormatter.string(fromByteCount: size)
}

/// Raster image extensions the preview renders as images. SVG is
/// deliberately absent: the server classifies it as text (looksBinary
/// whitelists image/svg), so SVGs already preview as highlighted
/// source — and NSImage's SVG decoding is unreliable anyway.
func explorerIsImagePath(_ path: String) -> Bool {
    switch (path as NSString).pathExtension.lowercased() {
    case "png", "jpg", "jpeg", "gif", "webp", "ico", "bmp", "tiff", "tif", "heic", "heif": true
    default: false
    }
}

/// Memoized preview highlighting. Highlighting runs the hljs JSContext
/// synchronously (SyntaxHighlighter is main-actor confined), so beyond
/// a size ceiling the preview falls back to plain mono text rather
/// than stalling the panel on a 256 KB file.
@MainActor
private enum PreviewHighlight {
    private static let highlightByteLimit = 128 * 1024

    private final class Box: NSObject {
        let value: AttributedString

        init(_ value: AttributedString) {
            self.value = value
        }
    }

    private static let cache: NSCache<NSString, Box> = {
        let cache = NSCache<NSString, Box>()
        cache.countLimit = 24
        return cache
    }()

    static func attributed(_ content: String, path: String) -> AttributedString {
        guard content.utf8.count <= highlightByteLimit else {
            return SyntaxHighlighter.plain(content)
        }
        let key = "\(path)#\(content.hashValue)" as NSString
        if let hit = cache.object(forKey: key) {
            return hit.value
        }
        let ext = (path as NSString).pathExtension
        let rendered = SyntaxHighlighter.attributed(content, language: ext.isEmpty ? nil : ext)
        cache.setObject(Box(rendered), forKey: key)
        return rendered
    }
}
