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

/// Session detail, laid out after the WebUI's shell: a 44px app-header
/// (bg0, hairline bottom border: sidebar toggle · theme toggle ·
/// workspace breadcrumb · session id · share · read-only/state badges ·
/// spacer · connection badge), the transcript with edge scroll-fades,
/// pending cards, the composer, and a 28px statusbar (bg1, hairline top
/// border) closing the pane.
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

    /// WebUI loom_theme: "dark" (default) or "light"; LoomApp applies
    /// it as the window's preferredColorScheme.
    @AppStorage("loom.theme") private var theme = "dark"
    @State private var copiedSessionId = false
    @State private var copiedShareLink = false

    var body: some View {
        VStack(spacing: 0) {
            header
            Hairline(axis: .horizontal)
            noticeBanner
            transcript
            pendingArea
            ComposerView(store: store, models: models)
            Hairline(axis: .horizontal)
            statusbar
        }
        .background(Theme.bg0)
    }

    // MARK: Header (.app-header)

    private var header: some View {
        HStack(spacing: 12) {
            // hdr-sidebar (bars)
            Button {
                sidebarCollapsed.toggle()
            } label: {
                Image(systemName: "line.3.horizontal")
            }
            .buttonStyle(GhostButtonStyle())
            .help("Toggle sidebar")

            // hdr-theme (circle-half-stroke)
            Button {
                theme = theme == "dark" ? "light" : "dark"
            } label: {
                Image(systemName: "circle.lefthalf.filled")
            }
            .buttonStyle(GhostButtonStyle())
            .help(theme == "dark" ? "Switch to light mode" : "Switch to dark mode")

            // hdr-ws: owning workspace breadcrumb — click to locate it
            // in the sidebar (WebUI revealCurrentWorkspace).
            if let workspaceName, !workspaceName.isEmpty {
                Button {
                    sidebarCollapsed = false
                } label: {
                    Text(workspaceName)
                        .font(.system(size: Theme.textSm))
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .frame(maxWidth: 240, alignment: .leading)
                        .fixedSize()
                }
                .buttonStyle(GhostButtonStyle())
                .help("Locate the owning workspace")
            }

            // hdr-session: the derived title once the conversation has
            // started (short id before that); click to copy the full id.
            Button(action: copySessionId) {
                Text(displayTitle)
                    .font(hasTitle ? .system(size: Theme.textSm) : Theme.monoSm)
                    .foregroundStyle(copiedSessionId ? Theme.success : Theme.muted)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: 280, alignment: .leading)
                    .fixedSize()
            }
            .buttonStyle(GhostButtonStyle())
            .help(copiedSessionId ? "Copied" : "\(store.sessionId) — click to copy the session ID")

            // hdr-share: mint + copy a public read-only link
            // (Shift+click revokes, like the WebUI).
            Button(action: shareSession) {
                Image(systemName: copiedShareLink ? "check" : "arrowshape.turn.up.right")
                    .foregroundStyle(copiedShareLink ? Theme.success : Theme.muted)
            }
            .buttonStyle(GhostButtonStyle())
            .help(copiedShareLink
                ? "Share link copied — anyone with the link can view this session read-only"
                : "Share session: copy a public read-only link (Shift+click to unshare)")

            readOnlyBadge

            stateBadge

            Spacer()

            connectionBadge

            Button {
                Task { await store.requestCompaction() }
            } label: {
                Image(systemName: "rectangle.compress.vertical")
            }
            .buttonStyle(GhostButtonStyle())
            .help("Compact context on next turn")
            .disabled(store.isBusy)
        }
        .padding(.horizontal, 20)
        .frame(minHeight: 44)
    }

    private var shortSessionId: String {
        store.sessionId.count > 8 ? String(store.sessionId.prefix(8)) : store.sessionId
    }

    private var hasTitle: Bool {
        sessionTitle?.isEmpty == false
    }

    private var displayTitle: String {
        hasTitle ? sessionTitle! : shortSessionId
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
        Task {
            guard let link = await store.shareLink() else { return }
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

    /// hdr-readonly (badge is-awaiting): sub-agent sessions (snapshot
    /// .delegated) and archived sessions are read-only.
    @ViewBuilder private var readOnlyBadge: some View {
        if store.readOnly {
            BadgeView(tone: .awaiting, text: "sub-agent · read-only")
                .help(store.readOnlyTitle)
        } else if archived {
            BadgeView(tone: .awaiting, text: "archived · read-only")
        }
    }

    @ViewBuilder private var stateBadge: some View {
        switch store.state {
        case .running:
            BadgeView(tone: .running, text: "running")
        case .awaitingApproval:
            BadgeView(tone: .awaiting, text: "awaiting approval")
        case .cancelling:
            BadgeView(tone: .awaiting, text: "cancelling")
        case .booting:
            BadgeView(tone: .plain, text: "booting")
        case .closed:
            BadgeView(tone: .plain, text: "closed")
        case .fatal:
            BadgeView(tone: .dead, text: "fatal")
        case .idle:
            BadgeView(tone: .plain, text: "idle")
        default:
            EmptyView()
        }
    }

    @ViewBuilder private var connectionBadge: some View {
        switch store.connection {
        case .live:
            BadgeView(tone: .live, text: "live")
        case .connecting:
            BadgeView(tone: .reconnecting, text: "connecting…")
        case let .offline(attempt):
            BadgeView(tone: .reconnecting, text: "reconnecting (\(attempt))")
        case .drained:
            BadgeView(tone: .dead, text: "server shut down")
        }
    }

    // MARK: Notices (.banner)

    @ViewBuilder private var noticeBanner: some View {
        if !store.notices.isEmpty || store.lastError != nil {
            HStack(spacing: 10) {
                Image(systemName: "exclamationmark.triangle")
                Text(store.lastError ?? store.notices.joined(separator: " · "))
                    .lineLimit(2)
                Button {
                    if store.lastError != nil {
                        Task { await store.refresh() }
                    } else {
                        store.dismissNotices()
                    }
                } label: {
                    Text(store.lastError != nil ? "Retry now" : "Dismiss")
                        .font(.system(size: Theme.textXs))
                        .padding(.horizontal, 10)
                        .padding(.vertical, 2)
                        .overlay(
                            RoundedRectangle(cornerRadius: Theme.radiusSm)
                                .strokeBorder(Theme.highlight, lineWidth: 1),
                        )
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.highlight)
            }
            .font(.system(size: Theme.textSm))
            .foregroundStyle(Theme.highlight)
            .frame(maxWidth: .infinity)
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .background(Theme.highlight.opacity(0.14))
            .overlay(alignment: .bottom) {
                Hairline(axis: .horizontal)
            }
        }
    }

    // MARK: Transcript (with the WebUI's edge scroll-fades)

    private var transcript: some View {
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
                ForEach(visibleMessages) { message in
                    MessageRow(
                        message: message,
                        toolResults: toolResults,
                        showActions: actionMessageIds.contains(message.id),
                        artifactLoader: { await store.artifactData($0) },
                    )
                }

                if let draft = store.draft, !draft.isEmpty {
                    DraftView(
                        draft: draft,
                        artifactLoader: { await store.artifactData($0) },
                    )
                } else if store.state == .running || store.state == .cancelling {
                    ThinkingDots()
                }
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
        .overlay(alignment: .top) { scrollFade(fromTop: true) }
        .overlay(alignment: .bottom) { scrollFade(fromTop: false) }
    }

    /// Cross-message call_id → result map (the WebUI's histTools):
    /// tool results arrive in their own assistant messages and patch
    /// the block created for the matching call.
    private var toolResults: [String: ContentPart.ToolResult] {
        var map: [String: ContentPart.ToolResult] = [:]
        for message in store.messages {
            for part in message.parts {
                if case let .toolResult(result) = part {
                    map[result.callId] = result
                }
            }
        }
        return map
    }

    /// Messages that produce at least one render item — pure
    /// tool_result carrier messages fold into their call's block and
    /// leave no row of their own.
    private var visibleMessages: [Message] {
        store.messages.filter { message in
            switch message.role {
            case .assistant:
                !message.assistantItems(toolResults: toolResults).isEmpty
            default:
                true
            }
        }
    }

    /// WebUI closeTurn: the action row attaches to the LAST text
    /// segment of a finished turn — settled at the next user message,
    /// and at the transcript tail only when no turn is in flight.
    private var actionMessageIds: Set<String> {
        var ids: Set<String> = []
        var candidate: String?
        for message in store.messages {
            switch message.role {
            case .user:
                if let candidate {
                    ids.insert(candidate)
                }
                candidate = nil
            case .assistant:
                if !message.copyText.isEmpty {
                    candidate = message.id
                }
            default:
                break
            }
        }
        let midTurn = store.state == .running
            || store.state == .cancelling
            || store.state == .awaitingApproval
        if !midTurn, let candidate {
            ids.insert(candidate)
        }
        return ids
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

    // MARK: Pending requests

    @ViewBuilder private var pendingArea: some View {
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

    // MARK: Statusbar (.statusbar)

    private var statusbar: some View {
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
            Text(version)
        }
        .font(.system(size: Theme.textXs))
        .foregroundStyle(Theme.muted)
        .padding(.horizontal, 16)
        .frame(minHeight: 28)
        .background(Theme.bg1)
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

// MARK: - Ghost button (.icon-btn)

/// The WebUI's .icon-btn: bare muted glyph, fg + bg2 wash on hover.
struct GhostButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled
    @State private var hovered = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15))
            .foregroundStyle(
                isEnabled ? (hovered ? Theme.fg : Theme.muted) : Theme.muted.opacity(0.4),
            )
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
            .background(
                configuration.isPressed || hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .contentShape(Rectangle())
            .onHover { hovered = $0 }
    }
}
