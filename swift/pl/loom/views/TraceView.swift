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

// MARK: - Trace: the session's execution trace as a dense event list

//
// Port of the WebUI's trace tab (webui/src/components/trace/TraceView.tsx
// + styles/trace.css): one line per prompt / assistant message / tool
// call, grouped into turns by a gutter rail. Sits between the chat tab
// (full fidelity) and the maze tab (macro shape) — the debugging view.
// Badges color by actor (Input=info / Model=purple / Tools=highlight),
// orthogonal to the maze's verdict coloring.

private enum TraceRowKind {
    case user, assistant, reasoning, tool, notice, error

    /// trace.css t-* badge: label + pill colors.
    var badge: (label: String, fg: Color, bg: Color) {
        switch self {
        case .user: ("USER", Theme.onAccent, Theme.info)
        case .assistant: ("ASSISTANT", Theme.onAccent, Theme.purple)
        case .reasoning: ("THINK", Theme.muted, Theme.bg2)
        case .tool: ("TOOL", Theme.onAccent, Theme.highlight)
        case .notice: ("SYS", Theme.muted, Theme.bg2)
        case .error: ("ERROR", Theme.onAccent, Theme.error)
        }
    }

    /// .trace-dot color (t-think renders the purple dot at 50%).
    var dotColor: Color {
        switch self {
        case .user: Theme.info
        case .assistant, .reasoning: Theme.purple
        case .tool: Theme.highlight
        case .notice: Theme.muted
        case .error: Theme.error
        }
    }
}

/// TraceView.tsx TraceRow: one render unit of the event list.
private struct TraceRow: Identifiable {
    let id: String
    let kind: TraceRowKind
    let turn: Int
    var text = ""
    /// Seconds since the first prompt (user/assistant); -1 when unknown.
    var ts: Double = -1
    /// Streaming assistant text (.trace-dur "Generating…").
    var live = false
    var reasoningMs: Int64?
    var tool: ToolRenderModel?
    /// .trace-row.is-warn (notice rows).
    var warn = false
}

private struct TraceTurnGroup: Identifiable {
    let turn: Int
    /// Seconds since the first prompt; -1 when unknown.
    var inputTs: Double
    var rows: [TraceRow]
    var id: Int {
        turn
    }
}

struct SessionTraceView: View {
    let store: SessionStore
    let locateInChat: (Int, String?) -> Void
    @Binding var targetTurn: Int?

    @State private var query = ""
    @State private var debouncedQuery = ""
    @State private var expanded: Set<String> = []
    @State private var maze: MazeData?
    @State private var following = true
    @State private var exporting = false
    @FocusState private var searchFocused: Bool

    private static let bottomId = "trace-bottom"

    var body: some View {
        let groups = visibleGroups
        VStack(spacing: 0) {
            toolbar
            Hairline(axis: .horizontal)
            if let maze, let lane = maze.lanes.first, lane.stats.steps > 0 {
                // Turn anchors come from the unfiltered groups (TraceView.tsx
                // inputs): searching must not shift the strip's seek targets.
                RhythmStrip(data: maze, inputs: self.groups.map(\.inputTs).filter { $0 >= 0 }) { turn in
                    following = false
                    scrollToTurn?(turn)
                }
                Hairline(axis: .horizontal)
            }
            if allRows.isEmpty {
                ContentUnavailableView("No trace yet", systemImage: "waveform.path",
                                       description: Text("Start a conversation and the full execution appears here."))
            } else {
                eventList(groups)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg0)
        .background {
            // ⌘F focuses the trace search (browser convention).
            Button("Search") { searchFocused = true }
                .keyboardShortcut("f", modifiers: .command)
                .opacity(0)
                .frame(width: 0, height: 0)
        }
        .task(id: query) {
            // Search debounce (TraceView.tsx 150ms): a tool row's haystack
            // holds full output text, so per-keystroke scans stall frames.
            try? await Task.sleep(for: .milliseconds(150))
            guard !Task.isCancelled else { return }
            debouncedQuery = query
        }
        .task(id: store.sessionId) {
            // Session switch: reset view state (the WebUI remounts with
            // key={sessionId}) and drop the previous maze before refetch.
            maze = nil
            expanded = []
            following = true
            await refreshMaze()
            // Slow backstop poll while the run is active: SSE events are
            // the primary freshness signal now, but a reconnect gap or a
            // long tool call emits none (see SessionMazeView).
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                guard !Task.isCancelled else { return }
                if store.isBusy {
                    await refreshMaze()
                }
            }
        }
        .task(id: store.activityGeneration) {
            // SSE events bump activityGeneration (tool lifecycle, model
            // responses, turn boundaries…); task(id:) cancellation IS
            // the debounce, so the rhythm strip grows live with the run
            // and settles with the final projection.
            guard maze != nil else { return }
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            await refreshMaze()
        }
    }

    private func refreshMaze() async {
        maze = try? await store.maze()
    }

    // MARK: Row model (TraceView.tsx buildGroups)

    private var groups: [TraceTurnGroup] {
        var groups: [TraceTurnGroup] = []
        var turn = 0
        var firstUserDate: Date?

        func push(_ row: TraceRow) {
            if groups.isEmpty {
                groups.append(TraceTurnGroup(turn: 0, inputTs: -1, rows: []))
            }
            groups[groups.count - 1].rows.append(row)
        }

        for row in store.transcript.rows {
            guard case let .message(model) = row else { continue }
            let message = model.message
            switch message.role {
            case .user:
                turn += 1
                if firstUserDate == nil {
                    firstUserDate = message.createdAt
                }
                groups.append(TraceTurnGroup(
                    turn: turn,
                    inputTs: message.createdAt?.timeIntervalSince1970 ?? -1,
                    rows: [TraceRow(
                        id: message.id, kind: .user, turn: turn,
                        text: message.copyText,
                        ts: message.createdAt?.timeIntervalSince1970 ?? -1,
                    )],
                ))
            case .assistant:
                for (index, item) in model.items.enumerated() {
                    let id = "\(message.id)-\(index)"
                    switch item {
                    case let .markdown(text):
                        push(TraceRow(
                            id: id, kind: .assistant, turn: turn, text: text,
                            ts: message.createdAt?.timeIntervalSince1970 ?? -1,
                        ))
                    case let .reasoning(reasoning):
                        let text = reasoning.redacted == true ? "Reasoning redacted" : (reasoning.text ?? "")
                        if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                            push(TraceRow(
                                id: id, kind: .reasoning, turn: turn,
                                text: text, reasoningMs: reasoning.durationMs,
                            ))
                        }
                    case let .tool(tool):
                        push(TraceRow(id: id, kind: .tool, turn: turn, tool: tool))
                    default:
                        break // image / artifact: no trace row
                    }
                }
                if message.status == .interrupted {
                    push(TraceRow(id: "\(message.id)-interrupted", kind: .error,
                                  turn: turn, text: "Response interrupted"))
                }
            case .system:
                push(TraceRow(id: message.id, kind: .notice, turn: turn, text: message.copyText))
            case .unknown:
                break
            }
        }
        // Pending gates are live notices (TraceView.tsx approval/question blocks).
        if !store.pendingApprovals.isEmpty {
            push(TraceRow(id: "pending-approval", kind: .notice, turn: turn,
                          text: "Awaiting approval…", warn: true))
        }
        if !store.pendingQuestions.isEmpty {
            push(TraceRow(id: "pending-question", kind: .notice, turn: turn,
                          text: "Awaiting answer…", warn: true))
        }
        if let draft = store.draft {
            let activeTurn = draft.turnIndex ?? max(1, turn)
            for segment in draft.segments {
                switch segment {
                case let .text(text) where !text.text.isEmpty:
                    push(TraceRow(id: segment.id, kind: .assistant, turn: activeTurn,
                                  text: text.text, live: text.live))
                case let .reasoning(reasoning) where !reasoning.text.isEmpty:
                    push(TraceRow(id: segment.id, kind: .reasoning, turn: activeTurn,
                                  text: reasoning.text))
                case let .tool(tool):
                    push(TraceRow(id: segment.id, kind: .tool, turn: activeTurn,
                                  tool: ToolRenderModel(live: tool)))
                default:
                    break
                }
            }
        }
        if let error = store.lastError {
            push(TraceRow(id: "last-error", kind: .error, turn: max(turn, 1), text: error))
        }

        // Rebase timestamps to seconds since the first prompt (maze's origin).
        let origin = firstUserDate?.timeIntervalSince1970 ?? -1
        for index in groups.indices {
            if groups[index].inputTs >= 0, origin >= 0 {
                groups[index].inputTs -= origin
            } else {
                groups[index].inputTs = -1
            }
            for rowIndex in groups[index].rows.indices {
                let ts = groups[index].rows[rowIndex].ts
                if ts >= 0, origin >= 0 {
                    groups[index].rows[rowIndex].ts = ts - origin
                }
            }
        }
        return groups
    }

    private var allRows: [TraceRow] {
        groups.flatMap(\.rows)
    }

    // MARK: Filtering (TraceView.tsx rowHaystack / visible)

    private var trimmedQuery: String {
        debouncedQuery.trimmingCharacters(in: .whitespaces).lowercased()
    }

    private var searching: Bool {
        !trimmedQuery.isEmpty
    }

    private func haystack(_ row: TraceRow) -> String {
        if let tool = row.tool {
            return [tool.name, tool.target ?? "", tool.output ?? "", tool.fullOutput ?? ""]
                .joined(separator: "\n")
                .lowercased()
        }
        return row.text.lowercased()
    }

    private var visibleGroups: [TraceTurnGroup] {
        guard searching else { return groups }
        return groups.compactMap { group in
            let rows = group.rows.filter { haystack($0).contains(trimmedQuery) }
            return rows.isEmpty ? nil : TraceTurnGroup(turn: group.turn, inputTs: group.inputTs, rows: rows)
        }
    }

    private var hitCount: Int {
        guard searching else { return -1 }
        return visibleGroups.reduce(0) { $0 + $1.rows.count }
    }

    // MARK: Metrics (.trace-metrics)

    private var metrics: (turns: Int, calls: Int, duration: Double) {
        var turns = 0
        var calls = 0
        var minTs = -1.0
        var maxTs = -1.0
        for group in groups {
            for row in group.rows {
                if row.kind == .user {
                    turns += 1
                }
                if row.kind == .tool {
                    calls += 1
                }
                if row.kind == .user || row.kind == .assistant, row.ts >= 0 {
                    if minTs < 0 || row.ts < minTs {
                        minTs = row.ts
                    }
                    maxTs = max(maxTs, row.ts)
                }
            }
        }
        return (turns, calls, minTs >= 0 ? maxTs - minTs : -1)
    }

    private var toolbar: some View {
        HStack(spacing: 14) {
            let m = metrics
            HStack(spacing: 14) {
                Text("⏱ \(m.duration >= 0 ? mazeFormatDur(m.duration) : "—")")
                    .help("From the first message to the latest activity")
                Text("⚇ \(m.turns) turns")
                Text("⌗ \(m.calls) calls")
            }
            .font(.system(size: Theme.textXs))
            .foregroundStyle(Theme.muted)
            Spacer(minLength: 8)
            MiniSearchField(placeholder: "Search trace…", text: $query,
                            externalFocus: $searchFocused)
            if hitCount >= 0 {
                Text("\(hitCount) hits")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.primary)
            }
            if exporting {
                ProgressView()
                    .controlSize(.small)
                    .frame(width: 15, height: 15)
            } else {
                MazeButton(
                    title: "Session log",
                    systemImage: "arrow.down.to.line",
                    help: "Download raw event log (NDJSON)",
                ) { exportLog() }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    /// TracePage downloadLog: GET /v1/sessions/{id}/export → save NDJSON.
    private func exportLog() {
        guard !exporting else { return }
        exporting = true
        Task {
            defer { exporting = false }
            do {
                let data = try await store.exportSessionLog()
                let panel = NSSavePanel()
                panel.nameFieldStringValue = "loom-session-\(store.sessionId).jsonl"
                guard panel.runModal() == .OK, let url = panel.url else { return }
                do {
                    try data.write(to: url)
                } catch {
                    exportError("Failed to save session log: \(error.localizedDescription)")
                }
            } catch {
                exportError("Failed to export session log: \(error.localizedDescription)")
            }
        }
    }

    @MainActor
    private func exportError(_ message: String) {
        let alert = NSAlert()
        alert.messageText = "Export failed"
        alert.informativeText = message
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    // MARK: Event list (.trace-list)

    /// Wired by the ScrollViewReader inside eventList so the rhythm strip
    /// can seek turns (TraceView.tsx seekTurn).
    @State private var scrollToTurn: ((Int) -> Void)?

    private func eventList(_ groups: [TraceTurnGroup]) -> some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(groups) { group in
                        turnGroup(group)
                    }
                    // Bottom sentinel: drives follow mode + the jump button.
                    Color.clear
                        .frame(height: 1)
                        .id(Self.bottomId)
                        .onAppear { following = true }
                        .onDisappear { following = false }
                }
                .padding(.horizontal, 12)
                .padding(.top, 6)
                .padding(.bottom, 16)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .overlay(alignment: .bottom) {
                if !following {
                    Button {
                        following = true
                        withAnimation(.easeOut(duration: 0.2)) {
                            proxy.scrollTo(Self.bottomId, anchor: .bottom)
                        }
                    } label: {
                        HStack(spacing: 5) {
                            Image(systemName: "arrow.down")
                            Text("Back to bottom")
                        }
                        .font(.system(size: Theme.textXs, weight: .medium))
                        .foregroundStyle(Theme.primary)
                        .padding(.horizontal, 11)
                        .padding(.vertical, 5)
                        .background(Theme.bg1, in: Capsule())
                        .overlay(Capsule().strokeBorder(Theme.bg2, lineWidth: 1))
                        .shadow(color: .black.opacity(0.3), radius: 4, y: 2)
                    }
                    .buttonStyle(.plain)
                    .padding(.bottom, 12)
                    .transition(.opacity)
                }
            }
            .animation(.easeInOut(duration: 0.15), value: following)
            .onAppear {
                scrollToTurn = { turn in
                    // Turn-group ids ARE the turn numbers (turn 0 = the
                    // pre-first-prompt group), no lookup needed.
                    proxy.scrollTo(max(turn, 0), anchor: .top)
                }
                if let turn = targetTurn {
                    following = false
                    DispatchQueue.main.async {
                        proxy.scrollTo(turn, anchor: .top)
                        targetTurn = nil
                    }
                } else if store.isBusy, following {
                    // Opening the tab mid-run starts parked at the tail.
                    DispatchQueue.main.async {
                        proxy.scrollTo(Self.bottomId, anchor: .bottom)
                    }
                }
            }
            .onChange(of: targetTurn) { _, turn in
                if let turn {
                    following = false
                    proxy.scrollTo(turn, anchor: .top)
                    targetTurn = nil
                }
            }
            // Continuous follow: every applied stream flush bumps
            // store.streamRevision (~25fps while streaming) — the
            // viewport tracks the growing tail row continuously instead
            // of jumping per 256-char bucket; structural additions
            // (new rows) snap the tail too. Scrolling up or searching
            // holds the viewport still.
            .onChange(of: store.streamRevision) { _, _ in
                snapToBottom(proxy)
            }
            .onChange(of: allRows.count) { _, _ in
                snapToBottom(proxy)
            }
        }
    }

    private func snapToBottom(_ proxy: ScrollViewProxy) {
        guard store.isBusy, following, !searching else { return }
        proxy.scrollTo(Self.bottomId, anchor: .bottom)
    }

    /// .trace-turn: the gutter rail connects the first dot to the last.
    private func turnGroup(_ group: TraceTurnGroup) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(group.rows) { row in
                TraceRowView(
                    row: row,
                    expanded: expanded.contains(row.id),
                    onToggle: { toggle(row.id) },
                    onLocateInChat: { callId in locateInChat(row.turn, callId) },
                )
            }
        }
        .padding(.top, 4)
        .padding(.bottom, 8)
        // The rail sits BEHIND the dots (trace-turn::before): each dot
        // occludes the line where it sits, so it reads as a connector
        // between rows, not a line drawn over them.
        .background(alignment: .leading) {
            Rectangle()
                .fill(Theme.bg2)
                .frame(width: 2)
                .clipShape(RoundedRectangle(cornerRadius: 1))
                .padding(.vertical, 15)
                .padding(.leading, 15)
                .allowsHitTesting(false)
        }
        .id(group.id)
    }

    private func toggle(_ id: String) {
        if expanded.contains(id) {
            expanded.remove(id)
        } else {
            expanded.insert(id)
        }
    }
}

// MARK: - Rhythm strip (.trace-strip)

/// Three mini swimlanes (Input/Model/Tools) on the session timeline,
/// painted from the maze payload; click jumps to the matching turn.
private struct RhythmStrip: View {
    let data: MazeData
    /// Turn input times, seconds since the first prompt.
    let inputs: [Double]
    let onSeek: (Int) -> Void

    var body: some View {
        HStack(spacing: 8) {
            // One 13px label row per swimlane (STRIP_ROW_H): Spacers here
            // used to make the column flexible, stretching the whole strip
            // to fill the window instead of hugging its 39px canvas.
            VStack(alignment: .leading, spacing: 0) {
                Text("In").frame(height: 13)
                Text("Model").frame(height: 13)
                Text("Tool").frame(height: 13)
            }
            .font(.system(size: 10))
            .foregroundStyle(Theme.muted)
            GeometryReader { geo in
                Canvas { ctx, size in
                    let total = max(data.tmax, 1)
                    func x(_ s: Double) -> CGFloat {
                        CGFloat(s / total) * size.width
                    }
                    func bar(_ s: Double, _ e: Double) -> CGRect {
                        let x0 = x(s)
                        return CGRect(x: x0, y: 0, width: max(x(e) - x0, 4), height: 9)
                    }
                    for input in inputs {
                        var rect = bar(input, input + total * 0.004)
                        rect.origin.y = 2
                        ctx.fill(Path(rect), with: .color(Theme.info))
                    }
                    for node in data.lanes[0].main + data.lanes[0].detours {
                        var rect = bar(node.s, node.e)
                        rect.origin.y = 15
                        ctx.fill(Path(rect), with: .color(Theme.purple))
                        for tool in node.tools {
                            var toolRect = bar(tool.s, tool.e ?? node.e)
                            toolRect.origin.y = 28
                            ctx.fill(Path(toolRect), with: .color(Theme.highlight))
                        }
                    }
                }
                .contentShape(Rectangle())
                .onTapGesture { point in
                    seek(atX: point.x, width: geo.size.width)
                }
            }
            .frame(height: 39)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .help("Click to jump to the corresponding turn")
    }

    /// Strip click → the turn owning that moment (TraceView.tsx onClick).
    private func seek(atX x: CGFloat, width: CGFloat) {
        let total = max(data.tmax, 1)
        guard width > 0 else { return }
        let t = Double(x / width) * total
        var turn = 0
        for input in inputs {
            if input <= t {
                turn += 1
            } else {
                break
            }
        }
        onSeek(turn)
    }
}

// MARK: - Event rows (.trace-row)

private struct TraceRowView: View {
    let row: TraceRow
    let expanded: Bool
    let onToggle: () -> Void
    let onLocateInChat: (String?) -> Void

    @State private var hovered = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: onToggle) {
                HStack(spacing: 8) {
                    Circle()
                        .fill(row.kind.dotColor)
                        .frame(width: 8, height: 8)
                        .opacity(row.kind == .reasoning ? 0.5 : 1)
                        .padding(.leading, 12)
                    badge
                    rowText
                        .frame(maxWidth: .infinity, alignment: .leading)
                    durLabel
                    Image(systemName: expanded ? "chevron.down" : "chevron.right")
                        .font(.system(size: 10))
                        .foregroundStyle(Theme.muted)
                }
                .padding(.vertical, 3)
                .padding(.trailing, 8)
                .contentShape(Rectangle())
                .background(
                    hovered ? Theme.bg1 : Color.clear,
                    in: RoundedRectangle(cornerRadius: Theme.radiusSm),
                )
            }
            .buttonStyle(.plain)
            .onHover { hovered = $0 }
            .accessibilityLabel(accessibilityText)
            if expanded {
                TraceRowDetail(row: row, onLocateInChat: onLocateInChat)
            }
        }
    }

    private var accessibilityText: String {
        "\(row.kind.badge.label): \(row.tool.map { "\($0.name) \($0.target ?? "")" } ?? row.text)"
    }

    private var badge: some View {
        let config = row.kind.badge
        return Text(config.label)
            .font(.system(size: 10, weight: .bold))
            .tracking(0.4)
            .foregroundStyle(config.fg)
            .padding(.horizontal, 6)
            .padding(.vertical, 1)
            .background(config.bg, in: Capsule())
    }

    @ViewBuilder
    private var rowText: some View {
        if let tool = row.tool {
            // .trace-text.mono: toolname + target + " → result".
            let failed = tool.status == .failed
            let result = tool.status == .running
                ? ""
                : Self.firstLine(tool.errorMessage ?? tool.output ?? "")
            (Text(tool.name).font(.system(size: Theme.textXs, design: .monospaced).weight(.semibold))
                .foregroundStyle(Theme.primary)
                + Text(" \(Self.firstLine(tool.target ?? ""))")
                .font(Theme.monoXs)
                .foregroundStyle(Theme.fg)
                + Text(result.isEmpty ? "" : " → \(result)")
                .font(Theme.monoXs)
                .foregroundStyle(failed ? Theme.error : Theme.muted))
                .lineLimit(1)
                .truncationMode(.tail)
        } else {
            let text = Self.firstLine(row.text)
            Text(text.isEmpty ? "(empty)" : text)
                .font(.system(size: Theme.textSm))
                .foregroundStyle(row.kind == .error ? Theme.error
                    : (row.kind == .notice && row.warn) ? Theme.warning : Theme.fg)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    @ViewBuilder
    private var durLabel: some View {
        let label: String? = switch row.kind {
        case .tool:
            row.tool?.status == .running
                ? "Running…"
                : row.tool?.durationMs.map(Self.fmtMs)
        case .reasoning:
            row.reasoningMs.flatMap { $0 > 0 ? Self.fmtMs($0) : nil }
        case .assistant:
            row.live ? "Generating…" : nil
        default:
            nil
        }
        if let label, !label.isEmpty {
            Text(label)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.muted)
        }
    }

    static func firstLine(_ s: String) -> String {
        s.components(separatedBy: "\n").first ?? ""
    }

    /// TraceView.tsx fmtMs: sub-second in ms, then formatDur.
    static func fmtMs(_ ms: Int64) -> String {
        ms < 1000 ? "\(ms)ms" : mazeFormatDur(Double(ms) / 1000)
    }
}

// MARK: - Inline expansion (.trace-expand)

private struct TraceRowDetail: View {
    let row: TraceRow
    let onLocateInChat: (String?) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            switch row.kind {
            case .assistant:
                MarkdownText(source: row.text)
            case .tool:
                if let tool = row.tool {
                    if let target = tool.target, !target.isEmpty {
                        block("Args", text: target)
                    }
                    if let diff = tool.diff, !diff.isEmpty {
                        block("Changes", text: diff)
                    }
                    if tool.status != .running {
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Result")
                                .font(.system(size: Theme.textXs))
                                .foregroundStyle(Theme.muted)
                            if let error = tool.errorMessage, !error.isEmpty {
                                Text(error)
                                    .font(.system(size: Theme.textXs))
                                    .foregroundStyle(Theme.error)
                            }
                            pre(tool.fullOutput ?? tool.output ?? "(no output)")
                        }
                    }
                    if tool.callId != nil {
                        MazeButton(
                            title: "Locate in conversation",
                            systemImage: "arrow.turn.down.right",
                        ) { onLocateInChat(tool.callId) }
                    }
                }
            case .user, .reasoning, .error, .notice:
                pre(row.text)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusSm)
            .strokeBorder(Theme.bg2, lineWidth: 1))
        .padding(.leading, 24)
        .padding(.bottom, 6)
        .padding(.top, 2)
    }

    private func block(_ title: String, text: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.muted)
            pre(text)
        }
    }

    /// .trace-expand-pre: mono block on bg0, scrolls past 240px.
    private func pre(_ text: String) -> some View {
        ScrollView {
            Text(text)
                .font(Theme.monoXs)
                .foregroundStyle(Theme.fg)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxHeight: 240)
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
    }
}
