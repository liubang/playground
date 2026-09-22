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

    var body: some View {
        VStack(spacing: 8) {
            if let plan = store.plan, !plan.items.isEmpty {
                PlanPanel(plan: plan)
                    .frame(maxWidth: Theme.contentWidth)
                    .frame(maxWidth: .infinity)
            }

            if !store.pendingSteers.isEmpty {
                HStack(spacing: 6) {
                    Image(systemName: "text.badge.plus")
                    Text("\(store.pendingSteers.count) steered message(s) queued")
                        .font(.system(size: Theme.textXs))
                    Spacer()
                }
                .foregroundStyle(Theme.warning)
                .padding(.horizontal, 4)
            }

            VStack(spacing: 0) {
                // Textarea (composer.css .composer textarea): the
                // placeholder rides in the editor's own background so it
                // shares the frame — only the editor's internal text
                // inset (~5pt) separates them, keeping the caret and the
                // hint aligned. Caret takes the foreground color (WebUI
                // inherits --fg), not the system accent blue.
                TextEditor(text: $store.composerDraft)
                    .font(.system(size: 14))
                    // Match the app's text color: the default .primary is pure white
                    // in dark mode and read as bold-ish next to the beige transcript.
                    .foregroundStyle(Theme.fg)
                    .lineSpacing(3)
                    .scrollContentBackground(.hidden)
                    .tint(Theme.fg)
                    .focused($focused)
                    .frame(minHeight: 44, maxHeight: 200)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 9)
                    .padding(.vertical, 4)
                    .background(alignment: .topLeading) {
                        if store.composerDraft.isEmpty {
                            // Match the editor's internal text inset
                            // (~5pt leading, ~6pt top) so the caret and
                            // the hint sit on the same first line.
                            Text(placeholder)
                                .foregroundStyle(Theme.muted.opacity(0.7))
                                .font(.system(size: 14))
                                .lineSpacing(3)
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
                            .help(gaugeTooltip)
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
            // composer-box: padding 6px 6px 0 — the textarea's 14px CSS
            // padding lands on top of it, so text starts 20px in.
            .padding(.top, 6)
            .padding(.horizontal, 6)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusXl))
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
        .onAppear { focused = true }
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
                label: store.modelName.isEmpty ? "model" : store.modelName,
                isActive: false,
                help: "Switch model",
            ) { close in
                ForEach(models, id: \.name) { model in
                    let ref = "\(model.provider)/\(model.name)"
                    PickerMenuItem(
                        title: model.name,
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
        !store.composerDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
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
        let prompt = store.composerDraft
        store.composerDraft = ""
        Task {
            // A failed send puts the text back (unless the user has
            // already typed something new) — the message is never lost.
            if await !store.sendPrompt(prompt), store.composerDraft.isEmpty {
                store.composerDraft = prompt
            }
        }
    }

    private func formatContextWindow(_ value: Int) -> String {
        if value >= 1_000_000 {
            return "\(value / 1_000_000)M"
        }
        if value >= 1000 {
            return "\(value / 1000)k"
        }
        return "\(value)"
    }

    /// WebUI CtxGauge title: "Context 19.4k / 1.0M (2%) · Compacts at
    /// ~800k · Compact target 400k · Nominal window 1.0M".
    private var gaugeTooltip: String {
        guard let occupancy = store.occupancy,
              let effective = store.contextWindow, effective > 0
        else { return "Context occupancy" }
        let pct = Int((Double(occupancy) / Double(effective)) * 100)
        var parts = [
            "Context \(formatTokenCount(occupancy)) / \(formatTokenCount(Int64(effective))) (\(pct)%)",
        ]
        if let trigger = store.window?.compactTrigger, trigger > 0 {
            parts.append("Compacts at ~\(formatTokenCount(Int64(trigger)))")
        }
        if let target = store.window?.compactTarget, target > 0 {
            parts.append("Compact target \(formatTokenCount(Int64(target)))")
        }
        if let nominal = store.window?.nominal, nominal > 0, nominal != effective {
            parts.append("Nominal window \(formatTokenCount(Int64(nominal)))")
        }
        return parts.joined(separator: " · ")
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

// MARK: - Plan panel (plan.css — V2 variant A)

/// Bare status row in the steady state (title · progress dots · count ·
/// current goal); expanding stitches header and list into one card.
/// Once everything is done it lingers briefly on "All done ✓" and then
/// retires.
struct PlanPanel: View {
    let plan: PlanPayload

    @State private var expanded = false
    @State private var retiring = false
    @State private var retired = false
    @State private var hovered = false

    private var items: [PlanPayload.Item] {
        plan.items
    }

    private var doneCount: Int {
        items.filter { $0.status == "done" }.count
    }

    private var currentIndex: Int? {
        items.firstIndex { $0.status == "in_progress" }
    }

    private var allDone: Bool {
        doneCount == items.count
    }

    var body: some View {
        if !retired {
            VStack(spacing: 0) {
                summaryRow

                if expanded {
                    itemList
                }
            }
            .font(.system(size: Theme.textSm))
            // Expand stitching (plan.css [open]): summary and list join
            // into ONE card — bg1 fill, 1px bg2 border, shadow-sm;
            // collapsed there is no card chrome at all.
            .background(
                expanded ? Theme.bg1 : Color.clear,
                in: RoundedRectangle(cornerRadius: 10),
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(expanded ? Theme.bg2 : Color.clear, lineWidth: 1),
            )
            .shadow(
                color: expanded ? .black.opacity(0.25) : .clear,
                radius: 4, y: 2,
            )
            .opacity(retiring ? 0 : 1)
            .offset(y: retiring ? 3 : 0)
            .animation(.easeInOut(duration: 0.3), value: retiring)
            .onChange(of: allDone) { _, nowAllDone in
                guard nowAllDone, !expanded else { return }
                Task { await retire() }
            }
        }
    }

    /// summary: ▸ title · ●●●○ 3/7 · In progress: goal
    private var summaryRow: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.18)) { expanded.toggle() }
        } label: {
            HStack(spacing: 9) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(Theme.muted)
                    .rotationEffect(.degrees(expanded ? 90 : 0))
                    .frame(width: 18)

                Text(plan.title ?? "plan")
                    .font(.system(size: Theme.textSm, weight: .semibold))
                    .foregroundStyle(Theme.fg)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: 320, alignment: .leading)
                    .fixedSize()

                // Segmented progress dots.
                HStack(spacing: 3) {
                    ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                        if item.status == "in_progress" {
                            PulsingDot(color: Theme.primary, size: 6)
                        } else {
                            Circle()
                                .fill(item.status == "done"
                                    ? Theme.success
                                    : Theme.muted.opacity(0.28))
                                .frame(width: 6, height: 6)
                        }
                    }
                }

                Text("\(doneCount)/\(items.count)")
                    .font(.system(size: Theme.textXs, weight: allDone ? .bold : .regular))
                    .foregroundStyle(allDone ? Theme.success : Theme.muted)
                    .monospacedDigit()

                if let currentIndex {
                    HStack(spacing: 4) {
                        Text("In progress:")
                            .foregroundStyle(Theme.muted)
                        Text(items[currentIndex].goal)
                            .font(.system(size: Theme.textSm, weight: .medium))
                            .foregroundStyle(Theme.primary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                } else {
                    Text(allDone ? "All done ✓" : "Not started…")
                        .font(.system(size: Theme.textSm, weight: .medium))
                        .foregroundStyle(allDone ? Theme.success : Theme.fg)
                }

                Spacer(minLength: 0)
            }
            .padding(.vertical, 3)
            .padding(.leading, 14)
            .padding(.trailing, 8)
            // Collapsed: the hover wash wraps the row (bg1 65%); when
            // expanded the outer card supplies the chrome.
            .background(
                !expanded && hovered ? Theme.bg1.opacity(0.65) : Color.clear,
                in: RoundedRectangle(cornerRadius: 6),
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }

    private var itemRows: some View {
        VStack(spacing: 3) {
            ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                HStack(spacing: 9) {
                    stepBadge(index: index, status: item.status)
                    Text(item.goal)
                        .foregroundStyle(item.status == "todo" ? Theme.muted : Theme.fg)
                        .font(.system(
                            size: Theme.textSm,
                            weight: item.status == "in_progress" ? .medium : .regular,
                        ))
                        .strikethrough(item.status == "done", color: Theme.muted)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 3)
                .frame(minHeight: 26, alignment: .center)
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 5)
        .padding(.bottom, 8)
    }

    @ViewBuilder
    private var itemList: some View {
        // .pp-list: max-height 220px with its own scroll ONLY when the
        // content can actually overflow — a ScrollView would otherwise
        // stretch to the cap and leave dead space under a short plan.
        if items.count > 6 {
            ScrollView { itemRows }
                .frame(maxHeight: 220)
        } else {
            itemRows
        }
    }

    /// 18px step circle: number (todo), filled + pulsing (in progress),
    /// green check (done).
    private func stepBadge(index: Int, status: String) -> some View {
        ZStack {
            switch status {
            case "done":
                Circle().fill(Theme.success)
                Image(systemName: "checkmark")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundStyle(Theme.onAccent)
            case "in_progress":
                Circle().fill(Theme.primary.opacity(0.18))
                Text("\(index + 1)")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Theme.primary)
            default:
                Circle().strokeBorder(Theme.muted.opacity(0.5), lineWidth: 1)
                Text("\(index + 1)")
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Theme.muted)
            }
        }
        .frame(width: 18, height: 18)
    }

    private func retire() async {
        try? await Task.sleep(for: .milliseconds(2200))
        guard allDone, !expanded else { return }
        retiring = true
        try? await Task.sleep(for: .milliseconds(360))
        retired = true
    }
}

// MARK: - Context gauge (.ctx-gauge)

/// The WebUI's ctx-gauge: a quiet 32px ring that warms to amber at 60%
/// and to red at the compact-trigger ratio (≈80%); the percentage
/// appears inside the ring from the warm level up.
struct CtxGauge: View {
    let fraction: Double
    var hotThreshold = 0.8

    private static let warm = 0.6

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
            if fraction >= Self.warm {
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
