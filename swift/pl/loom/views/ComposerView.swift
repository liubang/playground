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

/// Message composer, after the WebUI's composer.css: the plan panel
/// pinned above, then a bg1 capsule box (radius 16) holding the
/// textarea, and a bottom bar — separated by a top hairline — with the
/// model/reasoning/approval picker capsules on the left and the
/// ctx-gauge ring + send/stop circle as twin 32px buttons on the right.
/// Return sends, ⇧Return inserts a newline (⌘Return also sends). While
/// a turn is busy the prompt is steered into the session queue by the
/// server. The input text lives in SessionStore.composerDraft so an
/// unfinished message survives switching to another session and back
/// (the chat view itself is destroyed on every switch).
struct ComposerView: View {
    @Bindable var store: SessionStore
    /// Model catalog for the picker (from SessionListStore).
    var models: [MetaModels.ModelInfo] = []

    @FocusState private var focused: Bool
    @ScaledMetric(relativeTo: .body) private var inputFontSize: CGFloat = 14
    /// The ctx-gauge's instant hover card (system tooltips lag 1–2s,
    /// which reads as "no hover feedback at all").
    @State private var gaugeCardShown = false
    @State private var gaugeHoverTask: Task<Void, Never>?
    @State private var steerQueueExpanded = false

    var body: some View {
        VStack(spacing: 8) {
            if let error = store.lastError {
                feedbackCard(title: "Action couldn't complete", detail: error, icon: "exclamationmark.circle", tint: Theme.warning) {
                    store.dismissError()
                }
            }

            if !store.notices.isEmpty {
                HStack(spacing: 8) {
                    Image(systemName: "info.circle")
                    Text(store.notices.joined(separator: " · "))
                        .lineLimit(2)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    Button { store.dismissNotices() } label: {
                        Image(systemName: "xmark")
                            .frame(width: 20, height: 20)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help("Dismiss notices")
                    .accessibilityLabel("Dismiss notices")
                }
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.muted)
                .padding(.horizontal, 4)
                .frame(maxWidth: Theme.contentWidth)
            }

            if let plan = store.plan, !plan.items.isEmpty {
                PlanPanel(plan: plan)
                    .id("\(store.planIdentity.uuidString):\(([plan.title ?? ""] + plan.items.map(\.goal)).joined(separator: "\u{1f}"))")
                    .frame(maxWidth: Theme.contentWidth)
                    .frame(maxWidth: .infinity)
                    .padding(.bottom, 6)
            }

            VStack(spacing: 0) {
                if !store.pendingSteers.isEmpty {
                    steerQueue
                        .padding(.horizontal, -6)
                }

                // Textarea (composer.css .composer textarea): the
                // placeholder rides in the editor's own background so it
                // shares the frame — only the editor's internal text
                // inset (~5pt) separates them, keeping the caret and the
                // hint aligned. Caret takes the foreground color (WebUI
                // inherits --fg), not the system accent blue.
                TextEditor(text: $store.composerDraft)
                    .font(.system(size: inputFontSize))
                    // Match the app's text color: the default .primary is pure white
                    // in dark mode and read as bold-ish next to the beige transcript.
                    .foregroundStyle(Theme.fg)
                    .lineSpacing(5) // ≈ the WebUI's 1.55 line-height at 14px
                    .scrollContentBackground(.hidden)
                    .tint(Theme.fg)
                    .focused($focused)
                    .accessibilityLabel("Message")
                    .accessibilityHint("Return to send, Shift-Return for a new line")
                    .frame(minHeight: 44, maxHeight: 200)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 9)
                    .padding(.vertical, 4)
                    .padding(.top, 6)
                    .background(alignment: .topLeading) {
                        if store.composerDraft.isEmpty {
                            // Match the editor's internal text inset
                            // (~5pt leading, ~6pt top) so the caret and
                            // the hint sit on the same first line.
                            Text(placeholder)
                                .foregroundStyle(Theme.muted.opacity(0.7))
                                .font(.system(size: inputFontSize))
                                .lineSpacing(5)
                                .padding(.leading, 5)
                                .padding(.top, 6)
                                .allowsHitTesting(false)
                        }
                    }
                    .onKeyPress(keys: [.return], phases: .down) { press in
                        // Bare Return only: ⇧Return inserts a newline;
                        // ⌘/⌥ Return fall through to the send button's
                        // and the approval card's keyboard shortcuts.
                        guard press.modifiers.isEmpty, !imeComposing, canSend
                        else { return .ignored }
                        send()
                        return .handled
                    }

                // composer-bar: hairline-separated button row; negative
                // horizontal margins stretch the divider to the box's
                // full width (composer.css: margin 2px -6px 0).
                HStack(spacing: 8) {
                    modelPicker
                    reasoningPicker
                    approvalPicker

                    Spacer()

                    if let fraction = store.occupancyFraction {
                        CtxGauge(fraction: fraction, hotThreshold: store.compactTriggerRatio)
                            .accessibilityElement(children: .ignore)
                            .accessibilityLabel("Context occupancy")
                            .accessibilityValue(gaugeFacts.joined(separator: ". "))
                            .onHover { inside in
                                // Hysteresis both ways: a passing
                                // cursor must not flash the card,
                                // and a jittery edge-crossing must
                                // not flap it.
                                gaugeHoverTask?.cancel()
                                gaugeHoverTask = Task { @MainActor in
                                    try? await Task.sleep(
                                        for: .milliseconds(inside ? 150 : 200),
                                    )
                                    guard !Task.isCancelled else { return }
                                    gaugeCardShown = inside
                                }
                            }
                            .onDisappear {
                                gaugeHoverTask?.cancel()
                                gaugeHoverTask = nil
                                gaugeCardShown = false
                            }
                    }

                    sendStopButton
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .padding(.horizontal, -6)
                .overlay(alignment: .top) {
                    Hairline(axis: .horizontal)
                }
            }
            // Keep the queue flush with the box edge while the editor and
            // toolbar retain their 6px horizontal inset.
            .padding(.horizontal, 6)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusXl))
            .clipShape(RoundedRectangle(cornerRadius: Theme.radiusXl))
            // Focus treatment (composer.css .composer-box:focus-within):
            // a crisp 1px primary border plus a crisp 3px halo drawn
            // just OUTSIDE the box (the CSS `0 0 0 3px` spread) — no
            // blurred neon; the ambient shadow stays barely-there.
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusXl)
                    .strokeBorder(
                        focused ? Theme.primary.opacity(0.5) : Theme.bg2,
                        lineWidth: 1,
                    ),
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusXl + 1.5)
                    .strokeBorder(Theme.primary.opacity(focused ? 0.12 : 0), lineWidth: 3)
                    .padding(-1.5),
            )
            .shadow(
                color: focused ? Theme.primary.opacity(0.08) : .clear,
                radius: focused ? 10 : 0,
            )
            .animation(.easeInOut(duration: 0.18), value: focused)
            // composer.css .composer-box: max-width --content-width, margin
            // 0 auto — the box shares the transcript's column clamp.
            .frame(maxWidth: Theme.contentWidth)
            .frame(maxWidth: .infinity)
        }
        .padding(.horizontal, 20)
        .padding(.top, 12)
        .padding(.bottom, 14)
        .background(Theme.bg0)
        .overlay(alignment: .bottomTrailing) {
            if gaugeCardShown, store.occupancyFraction != nil {
                // Anchored to the COMPOSER's bottom-trailing, not the
                // 32px gauge: it floats just above the composer bar,
                // trailing edge aligned with the gauge's (20 outer +
                // 6 box + 10 bar padding + 32 send + 8 spacing), and
                // stays entirely inside the composer's own bounds —
                // so the statusbar (a later sibling painting an
                // opaque bg over whatever sticks out) can never clip
                // it. Purely presentational: hit-testing off, or the
                // insert transition sweeping the gauge would retrigger
                // the hover and flap.
                GaugeDetailCard(facts: gaugeFacts)
                    .accessibilityHidden(true)
                    .padding(.trailing, 76)
                    .padding(.bottom, 46)
                    .allowsHitTesting(false)
                    .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
        }
        .animation(.easeInOut(duration: 0.12), value: gaugeCardShown)
        .onAppear { focused = true }
        .onChange(of: store.composerFocusRequest) { _, _ in focused = true }
        .onChange(of: store.pendingSteers.isEmpty) { _, empty in
            if empty {
                steerQueueExpanded = false
            }
        }
    }

    // MARK: Queued steering

    /// Pending steers belong to the composer, not the transcript. The server
    /// injects them before the next model call, or relays them into a new turn
    /// if the current turn ends first. There is no queue mutation API yet.
    private var steerQueue: some View {
        VStack(spacing: 0) {
            Button {
                withAnimation(.easeInOut(duration: 0.2)) {
                    steerQueueExpanded.toggle()
                }
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "arrow.turn.down.right")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(Theme.primary)
                        .frame(width: 16)

                    Text("Queued")
                        .font(.system(size: Theme.textXs, weight: .medium))
                        .foregroundStyle(Theme.muted)
                        .fixedSize()

                    Text(steerPreview)
                        .font(.system(size: Theme.textSm))
                        .foregroundStyle(Theme.fg)
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .frame(maxWidth: .infinity, alignment: .leading)

                    if store.pendingSteers.count > 1 {
                        Text("+\(store.pendingSteers.count - 1)")
                            .font(.system(size: Theme.textXs, weight: .medium))
                            .foregroundStyle(Theme.muted)
                    }
                    Image(systemName: "chevron.down")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(Theme.muted)
                        .rotationEffect(.degrees(steerQueueExpanded ? 180 : 0))
                }
                .padding(.horizontal, 16)
                .frame(height: 42)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("Queued for the next model step; if this turn finishes first, sent in the next turn")
            .accessibilityLabel("\(store.pendingSteers.count) queued steering messages. \(steerPreview)")
            .accessibilityHint("Expand to read all queued messages")

            if steerQueueExpanded {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(store.pendingSteers.enumerated()), id: \.offset) { index, text in
                            HStack(alignment: .top, spacing: 10) {
                                Text("\(index + 1)")
                                    .foregroundStyle(Theme.muted)
                                    .frame(width: 16, alignment: .leading)
                                Text(text)
                                    .foregroundStyle(Theme.fg)
                                    .textSelection(.enabled)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                            }
                            .font(.system(size: Theme.textSm))
                            .padding(.vertical, 7)
                        }
                    }
                    .padding(.horizontal, 16)
                }
                .frame(height: min(CGFloat(store.pendingSteers.count) * 56, 160))

                Text("Added before the next model step, or sent after this turn ends.")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 16)
                    .padding(.bottom, 10)
            }

            Hairline(axis: .horizontal)
        }
        .background(Theme.bg2.opacity(0.35))
    }

    private var steerPreview: String {
        store.pendingSteers.last?
            .split(whereSeparator: \.isNewline)
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }

    // MARK: Contextual feedback

    private func feedbackCard(
        title: String, detail: String, icon: String, tint: Color,
        dismiss: @escaping () -> Void,
    ) -> some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 15))
                .foregroundStyle(tint)
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .font(.system(size: Theme.textSm, weight: .medium))
                    .foregroundStyle(Theme.fg)
                DisclosureGroup("Details") {
                    Text(detail)
                        .font(Theme.monoXs)
                        .foregroundStyle(Theme.muted)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.top, 4)
                }
                .font(.system(size: Theme.textXs))
                .foregroundStyle(Theme.muted)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Button(action: dismiss) {
                Image(systemName: "xmark")
                    .font(.system(size: 10, weight: .semibold))
                    .frame(width: 20, height: 20)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.muted)
            .help("Dismiss")
            .accessibilityLabel("Dismiss \(title)")
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusLg))
        .overlay(RoundedRectangle(cornerRadius: Theme.radiusLg)
            .strokeBorder(Theme.bg2, lineWidth: 1))
        .frame(maxWidth: Theme.contentWidth)
    }

    // MARK: Send / stop (twin-circle with the ctx gauge)

    @ViewBuilder private var sendStopButton: some View {
        if store.isBusy {
            Button {
                Task { await store.cancel() }
            } label: {
                Image(systemName: "stop.fill")
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.onAccent)
                    .frame(width: 32, height: 32)
                    .background(Theme.error, in: Circle())
            }
            .buttonStyle(.plain)
            .keyboardShortcut(.escape, modifiers: [])
            .help("Stop the current turn (⎋)")
            .accessibilityLabel("Stop the current turn")
        } else {
            Button(action: send) {
                Image(systemName: "arrow.up")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(Theme.onAccent)
                    .frame(width: 32, height: 32)
                    .background(Theme.primary, in: Circle())
            }
            .buttonStyle(.plain)
            .opacity(canSend ? 1 : 0.4)
            .disabled(!canSend)
            .keyboardShortcut(.return, modifiers: .command)
            .help("Send (Return — ⇧Return for a newline)")
            .accessibilityLabel("Send message")
        }
    }

    // MARK: Pickers (.picker-btn)

    private var currentModelRef: String {
        // WebUI applySnapshotMeta: provider/model when both are known;
        // a bare snapshot model name falls back to the catalog's
        // default ref so the picker still marks the active row.
        if !store.providerName.isEmpty, !store.modelName.isEmpty {
            return "\(store.providerName)/\(store.modelName)"
        }
        return store.defaultModelRef ?? store.modelName
    }

    @ViewBuilder private var modelPicker: some View {
        if !models.isEmpty {
            let nameCounts = Dictionary(models.map { ($0.name, 1) }, uniquingKeysWith: +)
            PickerCapsule(
                label: store.modelName.isEmpty ? "model" : store.modelName,
                isActive: false,
                help: "Switch model",
            ) { close in
                ForEach(models, id: \.self) { model in
                    let ref = "\(model.provider)/\(model.name)"
                    PickerMenuItem(
                        title: nameCounts[model.name, default: 0] > 1
                            ? "\(model.provider) / \(model.name)" : model.name,
                        detail: model.contextWindow.map { formatContextWindow($0) },
                        isCurrent: ref == currentModelRef,
                    ) {
                        close()
                        Task { await store.pickModel(ref) }
                    }
                }
            }
        }
    }

    private var reasoningPicker: some View {
        PickerCapsule(
            label: store.reasoningEffort == "default" ? "reasoning" : store.reasoningEffort,
            isOn: store.reasoningEffort != "default",
            help: "Set reasoning (current: \(store.reasoningEffort))",
        ) { close in
            ForEach(ReasoningOption.all, id: \.value) { option in
                PickerMenuItem(
                    title: option.label,
                    isCurrent: store.reasoningEffort == option.value,
                ) {
                    close()
                    Task { await store.pickReasoning(option.value) }
                }
            }
        }
    }

    /// Approval baseline: the default (standard) shows only the shield
    /// icon; non-default modes expand a short name with an amber tint.
    private var approvalPicker: some View {
        PickerCapsule(
            icon: "shield",
            label: store.approvalMode == "on-request"
                ? nil
                : (ApprovalOption(rawValue: store.approvalMode)?.short ?? store.approvalMode),
            isWarn: store.approvalMode != "on-request",
            help: "Switch approval baseline (workspace-level; takes effect next turn)",
        ) { close in
            ForEach(ApprovalOption.all, id: \.rawValue) { option in
                PickerMenuItem(
                    title: "\(option.short) · \(option.rawValue)",
                    detail: option.hint,
                    isCurrent: store.approvalMode == option.rawValue,
                ) {
                    close()
                    Task { await store.pickApprovalMode(option.rawValue) }
                }
            }
        }
    }

    // MARK: Submit

    /// One stable hint, focused or not (diverges from the WebUI's
    /// focus-switched key-hint placeholder on purpose): the shortcut
    /// docs already live in the send/stop button tooltips, and a hint
    /// that rewrites itself on focus reads as UI noise. The busy/idle
    /// wording is the information worth keeping — it tells the user
    /// the input will steer the running turn.
    private var placeholder: String {
        store.isBusy ? "Steer this turn…" : "Message loom…"
    }

    private var canSend: Bool {
        !store.sendingPrompt
            && !store.composerDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && store.state != .closed
            && store.state != .booting
    }

    /// True while an IME composition (Chinese/Japanese input) is in
    /// progress: the Return that CONFIRMS a candidate must not send
    /// the message. The marked-text state lives on the first responder
    /// (the TextEditor's NSTextView), not on the input context.
    private var imeComposing: Bool {
        guard let view = NSApp.keyWindow?.firstResponder as? NSTextView else { return false }
        return view.hasMarkedText()
    }

    private func send() {
        guard canSend else { return }
        store.sendComposerDraft()
    }

    private func formatContextWindow(_ value: Int) -> String {
        formatTokenCount(Int64(value))
    }

    /// The gauge hover card's facts: occupancy, then the one fact
    /// that drives decisions — when compaction hits. (Compact target
    /// and nominal window are server internals; a four-row card was
    /// taller than the composer is deep and read as clutter.)
    private var gaugeFacts: [String] {
        guard let occupancy = store.occupancy,
              let effective = store.contextWindow, effective > 0
        else { return ["Context occupancy"] }
        let pct = Int((Double(occupancy) / Double(effective)) * 100)
        var facts = [
            "Context \(formatTokenCount(occupancy)) / \(formatTokenCount(Int64(effective))) (\(pct)%)",
        ]
        if let trigger = store.window?.compactTrigger, trigger > 0 {
            facts.append("Compacts at ~\(formatTokenCount(Int64(trigger)))")
        }
        return facts
    }
}

// MARK: - Picker options

private struct ReasoningOption {
    let value: String
    let label: String

    static let all = [
        ReasoningOption(value: "default", label: "Default (follow model)"),
        ReasoningOption(value: "off", label: "Off"),
        ReasoningOption(value: "low", label: "Low"),
        ReasoningOption(value: "medium", label: "Medium"),
        ReasoningOption(value: "high", label: "High"),
    ]
}

private enum ApprovalOption: String, CaseIterable {
    case onRequest = "on-request"
    case dangerOnly = "danger-only"
    case never

    static let all: [ApprovalOption] = [.onRequest, .dangerOnly, .never]

    var short: String {
        switch self {
        case .onRequest: "standard"
        case .dangerOnly: "dev"
        case .never: "auto"
        }
    }

    var hint: String {
        switch self {
        case .onRequest:
            "Default: in-workspace reads/writes are auto-approved; out-of-bounds or dangerous actions ask"
        case .dangerOnly:
            "Dev: only dangerous commands/sites ask; dev commands and normal browsing are auto-approved"
        case .never:
            "Unattended: dangerous actions are denied outright; never waits for approval"
        }
    }
}

// MARK: - Picker capsule (.picker-btn) + popover menu (.menu)

/// The 26px capsule buttons in the composer bar; opens a bg1 popover
/// styled after the WebUI's .menu overlay. The content receives a
/// `close` action so picking an item dismisses the popover (macOS
/// menu behavior — popovers do not close on their own).
private struct PickerCapsule<Content: View>: View {
    var icon: String?
    var label: String?
    var isActive = false
    var isOn = false
    var isWarn = false
    var help: String
    @ViewBuilder var content: (_ close: @escaping () -> Void) -> Content

    @State private var open = false
    @State private var hovered = false

    var body: some View {
        Button {
            open.toggle()
        } label: {
            HStack(spacing: 4) {
                if let icon {
                    Image(systemName: icon)
                        .font(.system(size: 11))
                }
                if let label {
                    Text(label)
                        .font(.system(size: Theme.textSm, weight: .medium))
                    Image(systemName: "chevron.down")
                        .font(.system(size: 8))
                        .foregroundStyle(Theme.muted)
                }
            }
            .foregroundStyle(foreground)
            .padding(.horizontal, 10)
            .frame(height: 26)
            .background(
                isWarn ? Theme.warning.opacity(0.12) : Theme.bg2,
                in: RoundedRectangle(cornerRadius: Theme.radiusMd),
            )
            // .picker-btn: 1px transparent border at rest, muted on
            // hover, primary while the menu is open.
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusMd)
                    .strokeBorder(
                        open || isActive ? Theme.primary : (hovered ? Theme.muted : Color.clear),
                        lineWidth: 1,
                    ),
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .help(help)
        .accessibilityLabel(help)
        .popover(isPresented: $open, arrowEdge: .top) {
            VStack(alignment: .leading, spacing: 2) {
                content { open = false }
            }
            .padding(6)
            .frame(minWidth: 220, alignment: .leading)
            .background(Theme.bg1)
            .presentationCompactAdaptation(.popover)
        }
    }

    private var foreground: Color {
        if isWarn {
            return Theme.warning
        }
        if isOn || isActive {
            return Theme.primary
        }
        return Theme.fg
    }
}

/// One row in a picker popover (.menu-item): title, optional detail on
/// the right, ✓ for the current value.
private struct PickerMenuItem: View {
    let title: String
    var detail: String?
    let isCurrent: Bool
    let action: () -> Void

    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Text(title)
                    .font(.system(size: Theme.textMd))
                    .foregroundStyle(isCurrent ? Theme.primary : Theme.fg)
                Spacer()
                if let detail {
                    Text(detail)
                        .font(.system(size: 11))
                        .foregroundStyle(Theme.muted)
                }
                Image(systemName: "checkmark")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(Theme.primary)
                    .opacity(isCurrent ? 1 : 0)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .background(
                hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }
}

// MARK: - Plan panel

/// A quiet work trail: a task title and completion count above the steps,
/// with one marker per step and a connector leading to the next. The active
/// step gets the emphasis; completed plans retire after a short pause.
struct PlanPanel: View {
    let plan: PlanPayload

    @State private var expanded = true
    @State private var retiring = false
    @State private var retired = false
    @State private var hovered = false
    @State private var retirementTask: Task<Void, Never>?

    private var items: [PlanPayload.Item] {
        plan.items
    }

    private var doneCount: Int {
        items.filter { $0.status == "done" }.count
    }

    private var currentIndex: Int? {
        items.firstIndex { $0.status == "in_progress" }
    }

    private var title: String {
        let value = plan.title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? "Plan" : value
    }

    private var allDone: Bool {
        doneCount == items.count
    }

    var body: some View {
        Group {
            if !retired {
                VStack(alignment: .leading, spacing: 0) {
                    summaryRow

                    if expanded {
                        itemList
                    }
                }
                .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusLg))
                .clipShape(RoundedRectangle(cornerRadius: Theme.radiusLg))
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusLg)
                        .strokeBorder(Theme.bg2.opacity(0.65), lineWidth: 1),
                )
                .opacity(retiring ? 0 : 1)
                .offset(y: retiring ? 3 : 0)
                .animation(.easeInOut(duration: 0.3), value: retiring)
            }
        }
        .onAppear {
            if allDone {
                expanded = false
            }
            updateRetirement()
        }
        .onChange(of: allDone) { _, done in
            if done {
                expanded = false
            }
            updateRetirement()
        }
        .onChange(of: expanded) { _, _ in updateRetirement() }
        .onDisappear { retirementTask?.cancel() }
    }

    private var summaryRow: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.18)) { expanded.toggle() }
        } label: {
            HStack(alignment: .top, spacing: 12) {
                VStack(alignment: .leading, spacing: 5) {
                    Text(title)
                        .font(.system(size: Theme.textMd, weight: .semibold))
                        .foregroundStyle(Theme.fg)
                        .lineLimit(1)
                        .frame(maxWidth: .infinity, alignment: .leading)

                    if !expanded {
                        Text(allDone ? "All done" : currentIndex.map { items[$0].goal } ?? "Not started")
                            .font(.system(size: Theme.textSm))
                            .foregroundStyle(allDone ? Theme.success : currentIndex == nil ? Theme.muted : Theme.primary)
                            .lineLimit(1)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }

                Text("\(doneCount) / \(items.count)")
                    .font(.system(size: Theme.textXs, weight: .medium, design: .monospaced))
                    .foregroundStyle(allDone ? Theme.success : Theme.muted)
                    .monospacedDigit()
                    .padding(.top, 2)

                Image(systemName: "chevron.down")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(Theme.muted)
                    .rotationEffect(.degrees(expanded ? 0 : -90))
                    .padding(.top, 3)
            }
            .padding(.horizontal, 18)
            .padding(.vertical, 15)
            .background(hovered ? Theme.bg2.opacity(0.35) : Color.clear)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
        .accessibilityLabel("\(title), \(doneCount) of \(items.count) steps complete")
        .accessibilityHint(expanded ? "Collapse plan" : "Show plan steps")
    }

    private var itemRows: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                HStack(alignment: .top, spacing: 12) {
                    stepMarker(status: item.status)
                        .frame(width: 16, height: 16)
                        .frame(width: 18)
                    Text(item.goal)
                        .font(.system(
                            size: Theme.textSm,
                            weight: item.status == "in_progress" ? .medium : .regular,
                        ))
                        .foregroundStyle(item.status == "in_progress" ? Theme.fg : Theme.muted)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(minHeight: index == items.count - 1 ? 18 : 36, alignment: .top)
                .overlay(alignment: .topLeading) {
                    if index < items.count - 1 {
                        Rectangle()
                            .fill(Theme.bg3)
                            .frame(width: 1)
                            .padding(.leading, 8.5)
                            .padding(.top, 20)
                            .padding(.bottom, 4)
                            .frame(maxHeight: .infinity)
                    }
                }
            }
        }
        .padding(.horizontal, 18)
        .padding(.bottom, 16)
    }

    @ViewBuilder
    private var itemList: some View {
        if items.count > 6 {
            ScrollView { itemRows }
                .frame(maxHeight: 220)
        } else {
            itemRows
        }
    }

    private func stepMarker(status: String) -> some View {
        ZStack {
            switch status {
            case "done":
                Circle().fill(Theme.success.opacity(0.18))
                Image(systemName: "checkmark")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(Theme.success)
            case "in_progress":
                Circle().strokeBorder(Theme.primary.opacity(0.6), lineWidth: 1)
                Circle().fill(Theme.primary).frame(width: 6, height: 6)
            default:
                Circle().strokeBorder(Theme.muted.opacity(0.42), lineWidth: 1)
            }
        }
    }

    private func updateRetirement() {
        retirementTask?.cancel()
        retirementTask = nil
        guard allDone, !expanded else {
            retiring = false
            retired = false
            return
        }
        guard !retired else { return }
        retirementTask = Task {
            do {
                try await Task.sleep(for: .milliseconds(2200))
                guard !Task.isCancelled else { return }
                retiring = true
                try await Task.sleep(for: .milliseconds(360))
                guard !Task.isCancelled else { return }
                retired = true
            } catch {
                // A changed or expanded plan cancels the pending retirement.
            }
        }
    }
}

// MARK: - Context gauge (.ctx-gauge)

/// Instant hover card for the ctx-gauge: occupancy plus the
/// compaction schedule, without the system tooltip's 1–2s delay.
private struct GaugeDetailCard: View {
    let facts: [String]

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            ForEach(Array(facts.enumerated()), id: \.offset) { index, fact in
                Text(fact)
                    .font(.system(size: Theme.textXs, weight: index == 0 ? .semibold : .regular))
                    .foregroundStyle(index == 0 ? Theme.fg : Theme.muted)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
        .shadow(color: .black.opacity(0.3), radius: 8, y: 2)
        .fixedSize()
    }
}

/// The WebUI's ctx-gauge: a quiet 32px ring that starts showing the
/// percentage inside at 40%, warms to amber at 60% and to red at the
/// compact-trigger ratio (≈80%).
struct CtxGauge: View {
    let fraction: Double
    var hotThreshold = 0.8

    private static let warm = 0.6
    /// Below this the arc alone carries the value (a number would be
    /// noise at 2% anyway); from here up the percentage rides inside
    /// the ring — earlier than the amber level, so the exact figure
    /// is on screen before compaction looms.
    private static let showPercent = 0.4

    var body: some View {
        ZStack {
            Circle()
                .stroke(Theme.bg2, lineWidth: 3)
            // Near-zero occupancy: hide the arc outright — with a round
            // cap a 1-2% trim renders as a stray dot on the ring, while
            // the WebUI's dashoffset collapses to invisibility there.
            if fraction > 0.005 {
                Circle()
                    .trim(from: 0, to: min(max(fraction, 0), 1))
                    .stroke(ringColor, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .animation(.easeOut(duration: 0.35), value: fraction)
            }
            if fraction >= Self.showPercent {
                Text("\(Int(fraction * 100))%")
                    .font(.system(size: 9.5, weight: .semibold))
                    .foregroundStyle(ringColor)
            }
        }
        .frame(width: 32, height: 32)
    }

    private var ringColor: Color {
        if fraction >= hotThreshold {
            return Theme.error
        }
        if fraction >= Self.warm {
            return Theme.warning
        }
        return Theme.muted
    }
}
