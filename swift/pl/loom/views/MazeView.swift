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

// MARK: - Maze: server-judged execution path with attached detours

//
// Port of the WebUI's execution-trace maze (webui/src/components/maze/
// MazeView.tsx + styles/maze.css): the session paints as a timeline of
// main-path duration capsules with dashed detour arcs hanging off them,
// an idle-folded time axis (gaps > 20s collapse into ⏸ seams), a tick
// strip, a drag-to-zoom brush, Shift-drag/trackpad pan, wheel zoom
// around the cursor, hover cards, and a right-side detail panel.
// Single-lane only — the WebUI's compare mode (two lanes, alignment
// lines, branch audit) has no desktop peer.

/// maze.css layout constants (MazeView.tsx).
private enum MazeMetrics {
    static let laneHeaderH: CGFloat = 34
    static let mainH: CGFloat = 30
    static let parBarH: CGFloat = 8
    static let detourH: CGFloat = 30
    static let axisH: CGFloat = 26
    static let gapBandH: CGFloat = 16
    static let padX: CGFloat = 12
    static let minBarW: CGFloat = 5
    /// Viewport culling margin (MazeView.tsx CULL_MARGIN).
    static let cullMargin: CGFloat = 320
    static let detailWidth: CGFloat = 340
}

extension MazeVerdict {
    /// maze.css verdict → token color (capsules, sub-bars, legend dots).
    var color: Color {
        switch self {
        case .ok: Theme.primary
        case .answer: Theme.info
        case .error: Theme.error
        case .deadend: Theme.muted
        case .retry: Theme.warning
        case .pending: Theme.primary
        }
    }

    /// Rect-level opacity on top of the group opacity (the .v-deadend /
    /// .v-pending rules in maze.css).
    var fillOpacity: Double {
        switch self {
        case .deadend: 0.75
        case .pending: 0.55
        default: 1
        }
    }

    var title: String {
        switch self {
        case .ok: "OK"
        case .answer: "Answer"
        case .error: "Error"
        case .deadend: "Dead end"
        case .retry: "No-op retry"
        case .pending: "Running"
        }
    }

    /// .maze-badge colors (pending is a quiet bg2 badge; the rest are
    /// filled verdict color with on-accent text).
    var badgeBackground: Color {
        self == .pending ? Theme.bg2 : color
    }

    var badgeForeground: Color {
        self == .pending ? Theme.fg : Theme.onAccent
    }
}

// MARK: - Scene layout

private struct MazeLaneLayout {
    let lane: MazeLane
    let top: CGFloat
    let height: CGFloat
    /// Max parallel tool sub-bar rows any main step occupies.
    let maxPar: Int
    /// Greedy row assignment for the lane's detours (never overlap).
    let detourRows: [Int]
    /// step → main-node lookup for a detour's attach anchor.
    let byStep: [Int: MazeNode]

    var mainY: CGFloat {
        top + MazeMetrics.laneHeaderH + MazeMetrics.mainH / 2
    }

    var detourTop: CGFloat {
        top + MazeMetrics.laneHeaderH + MazeMetrics.mainH + CGFloat(maxPar) * MazeMetrics.parBarH + 4
    }
}

/// Everything the canvas and the gesture handlers need, computed once
/// per (data, width, window) triple — mirroring MazeView.tsx's memos.
private struct MazeScene {
    let axis: MazeAxis
    let lanes: [MazeLaneLayout]
    let totalH: CGFloat
    let width: CGFloat
    let dStart: Double
    let dEnd: Double
    /// Wall-clock seconds elapsed since the data was fetched: live nodes'
    /// capsules keep growing between refetches (the desktop "now line") —
    /// the web only advances them on its 1s backstop refetch.
    let liveElapsed: Double

    init(data: MazeData, width: CGFloat, window: ClosedRange<Double>?, liveElapsed: Double = 0) {
        self.liveElapsed = max(0, liveElapsed)
        var ranges: [(Double, Double)] = []
        var tmax = data.tmax
        for lane in data.lanes {
            for node in lane.main + lane.detours {
                // Inline effectiveEnd: instance methods can't run until
                // every stored property is initialized.
                let e = node.live == true ? node.e + self.liveElapsed : node.e
                ranges.append((node.s, e))
                tmax = max(tmax, e)
                for tool in node.tools {
                    ranges.append((tool.s, tool.e ?? e))
                }
            }
        }
        axis = MazeAxis(ranges: ranges, tmax: tmax)

        let total = max(axis.total, 1)
        let clamped = window.map {
            max(0, min($0.lowerBound, total)) ... max(0, min($0.upperBound, total))
        }
        dStart = clamped?.lowerBound ?? 0
        dEnd = clamped?.upperBound ?? total
        self.width = width

        var top: CGFloat = 0
        var layouts: [MazeLaneLayout] = []
        for lane in data.lanes {
            let maxPar = lane.main.reduce(0) { max($0, $1.tools.count > 1 ? $1.tools.count : 0) }
            let rows = mazePackRows(lane.detours.map { (s: $0.s, e: $0.e) })
            let detourRowCount = (rows.max() ?? -1) + 1
            let height = MazeMetrics.laneHeaderH + MazeMetrics.mainH
                + CGFloat(maxPar) * MazeMetrics.parBarH
                + CGFloat(detourRowCount) * MazeMetrics.detourH + 8
            var byStep: [Int: MazeNode] = [:]
            for node in lane.main {
                byStep[node.step] = node
            }
            layouts.append(MazeLaneLayout(
                lane: lane, top: top, height: height, maxPar: maxPar,
                detourRows: rows, byStep: byStep,
            ))
            top += height
        }
        lanes = layouts
        let bandH: CGFloat = axis.gaps.isEmpty ? 0 : MazeMetrics.gapBandH
        totalH = top + bandH + MazeMetrics.axisH
    }

    /// Display-domain → canvas x (MazeView.tsx toX).
    func toX(_ d: Double) -> CGFloat {
        MazeMetrics.padX + CGFloat((d - dStart) / max(dEnd - dStart, 1e-6))
            * (width - MazeMetrics.padX * 2)
    }

    /// Canvas x → display-domain (brush/label math).
    func domain(atX x: CGFloat) -> Double {
        let w = max(width - MazeMetrics.padX * 2, 1)
        let frac = Double(min(max((x - MazeMetrics.padX) / w, 0), 1))
        return dStart + frac * (dEnd - dStart)
    }

    /// A node's effective end: live capsules grow with the wall clock
    /// between refetches.
    func effectiveEnd(_ node: MazeNode) -> Double {
        node.live == true ? node.e + liveElapsed : node.e
    }

    /// Duration-capsule rect for a node (MazeView.tsx barProps).
    func barProps(_ node: MazeNode) -> (x: CGFloat, w: CGFloat) {
        let x1 = toX(axis.map(node.s))
        let x2 = toX(axis.map(effectiveEnd(node)))
        return (x1, max(x2 - x1, MazeMetrics.minBarW))
    }
}

/// Hit target for click/hover: a node's capsule rect in canvas coordinates.
private struct MazeHitFrame {
    let node: MazeNode
    let rect: CGRect
}

// MARK: - SessionMazeView

struct SessionMazeView: View {
    let store: SessionStore
    let locateInTrace: (Int) -> Void

    @State private var data: MazeData?
    @State private var error: String?
    @State private var selected: MazeNode?
    @State private var failOnly = false
    @State private var query = ""
    @State private var debouncedQuery = ""
    /// Display-domain zoom window; nil = whole map.
    @State private var window: ClosedRange<Double>?
    @State private var brush: (x0: CGFloat, x1: CGFloat)?
    @State private var dragState: DragState?
    @State private var pinchLast: CGFloat = 1
    @State private var hover: (point: CGPoint, card: HoverCard)?
    @State private var refreshGeneration = 0
    /// Vertical scroll offset of the maze content (wheel-driven; the
    /// content is taller than the viewport only on dense maps).
    @State private var scrollY: CGFloat = 0
    /// Wall-clock of the last successful /maze fetch — the base for the
    /// live capsules' between-refresh extrapolation.
    @State private var fetchedAt = Date()
    /// First-seen timestamp per "lane:step" — drives the ~150ms fade-in
    /// of freshly materialized nodes while the pulse ticker runs.
    @State private var firstSeen: [String: Date] = [:]
    @FocusState private var searchFocused: Bool

    private enum DragMode { case brush, pan }

    private struct DragState {
        let mode: DragMode
        let startX: CGFloat
        let window: ClosedRange<Double>
    }

    private struct HoverCard {
        let title: String
        let lines: [String]
    }

    var body: some View {
        VStack(spacing: 0) {
            if let error {
                ContentUnavailableView("Maze unavailable", systemImage: "exclamationmark.triangle",
                                       description: Text(error))
                    .overlay(alignment: .bottom) {
                        Button("Retry") { Task { await refresh() } }.padding(20)
                    }
            } else if let data {
                if data.lanes.isEmpty || data.lanes.allSatisfy({ $0.stats.steps == 0 }) {
                    ContentUnavailableView(
                        store.isBusy ? "Building execution path…" : "No execution trace yet",
                        systemImage: "point.topleft.down.curvedto.point.bottomright.up",
                        description: Text(store.isBusy
                            ? "New steps will appear here as the run progresses."
                            : "Start a conversation and the exploration maze appears here."),
                    )
                } else {
                    toolbar(data)
                    Hairline(axis: .horizontal)
                    mazeBody(data)
                }
            } else {
                ProgressView("Loading maze…").frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.bg0)
        .task(id: query) {
            // Search debounce (MazeView.tsx 150ms): filtering joins full
            // tool args/results, so per-keystroke recompute stalls frames.
            try? await Task.sleep(for: .milliseconds(150))
            guard !Task.isCancelled else { return }
            debouncedQuery = query
        }
        .task(id: store.sessionId) {
            // Session switch: reset the view state (the WebUI remounts
            // with key={sessionId}) and drop the previous maze BEFORE
            // fetching — otherwise the old session's map flashes under
            // the new one (useMazeData).
            data = nil
            error = nil
            window = nil
            selected = nil
            hover = nil
            scrollY = 0
            firstSeen = [:]
            await refresh()
            // Slow backstop poll while the run is active: SSE events are
            // the primary freshness signal now, but a reconnect gap or a
            // long tool call emits none — the poll re-anchors the live
            // extrapolation and catches whatever the stream missed.
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                guard !Task.isCancelled else { return }
                if store.isBusy {
                    await refresh()
                }
            }
        }
        .task(id: store.activityGeneration) {
            // SSE events bump activityGeneration (tool lifecycle, model
            // responses, turn boundaries…). task(id:) cancellation IS
            // the debounce: a burst of events collapses into one
            // refetch, and the turn.finished bump captures the final
            // projection after the run settles.
            guard data != nil else { return }
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            await refresh()
        }
    }

    private func refresh() async {
        refreshGeneration += 1
        let generation = refreshGeneration
        do {
            let fresh = try await store.maze()
            guard !Task.isCancelled, generation == refreshGeneration else { return }
            data = fresh
            error = nil
            fetchedAt = Date()
            for lane in fresh.lanes {
                for node in lane.main + lane.detours {
                    let key = "\(lane.key):\(node.step)"
                    if firstSeen[key] == nil {
                        firstSeen[key] = fetchedAt
                    }
                }
            }
            if let selected {
                self.selected = fresh.lanes.first?.main.first { $0.step == selected.step }
                    ?? fresh.lanes.first?.detours.first { $0.step == selected.step }
            }
        } catch {
            guard !Task.isCancelled, generation == refreshGeneration else { return }
            if data == nil {
                self.error = error.localizedDescription
            }
        }
    }

    // MARK: Toolbar (maze.css .maze-toolbar)

    private func toolbar(_ data: MazeData) -> some View {
        HStack(spacing: 14) {
            legend
            Spacer(minLength: 8)
            Toggle("Errors/retries only", isOn: $failOnly)
                .toggleStyle(.checkbox)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(failOnly ? Theme.primary : Theme.muted)
            MiniSearchField(placeholder: "Search commands/results…", text: $query,
                            externalFocus: $searchFocused)
            if filtering, hitCount(data) >= 0 {
                Text("\(hitCount(data)) hits")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.primary)
            }
            MazeButton(title: "⤢ Full view", help: "Reset zoom") { window = nil }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    private var legend: some View {
        HStack(spacing: 14) {
            legendItem(Theme.primary, "Main path")
            legendItem(Theme.info, "Answer")
            legendItem(Theme.error, "Error")
            legendItem(Theme.muted, "Dead end")
            legendItem(Theme.warning, "Retry")
            legendItem(Theme.purple, "Subagent")
        }
        .font(.system(size: Theme.textXs))
        .foregroundStyle(Theme.muted)
    }

    private func legendItem(_ color: Color, _ label: String) -> some View {
        HStack(spacing: 6) {
            RoundedRectangle(cornerRadius: 5).fill(color).frame(width: 10, height: 10)
            Text(label)
        }
    }

    // MARK: Filtering (MazeView.tsx nodeMatches / dimSet)

    private var trimmedQuery: String {
        debouncedQuery.trimmingCharacters(in: .whitespaces).lowercased()
    }

    private var filtering: Bool {
        failOnly || !trimmedQuery.isEmpty
    }

    private func nodeMatches(_ node: MazeNode) -> Bool {
        if failOnly, node.v != .error, node.v != .retry, node.v != .deadend {
            return false
        }
        let q = trimmedQuery
        if q.isEmpty {
            return true
        }
        var hay = [node.label ?? "", node.rzTxt ?? "", node.why ?? ""]
        for tool in node.tools {
            hay += [tool.name, tool.args, tool.argsFull ?? "", tool.res, tool.resFull ?? ""]
        }
        return hay.joined(separator: "\n").lowercased().contains(q)
    }

    /// Steps to dim while filtering ("lane:step"); nil when no filter is
    /// active. Mirrors MazeView.tsx's precomputed dimSet.
    private func dimSet(_ data: MazeData) -> Set<String>? {
        guard filtering else { return nil }
        var dim = Set<String>()
        for lane in data.lanes {
            for node in lane.main + lane.detours where !nodeMatches(node) {
                dim.insert("\(lane.key):\(node.step)")
            }
        }
        return dim
    }

    private func hitCount(_ data: MazeData) -> Int {
        guard filtering else { return -1 }
        var hits = 0
        for lane in data.lanes {
            for node in lane.main + lane.detours where nodeMatches(node) {
                hits += 1
            }
        }
        return hits
    }

    // MARK: Canvas

    private func mazeBody(_ data: MazeData) -> some View {
        GeometryReader { geo in
            let scene = MazeScene(data: data, width: max(geo.size.width, 80), window: window)
            let frames = hitFrames(scene)
            let dimmed = dimSet(data)
            let hasLive = data.lanes.contains { ($0.main + $0.detours).contains { $0.live == true } }
            let overflow = max(0, scene.totalH - geo.size.height)
            let offsetY = min(max(scrollY, 0), overflow)
            ZStack(alignment: .topLeading) {
                // Vertical scrolling is state-driven (scrollY + offset)
                // rather than a ScrollView: one WheelCapture layer then
                // owns EVERY scroll event and picks the desktop-natural
                // meaning — vertical scrolls when the map overflows,
                // ⌘+wheel always zooms, pinch zooms, dominant-
                // horizontal pans — with no NSScrollView fighting back.
                ZStack(alignment: .topLeading) {
                    canvasLayer(data: data, scene: scene, dimmed: dimmed, hasLive: hasLive)
                    if let brush {
                        brushView(brush, scene: scene)
                    }
                    if let hover {
                        hoverCardView(hover, scene: scene)
                    }
                }
                .frame(width: scene.width, height: max(scene.totalH, geo.size.height),
                       alignment: .topLeading)
                .contentShape(Rectangle())
                .gesture(dragGesture(scene: scene))
                .gesture(magnifyGesture(scene: scene))
                .onTapGesture(count: 2) { window = nil }
                .onTapGesture(count: 1) { point in handleTap(point, frames: frames) }
                .onContinuousHover(coordinateSpace: .local) { phase in
                    handleHover(phase, frames: frames)
                }
                .offset(y: -offsetY)

                WheelCaptureView { event, point in
                    handleWheel(event, at: point, scene: scene, viewportH: geo.size.height)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                if overflow > 0 {
                    scrollIndicator(totalH: scene.totalH, viewportH: geo.size.height,
                                    offsetY: offsetY)
                }

                if let selected {
                    detailPanel(selected)
                        .frame(maxHeight: .infinity)
                        .transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
            .clipped()
            .animation(.easeInOut(duration: 0.15), value: selected?.step)
            .background {
                // ⌘0 reset zoom / ⌘F focus search (browser conventions).
                Button("Reset zoom") { window = nil }
                    .keyboardShortcut("0", modifiers: .command)
                    .opacity(0)
                    .frame(width: 0, height: 0)
                Button("Search") { searchFocused = true }
                    .keyboardShortcut("f", modifiers: .command)
                    .opacity(0)
                    .frame(width: 0, height: 0)
            }
        }
    }

    /// Minimal overlay scrollbar (only while the map overflows).
    private func scrollIndicator(totalH: CGFloat, viewportH: CGFloat, offsetY: CGFloat) -> some View {
        let trackH = viewportH - 8
        let thumbH = max(24, trackH * viewportH / totalH)
        let travel = max(trackH - thumbH, 0)
        let fraction = totalH - viewportH > 0 ? offsetY / (totalH - viewportH) : 0
        return RoundedRectangle(cornerRadius: 3)
            .fill(Theme.bg3)
            .frame(width: 6, height: thumbH)
            .offset(y: 4 + travel * fraction)
            .padding(.trailing, 2)
            .frame(maxWidth: .infinity, alignment: .trailing)
            .allowsHitTesting(false)
    }

    @ViewBuilder
    private func canvasLayer(data: MazeData, scene: MazeScene, dimmed: Set<String>?,
                             hasLive: Bool) -> some View
    {
        if hasLive {
            TimelineView(.periodic(from: .now, by: 1.0 / 15)) { context in
                // Between refetches the live capsules keep growing with
                // the wall clock (the axis grows to match) instead of
                // freezing until the next server projection.
                let liveScene = MazeScene(
                    data: data, width: scene.width, window: window,
                    liveElapsed: context.date.timeIntervalSince(fetchedAt),
                )
                mazeCanvas(scene: liveScene, dimmed: dimmed,
                           pulse: pulseValue(at: context.date), fadeNow: context.date)
            }
        } else {
            mazeCanvas(scene: scene, dimmed: dimmed, pulse: 1, fadeNow: nil)
        }
    }

    /// ~150ms opacity ramp for a freshly materialized node (web mounts
    /// nodes with a CSS transition). Only applied while the pulse
    /// ticker runs; settled maps render fully opaque.
    private func fade(lane: String, step: Int, now: Date?) -> Double {
        guard let now, let t0 = firstSeen["\(lane):\(step)"] else { return 1 }
        return min(1, max(0.05, now.timeIntervalSince(t0) / 0.15))
    }

    /// .maze-node.is-live pulse (1.6s ease-in-out, opacity 0.4…1.0).
    private func pulseValue(at date: Date) -> Double {
        let phase = date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1.6) / 1.6
        return 0.4 + 0.6 * (0.5 + 0.5 * sin(phase * 2 * .pi - .pi / 2))
    }

    private func mazeCanvas(scene: MazeScene, dimmed: Set<String>?, pulse: Double,
                            fadeNow: Date?) -> some View
    {
        Canvas { ctx, _ in
            drawGapSeams(ctx: ctx, scene: scene)
            drawTicks(ctx: ctx, scene: scene)
            for layout in scene.lanes {
                drawLaneHeader(ctx: ctx, layout: layout, scene: scene)
                drawMainline(ctx: ctx, layout: layout, scene: scene)
                for node in layout.lane.main {
                    drawMainNode(
                        ctx: ctx, node: node, layout: layout, scene: scene,
                        dim: dimmed?.contains("\(layout.lane.key):\(node.step)") ?? false,
                        pulse: pulse, fadeNow: fadeNow,
                    )
                }
                for (index, detour) in layout.lane.detours.enumerated() {
                    drawDetourNode(
                        ctx: ctx, node: detour, row: layout.detourRows[index],
                        layout: layout, scene: scene,
                        dim: dimmed?.contains("\(layout.lane.key):\(detour.step)") ?? false,
                        pulse: pulse, fadeNow: fadeNow,
                    )
                }
            }
        }
    }

    private func hitFrames(_ scene: MazeScene) -> [MazeHitFrame] {
        var frames: [MazeHitFrame] = []
        for layout in scene.lanes {
            for node in layout.lane.main {
                let (x, w) = scene.barProps(node)
                frames.append(MazeHitFrame(
                    node: node,
                    rect: CGRect(x: x, y: layout.mainY - 9, width: w, height: 18),
                ))
            }
            for (index, detour) in layout.lane.detours.enumerated() {
                let (x, w) = scene.barProps(detour)
                let y = layout.detourTop + CGFloat(layout.detourRows[index]) * MazeMetrics.detourH
                    + MazeMetrics.detourH / 2
                frames.append(MazeHitFrame(
                    node: detour,
                    rect: CGRect(x: x, y: y - 8, width: w, height: 16),
                ))
            }
        }
        return frames
    }

    // MARK: Canvas drawing — seams and ticks

    private func drawGapSeams(ctx: GraphicsContext, scene: MazeScene) {
        // One label per visible seam, x-clamped into the canvas and
        // thinned against each other (MazeView.tsx gapMarks).
        let minSpacing: CGFloat = 12
        var lastRight = -CGFloat.infinity
        for gap in scene.axis.gaps {
            let x1 = scene.toX(gap.dStart)
            let x2 = scene.toX(gap.dEnd)
            if x2 < 0 || x1 > scene.width {
                continue
            }
            var seam = ctx
            seam.opacity = 0.6
            seam.fill(
                Path(CGRect(x: x1, y: 0, width: max(x2 - x1, 2), height: scene.totalH - MazeMetrics.axisH)),
                with: .color(Theme.bg1),
            )
            let label = "⏸ \(mazeFormatDur(gap.skipped)) skipped"
            let w = CGFloat(mazeEstTextWidth(label))
            let lo = MazeMetrics.padX + w / 2
            let hi = scene.width - MazeMetrics.padX - w / 2
            let cx = lo <= hi ? min(max((x1 + x2) / 2, lo), hi) : scene.width / 2
            guard cx - w / 2 >= lastRight + minSpacing else { continue }
            lastRight = cx + w / 2
            ctx.draw(
                Text(label).font(.system(size: 10)).foregroundStyle(Theme.muted),
                at: CGPoint(x: cx, y: scene.totalH - MazeMetrics.axisH - MazeMetrics.gapBandH + 8),
                anchor: .center,
            )
        }
    }

    private func drawTicks(ctx: GraphicsContext, scene: MazeScene) {
        let ticks = scene.axis.ticks(
            dStart: scene.dStart, dEnd: scene.dEnd,
            pxWidth: Double(scene.width - MazeMetrics.padX * 2),
        )
        for tick in ticks {
            let tx = scene.toX(tick.d)
            var line = Path()
            line.move(to: CGPoint(x: tx, y: scene.totalH - MazeMetrics.axisH))
            line.addLine(to: CGPoint(x: tx, y: scene.totalH - MazeMetrics.axisH + 6))
            ctx.stroke(line, with: .color(Theme.bg2), lineWidth: 1)
            // Labels are centered on their tick: clamp the text (not the
            // tick line) into the canvas so edge labels aren't clipped.
            let hw = CGFloat(mazeEstTextWidth(tick.label)) / 2
            let lo = MazeMetrics.padX + hw
            let hi = scene.width - MazeMetrics.padX - hw
            let cx = lo <= hi ? min(max(tx, lo), hi) : scene.width / 2
            ctx.draw(
                Text(tick.label).font(.system(size: 10)).foregroundStyle(Theme.muted),
                at: CGPoint(x: cx, y: scene.totalH - 12),
                anchor: .center,
            )
        }
    }

    // MARK: Canvas drawing — lanes

    private func drawLaneHeader(ctx: GraphicsContext, layout: MazeLaneLayout, scene: MazeScene) {
        let lane = layout.lane
        var title = lane.model ?? "session"
        if let laneTitle = lane.title, !laneTitle.isEmpty {
            title += " · \(laneTitle)"
        }
        ctx.draw(
            Text(title)
                .font(.system(size: Theme.textSm, weight: .semibold))
                .foregroundStyle(Theme.fg),
            at: CGPoint(x: MazeMetrics.padX, y: layout.top + 12),
            anchor: .leading,
        )
        let st = lane.stats
        var stats = "\(st.steps) steps · \(st.tools) tools · \(st.detours) branches · \(mazeFormatDur(st.t))"
        if st.outTok > 0 {
            stats += " · \(st.inTok + st.outTok) tok"
        }
        if let rzMs = st.rzMs, rzMs > 0 {
            stats += " · reasoning \(formatDuration(rzMs))"
        }
        ctx.draw(
            Text(stats).font(.system(size: Theme.textXs)).foregroundStyle(Theme.muted),
            at: CGPoint(x: scene.width - MazeMetrics.padX, y: layout.top + 12),
            anchor: .trailing,
        )
    }

    private func drawMainline(ctx: GraphicsContext, layout: MazeLaneLayout, scene: MazeScene) {
        var line = Path()
        line.move(to: CGPoint(x: MazeMetrics.padX, y: layout.mainY))
        line.addLine(to: CGPoint(x: scene.width - MazeMetrics.padX, y: layout.mainY))
        ctx.stroke(line, with: .color(Theme.bg2), lineWidth: 2)
    }

    private func drawMainNode(
        ctx: GraphicsContext, node: MazeNode, layout: MazeLaneLayout,
        scene: MazeScene, dim: Bool, pulse: Double, fadeNow: Date?,
    ) {
        let (x, w) = scene.barProps(node)
        if x > scene.width + MazeMetrics.cullMargin || x + w < -MazeMetrics.cullMargin {
            return
        }
        let y = layout.mainY
        var c = ctx
        c.opacity = node.v.fillOpacity * (dim ? 0.15 : 1) * (node.live == true ? pulse : 1)
            * fade(lane: layout.lane.key, step: node.step, now: fadeNow)
        let rect = CGRect(x: x, y: y - 9, width: w, height: 18)
        c.fill(Path(roundedRect: rect, cornerRadius: 9), with: .color(node.v.color))
        if selected?.step == node.step {
            c.stroke(Path(roundedRect: rect, cornerRadius: 9), with: .color(Theme.fg), lineWidth: 1.5)
        }
        // Fit the longest label first, fall back to the step id, then
        // nothing (MazeView.tsx MainNode).
        let full = "S\(node.step)·\(node.turn) \(mazeFormatDur(node.e - node.s))"
        let short = "S\(node.step)"
        let label = w >= CGFloat(full.count) * 6 + 8 ? full : (w >= CGFloat(short.count) * 6 + 8 ? short : "")
        if !label.isEmpty {
            let color = (node.v == .deadend || node.v == .retry) ? Theme.bg0 : Theme.onAccent
            c.draw(
                Text(label).font(.system(size: 10)).foregroundStyle(color),
                at: CGPoint(x: x + w / 2, y: y),
                anchor: .center,
            )
        }
        // Parallel tool sub-bars: with ≥2 calls, each sits at its real span.
        if node.tools.count > 1 {
            for (i, tool) in node.tools.enumerated() {
                let tx1 = scene.toX(scene.axis.map(tool.s))
                let tx2 = scene.toX(scene.axis.map(tool.e ?? scene.effectiveEnd(node)))
                let bar = CGRect(
                    x: tx1, y: y + 12 + CGFloat(i) * MazeMetrics.parBarH,
                    width: max(tx2 - tx1, 3), height: 5,
                )
                c.fill(Path(roundedRect: bar, cornerRadius: 2.5), with: .color(tool.v.color))
            }
        }
        // Base tick marking that this lane carries parallel sub-bars.
        if layout.maxPar > 0 {
            c.fill(Path(CGRect(x: x, y: y + 10, width: w, height: 2)), with: .color(Theme.bg2))
        }
    }

    private func drawDetourNode(
        ctx: GraphicsContext, node: MazeNode, row: Int, layout: MazeLaneLayout,
        scene: MazeScene, dim: Bool, pulse: Double, fadeNow: Date?,
    ) {
        let (x, w) = scene.barProps(node)
        if x > scene.width + MazeMetrics.cullMargin || x + w < -MazeMetrics.cullMargin {
            return
        }
        let y = layout.detourTop + CGFloat(row) * MazeMetrics.detourH + MazeMetrics.detourH / 2
        // Dashed arc: from the attach step's bottom edge curving down to
        // the detour's left edge; a return path bends back once it ends.
        let attachNode = node.attach.flatMap { layout.byStep[$0] }
        let ax = attachNode.map { scene.toX(scene.axis.map(scene.effectiveEnd($0))) }
            ?? MazeMetrics.padX
        let ay = layout.mainY + 10
        let baseOpacity = node.v.fillOpacity * (dim ? 0.15 : 1) * (node.live == true ? pulse : 1)
            * fade(lane: layout.lane.key, step: node.step, now: fadeNow)

        var arc = Path()
        arc.move(to: CGPoint(x: ax, y: ay))
        arc.addCurve(
            to: CGPoint(x: x, y: y),
            control1: CGPoint(x: ax, y: y - 14),
            control2: CGPoint(x: x - 8, y: y - 14),
        )
        var arcCtx = ctx
        arcCtx.opacity = baseOpacity * 0.8
        arcCtx.stroke(arc, with: .color(Theme.muted), style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
        if node.live != true, node.v != .ok {
            var ret = Path()
            ret.move(to: CGPoint(x: x + w, y: y))
            ret.addCurve(
                to: CGPoint(x: ax, y: ay + 2),
                control1: CGPoint(x: x + w + 6, y: y + 12),
                control2: CGPoint(x: ax + 6, y: y + 12),
            )
            var retCtx = ctx
            retCtx.opacity = baseOpacity * 0.4
            retCtx.stroke(ret, with: .color(Theme.muted), style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
        }

        let isSub = node.sub == true
        var c = ctx
        c.opacity = baseOpacity
        let rect = CGRect(x: x, y: y - 8, width: w, height: 16)
        c.fill(Path(roundedRect: rect, cornerRadius: 8),
               with: .color(isSub ? Theme.purple : node.v.color))
        if selected?.step == node.step {
            c.stroke(Path(roundedRect: rect, cornerRadius: 8), with: .color(Theme.fg), lineWidth: 1.5)
        }
        let glyph = node.v == .error ? "✗" : node.v == .deadend ? "·" : node.v == .retry ? "↻" : ""
        let labelText = isSub ? "⤴ \(node.label ?? "")" : "S\(node.step) \(glyph)"
        let estW = CGFloat(mazeEstTextWidth(labelText)) + 8 // +8 capsule padding
        if isSub {
            drawSubLabel(ctx: c, x: x, w: w, y: y, text: labelText, estW: estW, scene: scene)
        } else if w >= estW {
            let color = (node.v == .deadend || node.v == .retry) ? Theme.bg0 : Theme.onAccent
            c.draw(
                Text(labelText).font(.system(size: 10)).foregroundStyle(color),
                at: CGPoint(x: x + w / 2, y: y),
                anchor: .center,
            )
        }
        // Sub-agent node sub-bars: all of the child's judged tool calls.
        if isSub {
            for (i, tool) in node.tools.prefix(24).enumerated() {
                let tx1 = scene.toX(scene.axis.map(tool.s))
                let tx2 = scene.toX(scene.axis.map(tool.e ?? scene.effectiveEnd(node)))
                let bar = CGRect(
                    x: tx1, y: y + 10 + CGFloat(i % 3) * 6,
                    width: max(tx2 - tx1, 3), height: 4,
                )
                c.fill(Path(roundedRect: bar, cornerRadius: 2), with: .color(tool.v.color))
            }
        }
    }

    /// Sub-agent titles annotate beside the bar: right side preferred,
    /// flipping left near the canvas edge, ellipsized when neither side
    /// fits (MazeView.tsx SubLabel).
    private func drawSubLabel(
        ctx: GraphicsContext, x: CGFloat, w: CGFloat, y: CGFloat,
        text: String, estW: CGFloat, scene: MazeScene,
    ) {
        let spaceRight = scene.width - MazeMetrics.padX - (x + w + 6)
        let spaceLeft = x - 6 - MazeMetrics.padX
        let style = Text(text).font(.system(size: 10)).foregroundStyle(Theme.fg)
        if estW <= spaceRight {
            ctx.draw(style, at: CGPoint(x: x + w + 6, y: y), anchor: .leading)
        } else if estW <= spaceLeft {
            ctx.draw(style, at: CGPoint(x: x - 6, y: y), anchor: .trailing)
        } else if spaceRight >= spaceLeft {
            ctx.draw(
                Text(mazeFitLabel(text, budget: Double(max(spaceRight, 0))))
                    .font(.system(size: 10)).foregroundStyle(Theme.fg),
                at: CGPoint(x: x + w + 6, y: y),
                anchor: .leading,
            )
        } else {
            ctx.draw(
                Text(mazeFitLabel(text, budget: Double(max(spaceLeft, 0))))
                    .font(.system(size: 10)).foregroundStyle(Theme.fg),
                at: CGPoint(x: x - 6, y: y),
                anchor: .trailing,
            )
        }
    }

    // MARK: Gestures — brush zoom / Shift-drag pan / wheel / pinch

    private func dragGesture(scene: MazeScene) -> some Gesture {
        DragGesture(minimumDistance: 3, coordinateSpace: .local)
            .onChanged { value in
                if dragState == nil {
                    let panning = NSEvent.modifierFlags.contains(.shift)
                    dragState = DragState(
                        mode: panning ? .pan : .brush,
                        startX: value.startLocation.x,
                        window: scene.dStart ... scene.dEnd,
                    )
                    if !panning {
                        hover = nil
                    }
                }
                guard let dragState else { return }
                switch dragState.mode {
                case .brush:
                    brush = (x0: dragState.startX, x1: value.location.x)
                case .pan:
                    // Content follows the pointer (web: ns = win0 - dd).
                    let span = dragState.window.upperBound - dragState.window.lowerBound
                    let w = max(scene.width - MazeMetrics.padX * 2, 1)
                    let dd = Double(value.location.x - dragState.startX) / Double(w) * span
                    panWindow(start: dragState.window, delta: dd, total: scene.axis.total)
                }
            }
            .onEnded { value in
                let drag = dragState
                dragState = nil
                brush = nil
                guard let drag, drag.mode == .brush else { return }
                let lo = min(drag.startX, value.location.x)
                let hi = max(drag.startX, value.location.x)
                guard hi - lo >= 4 else { return } // micro-drag: keep the window
                let dLo = scene.domain(atX: lo)
                let dHi = scene.domain(atX: hi)
                guard dHi - dLo >= 0.5 else { return }
                window = dLo ... dHi
            }
    }

    private func magnifyGesture(scene: MazeScene) -> some Gesture {
        MagnifyGesture()
            .onChanged { value in
                let delta = value.magnification / pinchLast
                pinchLast = value.magnification
                guard delta != 1 else { return }
                zoomWindow(scene: scene, scale: 1 / delta, centerX: scene.width / 2)
            }
            .onEnded { _ in pinchLast = 1 }
    }

    private func handleWheel(_ event: NSEvent, at point: CGPoint, scene: MazeScene,
                             viewportH: CGFloat)
    {
        let precise = event.hasPreciseScrollingDeltas
        let rawX = precise ? event.scrollingDeltaX : event.deltaX * 16
        let rawY = precise ? event.scrollingDeltaY : event.deltaY * 16
        // Browser-sign normalization: AppKit's natural-scrolling deltas
        // already match the browser when inverted; plain mice are opposite.
        let dx = event.isDirectionInvertedFromDevice ? rawX : -rawX
        let dy = event.isDirectionInvertedFromDevice ? rawY : -rawY
        // ⌘+wheel always zooms — the browser/editor convention.
        if event.modifierFlags.contains(.command) {
            guard dy != 0 else { return }
            zoomWindow(scene: scene, scale: pow(1.01, Double(dy)), centerX: point.x)
            return
        }
        if abs(dx) > abs(dy) {
            // Pan: positive deltaX (swipe left) advances the window forward.
            let span = scene.dEnd - scene.dStart
            let w = max(scene.width - MazeMetrics.padX * 2, 1)
            panWindow(start: scene.dStart ... scene.dEnd, delta: -Double(dx / w) * span,
                      total: scene.axis.total)
            return
        }
        guard dy != 0 else { return }
        let overflow = scene.totalH - viewportH
        if overflow > 0 {
            // Dense maps overflow: vertical wheel scrolls (dy > 0 = down).
            scrollY = min(max(scrollY + dy, 0), overflow)
        } else {
            // The map fits: vertical wheel zooms around the cursor
            // (dy > 0 zooms out — web parity).
            zoomWindow(scene: scene, scale: pow(1.01, Double(dy)), centerX: point.x)
        }
    }

    private func panWindow(start: ClosedRange<Double>, delta: Double, total: Double) {
        var ns = start.lowerBound - delta
        var ne = start.upperBound - delta
        if ns < 0 {
            ne -= ns; ns = 0
        }
        if ne > total {
            ns -= ne - total; ne = total
        }
        ns = max(0, ns)
        ne = min(total, ne)
        if ns <= 0, ne >= total {
            window = nil
        } else {
            window = ns ... ne
        }
    }

    private func zoomWindow(scene: MazeScene, scale: Double, centerX: CGFloat) {
        let total = max(scene.axis.total, 1)
        let w = max(scene.width - MazeMetrics.padX * 2, 1)
        let frac = Double(min(max((centerX - MazeMetrics.padX) / w, 0), 1))
        let span = scene.dEnd - scene.dStart
        let center = scene.dStart + frac * span
        var ns = center - (center - scene.dStart) * scale
        var ne = center + (scene.dEnd - center) * scale
        if ne - ns < 0.5 {
            return
        } // finest zoom: 0.5 display seconds
        if ne - ns >= total {
            window = nil
            return
        }
        ns = max(0, ns)
        ne = min(total, ne)
        window = ns ... ne
    }

    private func handleTap(_ point: CGPoint, frames: [MazeHitFrame]) {
        // Later frames win: detours are appended after main nodes.
        if let hit = frames.last(where: { $0.rect.insetBy(dx: -3, dy: -3).contains(point) }) {
            selected = hit.node
        } else {
            selected = nil
        }
    }

    private func handleHover(_ phase: HoverPhase, frames: [MazeHitFrame]) {
        switch phase {
        case let .active(point):
            guard brush == nil else { return } // no tooltip stuck under the selection
            if let hit = frames.last(where: { $0.rect.insetBy(dx: -3, dy: -3).contains(point) }) {
                hover = (point: point, card: hoverCard(for: hit.node))
            } else {
                hover = nil
            }
        case .ended:
            hover = nil
        }
    }

    // MARK: Hover card (.maze-tip)

    private func hoverCard(for node: MazeNode) -> HoverCard {
        var lines: [String] = []
        if node.sub == true {
            lines.append("Subagent branch · \(node.tools.count) tool calls · \(mazeFormatDur(node.e - node.s))")
        } else {
            var took = "Took \(mazeFormatDur(node.e - node.s))"
            if let retries = node.retries, retries > 0 {
                took += " · retry waits ×\(retries)"
            }
            lines.append(took)
        }
        let tok = tokLabel(node)
        if !tok.isEmpty {
            lines.append(tok)
        }
        if node.sub != true {
            lines.append(node.tools.isEmpty ? "No tool calls (answer)" : "\(node.tools.count) tool calls")
        }
        if let why = node.why, !why.isEmpty {
            lines.append(why)
        }
        return HoverCard(title: "\(nodeTitle(node)) · \(node.v.title)", lines: lines)
    }

    private func hoverCardView(_ hover: (point: CGPoint, card: HoverCard), scene: MazeScene) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(hover.card.title)
                .font(.system(size: Theme.textXs, weight: .semibold))
                .foregroundStyle(Theme.fg)
            ForEach(hover.card.lines, id: \.self) { line in
                Text(line)
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .frame(maxWidth: 300, alignment: .leading)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Theme.bg2, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusMd).strokeBorder(Theme.bg0, lineWidth: 1))
        .shadow(color: .black.opacity(0.35), radius: 8, y: 2)
        .fixedSize()
        .offset(x: min(hover.point.x + 10, max(scene.width - 240, 0)), y: hover.point.y + 12)
        .allowsHitTesting(false)
    }

    // MARK: Brush (.maze-brush)

    private func brushView(_ brush: (x0: CGFloat, x1: CGFloat), scene: MazeScene) -> some View {
        let lo = min(brush.x0, brush.x1)
        let hi = max(brush.x0, brush.x1)
        let label = mazeFormatDur(scene.domain(atX: hi) - scene.domain(atX: lo))
        return ZStack(alignment: .top) {
            Rectangle().fill(Theme.highlight.opacity(0.16))
            Rectangle().fill(Theme.highlight).frame(width: 1)
                .frame(maxWidth: .infinity, alignment: .leading)
            Rectangle().fill(Theme.highlight).frame(width: 1)
                .frame(maxWidth: .infinity, alignment: .trailing)
            Text(label)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.highlight)
                .padding(.horizontal, 6)
                .padding(.vertical, 1)
                .background(Theme.bg2, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                .overlay(RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(Theme.bg0, lineWidth: 1))
                .padding(.top, 4)
        }
        .frame(width: max(hi - lo, 1))
        .offset(x: lo)
        .allowsHitTesting(false)
    }

    // MARK: Detail panel (.maze-detail)

    private func detailPanel(_ node: MazeNode) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    MazeVerdictBadge(verdict: node.v)
                    Text(nodeTitle(node))
                        .font(.system(size: Theme.textMd, weight: .semibold))
                        .foregroundStyle(Theme.fg)
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    GhostButton { selected = nil } label: { Image(systemName: "xmark") }
                        .accessibilityLabel("Close step details")
                }
                detailMeta(node)
                if let why = node.why, !why.isEmpty {
                    MazeWhyBlock(text: why)
                }
                if node.sub != true, (node.msgSeq ?? 0) > 0 || !node.tools.isEmpty {
                    MazeButton(
                        title: "Locate this step in the trace",
                        systemImage: "arrow.turn.down.right",
                        help: "Switch to the trace tab and scroll to this step",
                    ) { locateInTrace(node.turn) }
                }
                if let rzTxt = node.rzTxt, !rzTxt.isEmpty {
                    reasoningBlock(node, text: rzTxt)
                }
                if node.tools.isEmpty {
                    Text("No tool calls")
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                }
                ForEach(node.tools) { tool in
                    MazeToolCard(tool: tool)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(width: MazeMetrics.detailWidth)
        .background(Theme.bg1)
        .overlay(alignment: .leading) { Hairline(axis: .vertical) }
        .shadow(color: .black.opacity(0.3), radius: 12, x: -2)
        // Esc closes the panel first (MazeView.tsx capture-phase handler).
        .background(
            Button("") { selected = nil }
                .keyboardShortcut(.escape, modifiers: [])
                .opacity(0)
                .frame(width: 0, height: 0),
        )
    }

    private func detailMeta(_ node: MazeNode) -> some View {
        var parts = ["Took \(mazeFormatDur(node.e - node.s))"]
        if let retries = node.retries, retries > 0 {
            parts.append("Model retries ×\(retries)")
        }
        let tok = tokLabel(node)
        if !tok.isEmpty {
            parts.append(tok)
        }
        if let inTok = node.inTok {
            parts.append("\(inTok) tok in")
        }
        return Text(parts.joined(separator: "  ·  "))
            .font(.system(size: Theme.textXs))
            .foregroundStyle(Theme.muted)
    }

    private func reasoningBlock(_ node: MazeNode, text: String) -> some View {
        var summary = "Reasoning summary (\(node.rz) segments"
        if let rzMs = node.rzMs, rzMs > 0 {
            summary += " · \(formatDuration(rzMs))"
        }
        summary += ")"
        return DisclosureGroup {
            ScrollView {
                Text(text)
                    .font(Theme.monoXs)
                    .foregroundStyle(Theme.muted)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 180)
        } label: {
            Text(summary)
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.primary)
        }
        .font(.system(size: Theme.textXs))
        .foregroundStyle(Theme.muted)
    }

    // MARK: Shared labels

    private func nodeTitle(_ node: MazeNode) -> String {
        if node.sub == true {
            let label = node.label ?? ""
            return "⤴ \(label.isEmpty ? "Subagent" : label)"
        }
        return "S\(node.step) · Turn \(node.turn)"
    }

    private func tokLabel(_ node: MazeNode) -> String {
        var parts: [String] = []
        if let rzMs = node.rzMs, rzMs > 0 {
            parts.append("Reasoning \(formatDuration(rzMs))")
        }
        if let rzTok = node.rzTok {
            parts.append("\(rzTok) tok")
        } else if node.rz > 0 {
            parts.append("\(node.rz) reasoning segments")
        }
        if let outTok = node.outTok {
            parts.append("\(outTok) tok out")
        }
        return parts.joined(separator: " · ")
    }
}

// MARK: - Wheel capture (non-passive wheel listener equivalent)

/// Transparent overlay that claims ONLY scroll-wheel events — every other
/// event falls through to the SwiftUI content beneath (clicks, drags,
/// hover). The WebUI registers a native non-passive wheel listener for
/// the same reason: SwiftUI's ScrollView would otherwise scroll
/// vertically while zooming.
private struct WheelCaptureView: NSViewRepresentable {
    let onScroll: (NSEvent, CGPoint) -> Void

    func makeNSView(context _: Context) -> WheelCaptureNSView {
        let view = WheelCaptureNSView()
        view.onScroll = onScroll
        return view
    }

    func updateNSView(_ nsView: WheelCaptureNSView, context _: Context) {
        nsView.onScroll = onScroll
    }
}

private final class WheelCaptureNSView: NSView {
    var onScroll: ((NSEvent, CGPoint) -> Void)?

    override func hitTest(_: NSPoint) -> NSView? {
        NSApp.currentEvent?.type == .scrollWheel ? self : nil
    }

    override func scrollWheel(with event: NSEvent) {
        onScroll?(event, convert(event.locationInWindow, from: nil))
    }
}

// MARK: - Small components (maze.css)

/// .maze-badge: verdict pill (sm variant for tool cards).
private struct MazeVerdictBadge: View {
    let verdict: MazeVerdict
    var small = false

    var body: some View {
        Text(verdict.title)
            .font(.system(size: Theme.textXs))
            .foregroundStyle(verdict.badgeForeground)
            .padding(.horizontal, small ? 6 : 8)
            .padding(.vertical, small ? 0 : 1)
            .background(verdict.badgeBackground, in: Capsule())
    }
}

/// .maze-detail-why: warning text on bg0.
private struct MazeWhyBlock: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.system(size: Theme.textXs))
            .foregroundStyle(Theme.warning)
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
            .textSelection(.enabled)
    }
}

/// .maze-tool: a judged tool call — 3px verdict-colored left border,
/// mono args/result blocks with copy buttons.
private struct MazeToolCard: View {
    let tool: MazeTool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Text(tool.name)
                    .font(Theme.monoSm)
                    .foregroundStyle(Theme.fg)
                Spacer(minLength: 4)
                Text(tool.e == nil ? "Running…" : mazeFormatDur(tool.dur))
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                MazeVerdictBadge(verdict: tool.v, small: true)
            }
            if let args = tool.argsFull, !args.isEmpty {
                MazeToolBlock(title: "Args", text: args)
            }
            let result = tool.resFull ?? tool.res
            if !result.isEmpty {
                MazeToolBlock(title: "Result", text: result)
            }
            if let why = tool.why, !why.isEmpty {
                MazeWhyBlock(text: why)
            }
            if let child = tool.childId, !child.isEmpty {
                Text("⤴ Child session \(child)")
                    .font(Theme.monoXs)
                    .foregroundStyle(Theme.purple)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        .overlay(alignment: .leading) {
            Rectangle().fill(tool.v.color).frame(width: 3)
        }
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusSm)
            .strokeBorder(Theme.bg2, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: Theme.radiusSm))
    }
}

/// .maze-tool-block: header (label + copy) over a mono pre block.
private struct MazeToolBlock: View {
    let title: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(title)
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                Spacer()
                GhostButton(size: 11, action: copy) { Image(systemName: "doc.on.doc") }
                    .frame(width: 22, height: 22)
                    .help("Copy \(title.lowercased())")
                    .accessibilityLabel("Copy \(title.lowercased())")
            }
            ScrollView {
                Text(text)
                    .font(Theme.monoXs)
                    .foregroundStyle(Theme.fg)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 200)
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
        }
    }

    private func copy() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }
}
