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

/// Session detail: a 44px toolbar in the mac idiom (chrome controls
/// left, centered document title, status + actions right), the
/// transcript with edge scroll-fades, pending cards, the composer,
/// and a 28px statusbar (bg1, hairline top border) closing the pane.
///
/// Each pane is its own View reading only the SessionStore properties
/// it needs: @Observable tracks access per property, so a streaming
/// draft frame re-evaluates ONLY the transcript — the header and
/// statusbar no longer rebuild at token rate (and token-counter ticks
/// no longer re-layout the transcript).
struct ChatView: View {
    let store: SessionStore
    /// Server-derived session title (first user message); the header
    /// shows it once the conversation has started, falling back to the
    /// short session id before that.
    var sessionTitle: String?
    var workspaceName: String?
    var version: String = ""
    /// Model catalog for the composer picker (SessionListStore.models).
    var models: [MetaModels.ModelInfo] = []
    /// Browsing the sidebar's archive listing: the session is read-only
    /// (WebUI archived badge).
    var archived: Bool = false
    @Binding var sidebarCollapsed: Bool

    var body: some View {
        VStack(spacing: 0) {
            ChatHeaderView(
                store: store,
                sessionTitle: sessionTitle,
                workspaceName: workspaceName,
                archived: archived,
                sidebarCollapsed: $sidebarCollapsed,
            )
            // The share toast draws outside the header's bounds —
            // lift the header above the transcript siblings so the
            // toast never slides under scrolling content.
            .zIndex(1)
            Hairline(axis: .horizontal)
            NoticeBannerView(store: store)
            TranscriptView(store: store)
            PendingAreaView(store: store)
            ComposerView(store: store, models: models)
            Hairline(axis: .horizontal)
            StatusBarView(store: store, version: version)
        }
        .background(Theme.bg0)
    }
}

// MARK: - Header (mac toolbar idiom)

/// The window's title bar, redesigned after the macOS toolbar idiom:
/// window-chrome controls on the left (sidebar, theme), a centered
/// document title (session title over a quiet `workspace · id`
/// subtitle), and status + actions on the right. Status is quiet by
/// default — a pill appears only when something needs attention (a
/// busy turn, a read-only session, a dropped connection); an idle,
/// live session shows none. Supersedes the WebUI's badge row, whose
/// always-on "idle"/"live" labels were pure noise.
private struct ChatHeaderView: View {
    let store: SessionStore
    var sessionTitle: String?
    var workspaceName: String?
    var archived: Bool
    @Binding var sidebarCollapsed: Bool

    /// WebUI loom_theme: "dark" (default) or "light"; LoomApp applies
    /// it as the window's preferredColorScheme.
    @AppStorage("loom.theme") private var theme = "dark"
    @State private var copiedSessionId = false
    @State private var copiedShareLink = false
    /// True while the share-link request is in flight (the share
    /// button spins and is disabled against double-minting).
    @State private var sharing = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        HStack(spacing: 0) {
            leadingCluster
            Spacer(minLength: 12)
            titleCluster
            Spacer(minLength: 12)
            trailingCluster
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 44)
        // Custom titlebar: empty header areas move the window (the
        // buttons keep their clicks — see windowDragSurface).
        .windowDragSurface()
    }

    // MARK: Leading (window chrome)

    private var leadingCluster: some View {
        HStack(spacing: 2) {
            GhostButton {
                sidebarCollapsed.toggle()
            } label: {
                // sidebar.left: the platform's sidebar-toggle glyph
                // (was line.3.horizontal — a web hamburger).
                Image(systemName: "sidebar.left")
            }
            .help("Toggle sidebar (⌃⌘S)")
            .accessibilityLabel("Toggle sidebar")

            GhostButton {
                theme = theme == "dark" ? "light" : "dark"
            } label: {
                Image(systemName: "circle.lefthalf.filled")
            }
            .help(theme == "dark" ? "Switch to light mode" : "Switch to dark mode")
            .accessibilityLabel("Toggle color theme")
        }
    }

    // MARK: Center (document title)

    /// macOS document-title idiom: the session title in semibold over
    /// a quiet `workspace · id` subtitle. Clicking the title copies
    /// the full session id (the WebUI's hdr-session); clicking the
    /// workspace name reveals it in the sidebar.
    private var titleCluster: some View {
        VStack(spacing: 1) {
            Button(action: copySessionId) {
                Text(displayTitle)
                    .font(.system(size: Theme.textMd, weight: .semibold))
                    .foregroundStyle(copiedSessionId ? Theme.success : (hasTitle ? Theme.fg : Theme.muted))
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            .buttonStyle(.plain)
            .help(copiedSessionId ? "Copied" : "\(store.sessionId) — click to copy the session ID")
            .accessibilityLabel("Session \(displayTitle)")

            HStack(spacing: 4) {
                if let workspaceName, !workspaceName.isEmpty {
                    Button {
                        sidebarCollapsed = false
                    } label: {
                        Text(workspaceName)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                    .buttonStyle(.plain)
                    .help("Locate the owning workspace")
                    Text("·")
                }
                Text(shortSessionId)
                    .font(Theme.monoXs)
            }
            .font(.system(size: Theme.textXs))
            .foregroundStyle(Theme.muted)
            .lineLimit(1)
        }
        .frame(maxWidth: .infinity)
    }

    // MARK: Trailing (status pills + actions)

    /// Status first, then the two actions. Pills render only for
    /// exceptional states: an idle turn and a live connection are the
    /// default and stay invisible.
    private var trailingCluster: some View {
        HStack(spacing: 6) {
            if store.readOnly {
                StatusPill(color: Theme.warning, text: "sub-agent · read-only")
                    .help(store.readOnlyTitle)
            } else if archived {
                StatusPill(color: Theme.warning, text: "archived · read-only")
            }
            statePill
            connectionPill

            GhostButton(action: shareSession) {
                // Minting the link is a network round-trip: spin
                // while it is in flight (the button is disabled, so
                // no double-mint), swap to a check on success — and
                // the toast below says WHAT happened, so the icon
                // swap never reads as "the button vanished".
                if sharing {
                    ProgressView()
                        .controlSize(.small)
                        .frame(width: 15, height: 15)
                } else {
                    Image(systemName: copiedShareLink ? "check" : "square.and.arrow.up")
                        .foregroundStyle(copiedShareLink ? Theme.success : Theme.muted)
                }
            }
            .disabled(sharing)
            .help(sharing
                ? "Creating share link…"
                : copiedShareLink
                ? "Share link copied — anyone with the link can view this session read-only"
                : "Share session: copy a public read-only link (Shift+click to unshare)")
            .accessibilityLabel("Share session")
            .overlay(alignment: .bottomTrailing) {
                if copiedShareLink {
                    ShareCopiedToast()
                        .offset(y: 34)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
            .animation(reduceMotion ? nil : .easeInOut(duration: 0.15), value: copiedShareLink)

            GhostButton {
                Task { await store.requestCompaction() }
            } label: {
                Image(systemName: "rectangle.compress.vertical")
            }
            .help("Compact context on next turn")
            .accessibilityLabel("Compact context")
            .disabled(store.isBusy)
        }
    }

    private var shortSessionId: String {
        store.sessionId.count > 8 ? String(store.sessionId.prefix(8)) : store.sessionId
    }

    private var hasTitle: Bool {
        sessionTitle?.isEmpty == false
    }

    /// The derived title once the conversation has started; a quiet
    /// placeholder before that (the full id sits in the subtitle).
    private var displayTitle: String {
        hasTitle ? sessionTitle! : "New Session"
    }

    private func copySessionId() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(store.sessionId, forType: .string)
        copiedSessionId = true
        Task {
            try? await Task.sleep(for: .seconds(1.5))
            copiedSessionId = false
        }
    }

    private func shareSession() {
        if NSEvent.modifierFlags.contains(.shift) {
            confirmUnshare()
            return
        }
        guard !sharing else { return }
        sharing = true
        Task {
            let link = await store.shareLink()
            sharing = false
            // Failure: shareLink() already set store.lastError, which
            // the notice banner surfaces — no local flash needed.
            guard let link else { return }
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(link, forType: .string)
            copiedShareLink = true
            try? await Task.sleep(for: .seconds(2.5))
            copiedShareLink = false
        }
    }

    /// The WebUI's unshare confirm dialog (shareSession shift-click).
    private func confirmUnshare() {
        let alert = NSAlert()
        alert.messageText = "Unshare"
        alert.informativeText = "The shared link stops working immediately (sharing again creates a new link)."
        alert.addButton(withTitle: "Unshare")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            Task { await store.revokeShare() }
        }
    }

    /// Turn state, exceptional cases only (idle is the default and
    /// stays quiet). Pulses while the turn is in flight.
    @ViewBuilder private var statePill: some View {
        switch store.state {
        case .running:
            StatusPill(color: Theme.success, text: "running", pulses: true)
        case .awaitingApproval:
            StatusPill(color: Theme.warning, text: "awaiting approval", pulses: true)
        case .cancelling:
            StatusPill(color: Theme.warning, text: "cancelling", pulses: true)
        case .booting:
            StatusPill(color: Theme.muted, text: "booting")
        case .closed:
            StatusPill(color: Theme.muted, text: "closed")
        case .fatal:
            StatusPill(color: Theme.error, text: "fatal")
        default:
            EmptyView()
        }
    }

    /// Connection state, exceptional cases only (live is the default
    /// and stays quiet — the WebUI's always-on "live" badge was
    /// noise).
    @ViewBuilder private var connectionPill: some View {
        switch store.connection {
        case .live:
            EmptyView()
        case .connecting:
            StatusPill(color: Theme.warning, text: "connecting…", pulses: true)
        case let .offline(attempt):
            StatusPill(color: Theme.warning, text: "reconnecting (\(attempt))", pulses: true)
        case .drained:
            StatusPill(color: Theme.error, text: "server shut down")
        }
    }
}

/// Transient confirmation floating under the share button after the
/// link lands on the clipboard: explicit text feedback so the 2.5s
/// check-icon dwell never reads as "the button disappeared".
private struct ShareCopiedToast: View {
    var body: some View {
        HStack(spacing: 5) {
            Image(systemName: "checkmark")
                .font(.system(size: 9, weight: .bold))
            Text("Link copied")
        }
        .font(.system(size: Theme.textXs, weight: .medium))
        .foregroundStyle(Theme.success)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(Theme.bg1, in: Capsule())
        .overlay(Capsule().strokeBorder(Theme.bg2, lineWidth: 1))
        .shadow(color: .black.opacity(0.3), radius: 6, y: 2)
        .fixedSize()
    }
}

/// The header's status capsule: a small dot + 11pt label on a 12%
/// tint of the same hue. Replaces the WebUI's bare dot-and-text
/// badges — the tint gives a state a stable, scannable shape, and
/// because pills only appear for exceptional states, one showing up
/// actually means something.
private struct StatusPill: View {
    let color: Color
    let text: String
    var pulses = false

    var body: some View {
        HStack(spacing: 5) {
            if pulses {
                PulsingDot(color: color, size: 6)
            } else {
                Circle().fill(color).frame(width: 6, height: 6)
            }
            Text(text)
        }
        .font(.system(size: Theme.textXs, weight: .medium))
        .foregroundStyle(color)
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(color.opacity(0.12), in: Capsule())
        .fixedSize()
    }
}

// MARK: - Notices (.banner)

private struct NoticeBannerView: View {
    let store: SessionStore

    var body: some View {
        VStack(spacing: 0) {
            if let error = store.lastError {
                banner(error, icon: "exclamationmark.triangle", actionTitle: "Retry now") {
                    Task { await store.refresh() }
                }
            }
            if !store.notices.isEmpty {
                banner(store.notices.joined(separator: " · "), icon: "info.circle", actionTitle: "Dismiss") {
                    store.dismissNotices()
                }
            }
        }
    }

    private func banner(
        _ message: String, icon: String, actionTitle: String, action: @escaping () -> Void,
    ) -> some View {
        HStack(spacing: 10) {
            Image(systemName: icon)
            Text(message)
                .lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
            Button(action: action) {
                Text(actionTitle)
                    .font(.system(size: Theme.textXs))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 2)
                    .contentShape(Rectangle())
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.radiusSm)
                            .strokeBorder(Theme.highlight, lineWidth: 1),
                    )
            }
            .buttonStyle(.plain)
        }
        .font(.system(size: Theme.textSm))
        .foregroundStyle(Theme.highlight)
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(Theme.highlight.opacity(0.14))
        .overlay(alignment: .bottom) {
            Hairline(axis: .horizontal)
        }
    }
}

// MARK: - Transcript (with the WebUI's edge scroll-fades)

private struct TranscriptView: View {
    let store: SessionStore

    /// Tracks the bottom sentinel's visibility: scrolling up reveals
    /// the jump-to-bottom button; returning hides it.
    @State private var awayFromBottom = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private static let bottomId = "transcript-bottom"

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                // NOT LazyVStack: a lazy container estimates unmaterialized
                // row heights, and with .defaultScrollAnchor(.bottom) the
                // estimate error pins the scroll position to a region the
                // stack never materializes — the transcript renders as a
                // persistent blank page (reproduced: 200 heavy rows, doc
                // estimated at 5.6k vs 214k real, viewport pure black until
                // the next content change). A plain VStack always reports
                // exact heights, so the bottom anchor tracks reliably.
                VStack(alignment: .leading, spacing: 20) {
                    ForEach(store.transcript.rows) { row in
                        switch row {
                        case let .message(model):
                            MessageRow(
                                row: model,
                                artifactLoader: { await store.artifactData($0) },
                            )
                            // Rows are values precomputed at turn
                            // boundaries: a streaming frame only mutates
                            // the draft, so unchanged rows skip body
                            // evaluation entirely.
                            .equatable()
                        case let .turnSummary(summary):
                            // .block-turn-summary: the turn's closing
                            // review card. Revert stays hidden for
                            // read-only (sub-agent) sessions.
                            TurnSummaryView(
                                summary: summary,
                                statsLoader: { runId, force in
                                    await store.runStats(runId: runId, forceRefresh: force)
                                },
                                reverter: store.readOnly
                                    ? nil
                                    : { runId in await store.revertRun(runId: runId) },
                            )
                        }
                    }

                    if let draft = store.draft, !draft.isEmpty {
                        DraftView(
                            draft: draft,
                            artifactLoader: { await store.artifactData($0) },
                        )
                    } else if store.state == .running || store.state == .cancelling {
                        ThinkingDots()
                    }

                    // Bottom sentinel: drives the jump button's visibility.
                    Color.clear
                        .frame(height: 1)
                        .id(Self.bottomId)
                        .onAppear { awayFromBottom = false }
                        .onDisappear { awayFromBottom = true }
                }
                .padding(.horizontal, 24)
                .padding(.top, 24)
                .padding(.bottom, 12)
                .frame(maxWidth: Theme.contentWidth)
                .frame(maxWidth: .infinity)
            }
            // Pins the view to the latest content, including while a turn
            // streams — but releases as soon as the user scrolls up, unlike
            // a scrollToBottom-on-every-change loop.
            .defaultScrollAnchor(.bottom)
            .overlay {
                if !store.hasLoaded, store.transcript.rows.isEmpty, store.lastError == nil {
                    ProgressView("Loading conversation…")
                        .accessibilityLabel("Loading conversation")
                }
            }
            .overlay(alignment: .top) { scrollFade(fromTop: true) }
            .overlay(alignment: .bottom) { scrollFade(fromTop: false) }
            .overlay(alignment: .bottomTrailing) {
                if awayFromBottom {
                    Button {
                        if reduceMotion {
                            proxy.scrollTo(Self.bottomId, anchor: .bottom)
                        } else {
                            withAnimation(.easeOut(duration: 0.2)) {
                                proxy.scrollTo(Self.bottomId, anchor: .bottom)
                            }
                        }
                    } label: {
                        Image(systemName: "arrow.down")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Theme.muted)
                            .frame(width: 28, height: 28)
                            .background(Theme.bg1, in: Circle())
                            .overlay(Circle().strokeBorder(Theme.bg2, lineWidth: 1))
                            .shadow(color: .black.opacity(0.3), radius: 4, y: 2)
                    }
                    .buttonStyle(.plain)
                    .help("Jump to the latest message")
                    .accessibilityLabel("Jump to bottom")
                    .padding(.trailing, 20)
                    .padding(.bottom, 12)
                    .transition(.opacity)
                }
            }
            .animation(reduceMotion ? nil : .easeInOut(duration: 0.15), value: awayFromBottom)
        }
    }

    private func scrollFade(fromTop: Bool) -> some View {
        LinearGradient(
            colors: [Theme.bg0, Theme.bg0.opacity(0)],
            startPoint: fromTop ? .top : .bottom,
            endPoint: fromTop ? .bottom : .top,
        )
        .frame(height: 28)
        .allowsHitTesting(false)
    }
}

// MARK: - Pending requests

private struct PendingAreaView: View {
    let store: SessionStore

    var body: some View {
        if !store.pendingApprovals.isEmpty || !store.pendingQuestions.isEmpty {
            VStack(spacing: 10) {
                ForEach(store.pendingApprovals, id: \.approvalId) { approval in
                    ApprovalCard(approval: approval) { decision, always, trust in
                        Task {
                            await store.resolveApproval(
                                approval, decision: decision, always: always, trust: trust,
                            )
                        }
                    }
                }
                ForEach(store.pendingQuestions, id: \.id) { question in
                    QuestionCard(question: question) { selected, custom, skipped in
                        Task {
                            await store.answerQuestion(
                                question, selected: selected, customText: custom, skipped: skipped,
                            )
                        }
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 10)
            .frame(maxWidth: Theme.contentWidth)
            .frame(maxWidth: .infinity)
        }
    }
}

// MARK: - Statusbar (.statusbar)

private struct StatusBarView: View {
    let store: SessionStore
    var version: String

    var body: some View {
        HStack(spacing: 14) {
            Text(usageText)
                .font(Theme.monoXs)
            if store.turnCount > 0 {
                Text("turn \(store.turnCount)")
            }
            Spacer()
            if let occupancy = store.occupancy, let window = store.contextWindow, window > 0 {
                Text("\(formatTokenCount(occupancy)) / \(formatTokenCount(Int64(window))) context")
                    .help("Context occupancy")
            }
            if !version.isEmpty {
                Text(version)
            }
        }
        .font(.system(size: Theme.textXs))
        .foregroundStyle(Theme.muted)
        .padding(.horizontal, 16)
        .frame(minHeight: 28)
        .background(Theme.bg1)
        .windowDragSurface()
    }

    /// WebUI StatusBar: "12.3k in / 1.4k out · cache 82%".
    private var usageText: String {
        guard let usage = store.usage else { return "" }
        var text = "\(formatTokenCount(usage.inputTokens)) in / \(formatTokenCount(usage.outputTokens)) out"
        if let context = usage.contextTokens, context > 0 {
            let pct = min(100, Int(((Double(usage.cachedInputTokens ?? 0) / Double(context)) * 100).rounded()))
            text += " · cache \(pct)%"
        }
        return text
    }
}
