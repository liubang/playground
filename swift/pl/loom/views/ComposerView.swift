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

import SwiftUI

/// Message composer, after the WebUI's composer.css: the plan panel
/// pinned above, then a bg1 capsule box (radius 16) holding the
/// textarea, and a bottom bar — separated by a top hairline — with the
/// model/reasoning/approval picker capsules on the left, and on the
/// right a quiet 28px ctx-gauge ring next to the 32px send/stop button
/// — deliberately NOT a twin pair: the gauge is ambient information,
/// send is the bar's only primary action, and equals-in-shape would
/// read as equals-in-rank.
/// Return sends, ⇧Return inserts a newline (⌘Return also sends). While
/// a turn is busy the prompt is steered into the session queue by the
/// server. The input text lives in SessionStore.composerDraft so an
/// unfinished message survives switching to another session and back
/// (the chat view itself is destroyed on every switch).
struct ComposerView: View {
    @Bindable var store: SessionStore
    /// Model catalog for the picker (from SessionListStore).
    var models: [MetaModels.ModelInfo] = []

    @State private var editorFocused = false
    /// Increment to focus the editor (appear, composerFocusRequest) —
    /// see ComposerTextView.focusRequest for why this isn't a FocusState.
    @State private var focusToken = 0
    @ScaledMetric(relativeTo: .body) private var inputFontSize: CGFloat = 14
    /// The ctx-gauge's instant hover card (system tooltips lag 1–2s,
    /// which reads as "no hover feedback at all").
    @State private var gaugeCardShown = false
    @State private var gaugeHoverTask: Task<Void, Never>?
    @State private var steerQueueExpanded = false
    @State private var sendHovered = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

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

                // Textarea (composer.css .composer textarea): a
                // hand-rolled NSTextView — caret, placeholder, and typed
                // text share one coordinate space (the TextEditor's
                // overlay placeholder was hand-aligned and the default
                // caret sliced through its first glyph). Bare Return
                // submits, ⇧Return inserts a newline, ⌘Return submits
                // too — see ComposerNSTextView.keyDown.
                ComposerTextView(
                    text: $store.composerDraft,
                    fontSize: inputFontSize,
                    placeholder: placeholder,
                    focusRequest: focusToken,
                    onFocusChange: { editorFocused = $0 },
                    onSubmit: {
                        guard canSend else { return false }
                        send()
                        return true
                    },
                )
                .frame(minHeight: 44, maxHeight: 200)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.horizontal, 9)
                .padding(.vertical, 4)
                .padding(.top, 6)
                .accessibilityLabel("Message")
                .accessibilityHint("Return to send, Shift-Return for a new line")

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
                        editorFocused ? Theme.primary.opacity(0.5) : Theme.bg2,
                        lineWidth: 1,
                    ),
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusXl + 1.5)
                    .strokeBorder(Theme.primary.opacity(editorFocused ? 0.12 : 0), lineWidth: 3)
                    .padding(-1.5),
            )
            .shadow(
                color: editorFocused ? Theme.primary.opacity(0.08) : .clear,
                radius: editorFocused ? 10 : 0,
            )
            .animation(.easeInOut(duration: 0.18), value: editorFocused)
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
        .onAppear { focusToken += 1 }
        .onChange(of: store.composerFocusRequest) { _, _ in focusToken += 1 }
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

    // MARK: Send / stop (the bar's sole primary action)

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
            // Quiet-but-present: the disabled button keeps its button
            // SHAPE — a bg2 disc one notch above the composer's bg1,
            // muted glyph — so the primary action stays discoverable
            // without competing with the gauge. Typing ARMS it:
            // primary fill with a 0.96→1 spring pop (slight overshoot),
            // the clearest "you can send now" signal.
            Button(action: send) {
                Image(systemName: "arrow.up")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(canSend ? Theme.onAccent : Theme.muted)
                    .frame(width: 32, height: 32)
                    .background(
                        canSend ? (sendHovered ? Theme.primaryHover : Theme.primary) : Theme.bg2,
                        in: Circle(),
                    )
                    .scaleEffect(canSend || reduceMotion ? 1 : 0.96)
            }
            .buttonStyle(.plain)
            .onHover { sendHovered = $0 }
            .animation(reduceMotion ? nil : .spring(response: 0.3, dampingFraction: 0.65), value: canSend)
            .animation(.easeInOut(duration: 0.12), value: sendHovered)
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
            PickerCapsule(
                icon: "square.3.layers.3d",
                label: store.modelName.isEmpty ? "model" : store.modelName,
                isActive: false,
                help: "Switch model",
            ) { close in
                ForEach(models, id: \.self) { model in
                    let ref = "\(model.provider)/\(model.name)"
                    PickerMenuItem(
                        // Always qualify with the provider: a bare name hides
                        // ownership, and conditional qualification flips form
                        // whenever a duplicate name appears or disappears.
                        title: "\(model.provider) / \(model.name)",
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
            icon: "lightbulb",
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
    /// icon; non-default modes expand a short name with an amber tint —
    /// a deliberate "guardrails changed" signal, with the shield glyph
    /// spelling out HOW (full / half / slashed for ask-more to ask-never).
    private var approvalPicker: some View {
        PickerCapsule(
            icon: ApprovalOption(rawValue: store.approvalMode)?.icon ?? "shield",
            label: store.approvalMode == "on-request"
                ? nil
                : (ApprovalOption(rawValue: store.approvalMode)?.short ?? store.approvalMode),
            isWarn: store.approvalMode != "on-request",
            help: "Switch approval baseline (workspace-level; takes effect next turn)",
        ) { close in
            ForEach(ApprovalOption.all, id: \.rawValue) { option in
                // One name per mode in the UI: the friendly short name
                // (standard/dev/auto); the config value stays in the
                // tooltip. The hint rides a second line (the WebUI's
                // .menu-item-hint) — as a trailing detail it ragged
                // against the varying title widths.
                PickerMenuItem(
                    title: option.short,
                    desc: option.hint,
                    isCurrent: store.approvalMode == option.rawValue,
                    help: "config value: \(option.rawValue)",
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
        ReasoningOption(value: "auto", label: "Auto (plan high, act low)"),
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

    /// The capsule's shield state mirrors how much the mode asks.
    var icon: String {
        switch self {
        case .onRequest: "shield"
        case .dangerOnly: "shield.lefthalf.filled"
        case .never: "shield.slash"
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
/// the right, ✓ for the current value. A long description belongs on a
/// second line (`desc`, the WebUI's .menu-item-hint) — as trailing
/// `detail` it rags against the title column.
private struct PickerMenuItem: View {
    let title: String
    var detail: String?
    var desc: String?
    let isCurrent: Bool
    var help: String?
    let action: () -> Void

    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    Text(title)
                        .font(.system(size: Theme.textMd))
                        .foregroundStyle(isCurrent ? Theme.primary : Theme.fg)
                    Spacer()
                    if let detail {
                        Text(detail)
                            .font(.system(size: 11))
                            .foregroundStyle(Theme.hint)
                    }
                    Image(systemName: "checkmark")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(Theme.primary)
                        .opacity(isCurrent ? 1 : 0)
                }
                if let desc {
                    Text(desc)
                        .font(.system(size: 11))
                        .foregroundStyle(Theme.hint)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
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
        .help(help ?? "")
    }
}

// MARK: - Plan panel

/// A live progress rail rather than a todo card. The header has two jobs:
/// collapsed it is the "now playing" line — breathing node + current step
/// + its ticking elapsed time + n/N, no card chrome; expanded it switches
/// to the plan title and the timeline unfolds beneath, its left rail
/// doubling as the progress bar (done segments solid, the active segment
/// primary, upcoming dashed) with each node occluding the rail via an
/// opaque base. The current step lives in exactly one place per state;
/// a brief row flash guides the eye to it on unfold. The first appearance
/// shows the whole plan and folds into the rail after a few seconds;
/// completed plans linger briefly, then retire.
struct PlanPanel: View {
    let plan: PlanPayload

    @State private var expanded = true
    @State private var retiring = false
    @State private var retired = false
    @State private var hovered = false
    @State private var retirementTask: Task<Void, Never>?
    @State private var autoCollapseTask: Task<Void, Never>?
    /// Row flash guiding the eye from the header text to the highlighted
    /// active row when the list unfolds.
    @State private var flashedStep: Int?
    @State private var flashTask: Task<Void, Never>?
    /// When the current step started (the elapsed readout's base) —
    /// re-anchored on every step change.
    @State private var currentSince = Date()
    /// Panel birth / completion: the base of the "total" line.
    @State private var startedAt = Date()
    @State private var finishedAt: Date?

    private var items: [PlanPayload.Item] {
        plan.items
    }

    private var doneCount: Int {
        items.filter { Self.renderStatus($0.status) == "done" }.count
    }

    private var currentIndex: Int? {
        items.firstIndex { $0.status == "in_progress" }
    }

    private var allDone: Bool {
        doneCount == items.count
    }

    /// The wire status renamed todo/done → pending/completed; pre-rename
    /// sessions replay the legacy names, so accept both, mapping onto the
    /// panel's internal rendering vocabulary (done/in_progress/todo).
    private static func renderStatus(_ status: String) -> String {
        switch status {
        case "completed", "done": "done"
        case "in_progress": "in_progress"
        default: "todo"
        }
    }

    var body: some View {
        Group {
            if !retired {
                VStack(alignment: .leading, spacing: 0) {
                    headerRow

                    if expanded {
                        itemList
                            .transition(.opacity)
                    }
                }
                // Chrome follows intent: none in the steady state (a soft
                // hover wash only), a stitched card while inspecting.
                .background {
                    if expanded {
                        RoundedRectangle(cornerRadius: Theme.radiusLg).fill(Theme.bg1)
                    } else if hovered {
                        RoundedRectangle(cornerRadius: Theme.radiusMd).fill(Theme.bg2.opacity(0.35))
                    }
                }
                .overlay {
                    if expanded {
                        RoundedRectangle(cornerRadius: Theme.radiusLg)
                            .strokeBorder(Theme.bg2.opacity(0.65), lineWidth: 1)
                    }
                }
                .clipShape(RoundedRectangle(cornerRadius: expanded ? Theme.radiusLg : Theme.radiusMd))
                .opacity(retiring ? 0 : 1)
                .offset(y: retiring ? 3 : 0)
                .animation(.easeInOut(duration: 0.3), value: retiring)
            }
        }
        .onAppear {
            if allDone {
                expanded = false
                finishedAt = Date()
            }
            scheduleAutoCollapse()
            updateRetirement()
        }
        .onChange(of: allDone) { _, done in
            if done {
                finishedAt = Date()
                withAnimation(.easeInOut(duration: 0.18)) { expanded = false }
            }
            updateRetirement()
        }
        .onChange(of: currentIndex) { _, _ in currentSince = Date() }
        .onChange(of: expanded) { _, isExpanded in
            autoCollapseTask?.cancel()
            if isExpanded, let currentIndex {
                flashTask?.cancel()
                flashedStep = currentIndex
                flashTask = Task {
                    try? await Task.sleep(for: .milliseconds(120))
                    guard !Task.isCancelled else { return }
                    withAnimation(.easeOut(duration: 0.7)) { flashedStep = nil }
                }
            }
            updateRetirement()
        }
        .onDisappear {
            retirementTask?.cancel()
            autoCollapseTask?.cancel()
            flashTask?.cancel()
        }
    }

    // MARK: Header (steady state: the one "now playing" line)

    /// Two jobs, two texts: collapsed = "now playing" (current step),
    /// expanded = summary (plan title) — the current step then lives ONLY
    /// in the highlighted list row, never duplicated in the header.
    private var headerText: String {
        if expanded {
            let title = plan.title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return title.isEmpty ? "Plan" : title
        }
        if allDone {
            return "All done · \(items.count) steps"
        }
        if let currentIndex {
            return items[currentIndex].goal
        }
        return items.first?.goal ?? "Plan"
    }

    private var headerTint: Color {
        if expanded {
            return Theme.fg
        }
        if allDone {
            return Theme.success
        }
        return currentIndex == nil ? Theme.muted : Theme.fg
    }

    private var headerRow: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.18)) { expanded.toggle() }
        } label: {
            HStack(spacing: 10) {
                PlanStepNode(status: allDone ? "done" : currentIndex != nil ? "in_progress" : "todo")
                    .frame(width: 16, height: 16)
                    .frame(width: 18)
                Text(headerText)
                    .font(.system(size: Theme.textSm,
                                  weight: expanded || allDone ? .semibold : .medium))
                    .foregroundStyle(headerTint)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    // Step advance = a departure-board flip, not a silent swap.
                    .id(headerText)
                    .transition(.opacity)
                if !expanded, currentIndex != nil, !allDone {
                    PlanElapsedText(since: currentSince)
                }
                if !expanded, allDone, let finishedAt {
                    Text("Total \(mazeFormatDur(max(0, finishedAt.timeIntervalSince(startedAt))))")
                        .font(.system(size: Theme.textXs, design: .monospaced))
                        .foregroundStyle(Theme.muted)
                        .monospacedDigit()
                }
                Text("\(doneCount)/\(items.count)")
                    .font(.system(size: Theme.textXs, weight: .medium, design: .monospaced))
                    .foregroundStyle(allDone ? Theme.success : Theme.muted)
                    .monospacedDigit()
                Image(systemName: "chevron.down")
                    .font(.system(size: 9, weight: .medium))
                    .foregroundStyle(Theme.muted)
                    .rotationEffect(.degrees(expanded ? 0 : -90))
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover {
            hovered = $0
            if $0 {
                autoCollapseTask?.cancel() // reading the plan: don't fold it away
            }
        }
        .animation(.easeInOut(duration: 0.18), value: currentIndex)
        .accessibilityLabel("\(headerText), \(doneCount) of \(items.count) steps complete")
        .accessibilityHint(expanded ? "Collapse plan" : "Show plan steps")
    }

    // MARK: Expanded timeline (rail = progress bar)

    private var itemRows: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                HStack(spacing: 10) {
                    PlanStepNode(status: Self.renderStatus(item.status), base: Theme.bg1)
                        .frame(width: 16, height: 16)
                        .frame(width: 18)
                    Text(item.goal)
                        .font(.system(
                            size: Theme.textSm,
                            weight: item.status == "in_progress" ? .medium : .regular,
                        ))
                        .foregroundStyle(item.status == "in_progress" ? Theme.fg : Theme.muted)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    if item.status == "in_progress" {
                        PlanElapsedText(since: currentSince)
                    }
                }
                .frame(height: 24)
                .background(
                    flashedStep == index ? Theme.primary.opacity(0.12) : Color.clear,
                    in: RoundedRectangle(cornerRadius: Theme.radiusSm),
                )
                .background(alignment: .topLeading) {
                    // Continuous center-to-center segment (rows are
                    // contiguous 24pt, so node centers are 24 apart); each
                    // node's opaque base occludes the rail where they meet.
                    if index < items.count - 1 {
                        PlanRailLine(status: Self.renderStatus(item.status))
                            .frame(width: 2, height: 24)
                            .offset(x: 8, y: 12)
                    }
                }
                .id(index)
            }
        }
        .animation(.easeOut(duration: 0.5), value: flashedStep)
        .background(alignment: .topLeading) {
            // Trunk stitching the header node to the first row, colored by
            // the progress INTO step 1. The 7pt above the list top reaches
            // into the header's bottom padding (the node ends there); in
            // the scrolling variant that part clips at the viewport — the
            // rail simply continues out of view.
            PlanRailLine(status: Self.renderStatus(items.first?.status ?? "pending"))
                .frame(width: 2, height: 11)
                .offset(x: 8, y: -7)
        }
    }

    @ViewBuilder
    private var itemList: some View {
        // Leading inset aligns the rail with the header's node column
        // (header padding 10 + node column 18 ⇒ node center x = 19).
        if items.count > 6 {
            ScrollViewReader { proxy in
                ScrollView { itemRows }
                    .frame(maxHeight: 168) // ~7 rows
                    .onAppear {
                        if let currentIndex {
                            proxy.scrollTo(currentIndex, anchor: .center)
                        }
                    }
            }
            .padding(.horizontal, 10)
            .padding(.bottom, 10)
        } else {
            itemRows
                .padding(.horizontal, 10)
                .padding(.bottom, 10)
        }
    }

    // MARK: Lifecycle

    /// The first appearance shows the full plan so the user can vet what
    /// the agent intends to do, then folds into the single-line rail.
    private func scheduleAutoCollapse() {
        guard !allDone else { return }
        autoCollapseTask?.cancel()
        autoCollapseTask = Task {
            do {
                try await Task.sleep(for: .seconds(6))
                guard !Task.isCancelled, !hovered, !allDone else { return }
                withAnimation(.easeInOut(duration: 0.22)) { expanded = false }
            } catch {
                // Toggled or hovered first: the user owns the state now.
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

/// Timeline node: done pops a checkmark in (spring), the active node
/// breathes (the agent-liveness signal), upcoming steps are quiet hollow
/// rings.
private struct PlanStepNode: View {
    let status: String
    /// Opaque disc behind the node: list rows pass it so the continuous
    /// rail terminates visually at the node instead of showing through
    /// the hollow rings (the trace dot's trick). The header node needs
    /// none — no rail crosses it.
    var base: Color?

    @State private var breathing = false

    var body: some View {
        ZStack {
            if let base {
                Circle().fill(base)
            }
            switch status {
            case "done":
                Circle().fill(Theme.success)
                Image(systemName: "checkmark")
                    .font(.system(size: 8, weight: .bold))
                    .foregroundStyle(Theme.onAccent)
                    .transition(.scale(scale: 0.2).combined(with: .opacity))
            case "in_progress":
                Circle().strokeBorder(Theme.primary.opacity(0.7), lineWidth: 1.5)
                Circle()
                    .fill(Theme.primary)
                    .frame(width: 6, height: 6)
                    .scaleEffect(breathing ? 1.25 : 0.8)
                    .opacity(breathing ? 1 : 0.55)
            default:
                Circle().strokeBorder(Theme.muted.opacity(0.42), lineWidth: 1)
            }
        }
        .animation(.spring(duration: 0.34, bounce: 0.45), value: status)
        .onAppear { startBreathing(if: status) }
        .onChange(of: status) { _, new in
            if new == "in_progress" {
                startBreathing(if: new)
            } else {
                breathing = false
            }
        }
    }

    private func startBreathing(if status: String) {
        guard status == "in_progress", !breathing else { return }
        withAnimation(.easeInOut(duration: 1.6).repeatForever(autoreverses: true)) {
            breathing = true
        }
    }
}

/// One rail segment between adjacent step nodes; the rail as a whole is
/// the progress bar: done = success, active = primary, upcoming = dashed.
private struct PlanRailLine: View {
    let status: String

    var body: some View {
        switch status {
        case "done":
            Capsule().fill(Theme.success.opacity(0.55))
        case "in_progress":
            Capsule().fill(Theme.primary.opacity(0.55))
        default:
            Canvas { ctx, size in
                var path = Path()
                path.move(to: CGPoint(x: size.width / 2, y: 0))
                path.addLine(to: CGPoint(x: size.width / 2, y: size.height))
                ctx.stroke(
                    path, with: .color(Theme.bg3),
                    style: StrokeStyle(lineWidth: 1.5, dash: [2, 3]),
                )
            }
        }
    }
}

/// Ticking "time on this step" readout — the strongest "not stuck" signal.
private struct PlanElapsedText: View {
    let since: Date

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            Text(mazeFormatDur(max(0, context.date.timeIntervalSince(since))))
                .font(.system(size: Theme.textXs, design: .monospaced))
                .foregroundStyle(Theme.muted)
                .monospacedDigit()
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

/// A quiet 28px ring (in a 32px layout frame, so hit/hover area and
/// bar geometry don't move) that starts showing the percentage inside
/// at 40%, warms to amber at 60% and to red at the compact-trigger
/// ratio (≈80%). The ring is deliberately smaller than the 32px send
/// button: it is ambient information, not a peer action, and a
/// twin-sized circle would claim equal visual rank.
struct CtxGauge: View {
    let fraction: Double
    var hotThreshold = 0.8

    private static let warm = 0.6
    /// Below this the arc alone carries the value (a number would be
    /// noise at 2% anyway); from here up the percentage rides inside
    /// the ring — earlier than the amber level, so the exact figure
    /// is on screen before compaction looms.
    private static let showPercent = 0.4
    /// Ring geometry: 28px outer diameter, 2.5px stroke. SwiftUI
    /// strokes CENTER on the shape's path, so the circle itself is
    /// drawn at (outer - lineWidth) to land the stroke's outer edge
    /// exactly on 28px instead of spilling 1.25px past it.
    private static let ringOuter: CGFloat = 28
    private static let ringLine: CGFloat = 2.5
    /// Below this the arc would be sub-pixel noise; the bare track
    /// already says "gauge, idle".
    private static let arcFloor = 0.005
    /// Below this the arc draws with a BUTT cap — a short tick flush
    /// with the track, a deliberate "dial just started" mark. A round
    /// cap at these trims renders as a stray floating DOT instead
    /// (a loading-artifact look, not information).
    private static let tickCeiling = 0.06

    var body: some View {
        ZStack {
            // bg3, not bg2: the track must read as "a dial, currently
            // empty" even with no arc — bg2 on the composer's bg1 is
            // nearly invisible and the gauge reads as missing.
            Circle()
                .stroke(Theme.bg3, lineWidth: Self.ringLine)
            // Near-zero occupancy: hide the arc outright (see arcFloor);
            // tiny occupancy draws as a tick (see tickCeiling).
            if fraction >= Self.arcFloor {
                Circle()
                    .trim(from: 0, to: min(max(fraction, 0), 1))
                    .stroke(ringColor, style: StrokeStyle(
                        lineWidth: Self.ringLine,
                        lineCap: fraction < Self.tickCeiling ? .butt : .round,
                    ))
                    .rotationEffect(.degrees(-90))
                    .animation(.easeOut(duration: 0.35), value: fraction)
            }
            if fraction >= Self.showPercent {
                Text("\(Int(fraction * 100))%")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(ringColor)
                    // Ø21 inner: "100%" only fits scaled down a notch.
                    .minimumScaleFactor(0.7)
                    .lineLimit(1)
            }
        }
        .frame(width: Self.ringOuter - Self.ringLine, height: Self.ringOuter - Self.ringLine)
        .frame(width: 32, height: 32)
    }

    private var ringColor: Color {
        if fraction >= hotThreshold {
            return Theme.error
        }
        if fraction >= Self.warm {
            return Theme.warning
        }
        return Theme.gaugeIdle
    }
}
