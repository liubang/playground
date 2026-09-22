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

/// The settings panel (WebUI SettingsPanel): a modal config.yaml
/// graphical editor — header (title + config path + close), left tab
/// nav, spec-driven content, and a footer with the status line and
/// reload/close/save. Closing with unsaved edits confirms first.
struct SettingsView: View {
    @Bindable var store: SettingsStore
    let onClose: () -> Void

    /// The destructive action waiting on the discard confirmation
    /// (WebUI confirmDialog): closing the panel, or reloading and
    /// throwing away the draft.
    private enum DiscardAction: Identifiable {
        case close
        case reload

        var id: Int {
            switch self {
            case .close: 0
            case .reload: 1
            }
        }
    }

    @State private var pendingDiscard: DiscardAction?

    var body: some View {
        VStack(spacing: 0) {
            header
            Hairline(axis: .horizontal)
            bodyContent
            Hairline(axis: .horizontal)
            footer
        }
        .frame(minWidth: 880, idealWidth: 940, minHeight: 600, idealHeight: 660)
        .background(Theme.bg0)
        .task { await store.load() }
        .confirmationDialog(
            "You have unsaved changes. They will be lost.",
            isPresented: Binding(
                get: { pendingDiscard != nil },
                set: {
                    if !$0 {
                        pendingDiscard = nil
                    }
                },
            ),
            titleVisibility: .visible,
        ) {
            Button("Discard Changes", role: .destructive) {
                switch pendingDiscard {
                case .close:
                    onClose()
                case .reload:
                    Task { await store.load() }
                case nil:
                    break
                }
                pendingDiscard = nil
            }
            Button("Keep Editing", role: .cancel) { pendingDiscard = nil }
        }
    }

    // MARK: Header (.settings-head)

    private var header: some View {
        HStack(spacing: 10) {
            Text("Settings")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Theme.fg)
            Text(store.cfgPath)
                .font(Theme.monoSm)
                .foregroundStyle(Theme.muted)
                .lineLimit(1)
                .truncationMode(.middle)
                .help(store.cfgPath)
            Spacer()
            GhostButton(action: attemptClose) {
                Image(systemName: "xmark")
                    .font(.system(size: 12, weight: .medium))
            }
            .help("Close (Esc)")
            .accessibilityLabel("Close settings")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    // MARK: Body (tab nav + content)

    private var bodyContent: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(settingsTabs, id: \.id) { tab in
                    Button {
                        store.activeTab = tab.id
                    } label: {
                        HStack(spacing: 8) {
                            Image(systemName: tab.icon)
                                .font(.system(size: 12))
                                .frame(width: 16)
                            Text(tab.label)
                                .font(.system(size: Theme.textMd))
                            Spacer()
                        }
                        .foregroundStyle(store.activeTab == tab.id ? Theme.primary : Theme.fg)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 7)
                        .background(
                            store.activeTab == tab.id ? Theme.bg2 : Color.clear,
                            in: RoundedRectangle(cornerRadius: Theme.radiusMd),
                        )
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
                Spacer()
            }
            .padding(10)
            .frame(width: 168)
            .background(Theme.bg1)

            Hairline(axis: .vertical)

            switch store.loadState {
            case .idle, .loading:
                VStack(spacing: 10) {
                    ProgressView()
                    Text("Loading configuration…")
                        .font(.system(size: Theme.textMd))
                        .foregroundStyle(Theme.muted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            case let .failed(error):
                VStack(spacing: 10) {
                    Label("Failed to load configuration: \(error)", systemImage: "exclamationmark.triangle")
                        .font(.system(size: Theme.textMd))
                        .foregroundStyle(Theme.error)
                    Button("Retry") { Task { await store.load() } }
                        .buttonStyle(OutlineButtonStyle())
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            case .loaded:
                tabContent
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    /// Mount on demand: only the active tab renders (WebUI
    /// mount-on-demand; the draft lives in the store and survives).
    private var tabContent: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    switch store.activeTab {
                    case "providers":
                        ProvidersTabView(store: store)
                    case "mcp":
                        McpTabView(store: store)
                    case "skills":
                        SkillsTabView(store: store)
                    default:
                        if let spec = settingsTabs.first(where: { $0.id == store.activeTab }),
                           let sections = spec.sections
                        {
                            SectionsTabView(sections: sections, store: store)
                            if spec.id == "permission" {
                                RulePacksView(store: store)
                            }
                            if spec.id == "system" {
                                SystemExtrasView(store: store)
                            }
                        }
                    }
                }
                .padding(18)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .onChange(of: store.invalid) { _, fieldId in
                // WebUI locate: scroll the failing field into view.
                guard let fieldId else { return }
                withAnimation {
                    proxy.scrollTo(fieldId, anchor: .center)
                }
            }
        }
    }

    // MARK: Footer (.settings-foot)

    private var footer: some View {
        HStack(spacing: 10) {
            Text(store.msg)
                .font(.system(size: Theme.textSm))
                .foregroundStyle(store.msgIsError ? Theme.error : Theme.muted)
                .lineLimit(2)
                .truncationMode(.tail)
                .help(store.msg)
            Spacer()
            Button("Reload") {
                if store.dirty {
                    pendingDiscard = .reload
                } else {
                    Task { await store.load() }
                }
            }
            .buttonStyle(OutlineButtonStyle())
            Button("Close", action: attemptClose)
                .buttonStyle(OutlineButtonStyle())
                .keyboardShortcut(.cancelAction)
            Button("Save") { Task { await store.save() } }
                .buttonStyle(PrimaryButtonStyle())
                .disabled(store.saving)
                .overlay(
                    RoundedRectangle(cornerRadius: Theme.radiusMd)
                        .strokeBorder(
                            store.flashSave ? Theme.success
                                : (store.dirty ? Theme.warning : Color.clear),
                            lineWidth: 1.5,
                        ),
                )
                .help(store.dirty ? "Unsaved changes" : "Save to config.yaml")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    private func attemptClose() {
        if store.dirty {
            pendingDiscard = .close
        } else {
            onClose()
        }
    }
}

// MARK: - Sections tab (spec-driven, WebUI SectionsTab)

struct SectionsTabView: View {
    let sections: [(String, [FieldSpec])]
    @Bindable var store: SettingsStore

    var body: some View {
        ForEach(Array(sections.enumerated()), id: \.offset) { _, section in
            VStack(alignment: .leading, spacing: 10) {
                Text(section.0)
                    .font(.system(size: Theme.textMd, weight: .semibold))
                    .foregroundStyle(Theme.fg)
                ForEach(section.1, id: \.key) { spec in
                    FieldRow(
                        spec: spec,
                        value: store.draft.globals[spec.key] ?? spec.emptyState,
                        invalid: store.invalid == spec.key,
                        onChange: { store.setGlobal(spec.key, $0) },
                        onReveal: spec.revealRef.map { ref in
                            { await store.reveal(ref) }
                        },
                    )
                    .id(spec.key)
                }
            }
        }
    }
}

extension FieldSpec {
    /// Initial control state (SettingsPanel emptyState).
    var emptyState: ControlState {
        type == .bool || type == .flagList ? .flag(false) : .text("")
    }
}

// MARK: - Field row (WebUI FieldRow — one control per spec type)

struct FieldRow: View {
    let spec: FieldSpec
    let value: ControlState
    var invalid = false
    let onChange: (ControlState) -> Void
    var onReveal: (() async -> String?)?

    @State private var revealed: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                if let label = spec.label {
                    HStack(spacing: 4) {
                        Text(label)
                            .font(.system(size: Theme.textMd))
                            .foregroundStyle(Theme.fg)
                        if spec.required {
                            Text("*")
                                .font(.system(size: Theme.textMd))
                                .foregroundStyle(Theme.error)
                        }
                        if let def = spec.def {
                            Text("default: \(def)")
                                .font(.system(size: Theme.textXs))
                                .foregroundStyle(Theme.muted)
                        }
                    }
                    .frame(width: 168, alignment: .leading)
                }
                control
            }
            if let hint = effectiveHint {
                Text(hint)
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .padding(.leading, spec.label == nil ? 0 : 176)
            }
        }
    }

    /// The select type's per-option explanation wins over the static
    /// hint (WebUI optionHints).
    private var effectiveHint: String? {
        if spec.type == .select, let hints = spec.optionHints {
            return hints[value.textValue] ?? spec.hint
        }
        return spec.hint
    }

    @ViewBuilder private var control: some View {
        switch spec.type {
        case .bool, .flagList:
            Toggle("", isOn: flagBinding)
                .toggleStyle(.switch)
                .labelsHidden()
        case .tristate:
            Picker("", selection: textBinding) {
                Text("On").tag("true")
                Text("Off").tag("false")
                Text("Auto").tag("")
            }
            .pickerStyle(.segmented)
            .frame(width: 180)
        case .select:
            Picker("", selection: textBinding) {
                ForEach(spec.options ?? [], id: \.0) { option in
                    Text(option.1).tag(option.0)
                }
            }
            .pickerStyle(.menu)
            .frame(maxWidth: 380, alignment: .leading)
        case .textarea, .listText, .kvText, .pairList:
            multiline(text: textBinding, rows: spec.rows ?? (spec.type == .textarea ? 4 : 3))
        case .password:
            passwordControl
        default:
            input(TextField(spec.ph ?? "", text: textBinding))
        }
    }

    /// Masked secret field: the eye button fetches the plaintext on
    /// demand and swaps the editor for a read-only reveal (WebUI
    /// reveal button).
    private var passwordControl: some View {
        HStack(spacing: 6) {
            if let revealed {
                input(TextField("", text: .constant(revealed)).disabled(true))
            } else {
                input(SecureField(spec.ph ?? "", text: textBinding))
            }
            if onReveal != nil {
                GhostButton {
                    if revealed != nil {
                        revealed = nil
                    } else {
                        Task { revealed = await onReveal?() }
                    }
                } label: {
                    Image(systemName: revealed == nil ? "eye" : "eye.slash")
                        .font(.system(size: 11))
                }
                .help(revealed == nil ? "Reveal the saved secret" : "Hide")
            }
        }
    }

    private func input(_ field: some View) -> some View {
        field
            .textFieldStyle(.plain)
            .font(.system(size: Theme.textMd))
            .padding(.horizontal, 9)
            .padding(.vertical, 6)
            .frame(maxWidth: 380)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(invalid ? Theme.error : Theme.bg2, lineWidth: 1),
            )
    }

    private func multiline(text: Binding<String>, rows: Int) -> some View {
        TextEditor(text: text)
            .font(.system(size: Theme.textMd))
            .lineSpacing(2)
            .scrollContentBackground(.hidden)
            .padding(5)
            .frame(height: CGFloat(rows) * 20 + 12)
            .frame(maxWidth: 520)
            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(invalid ? Theme.error : Theme.bg2, lineWidth: 1),
            )
    }

    private var textBinding: Binding<String> {
        Binding(
            get: { value.textValue },
            set: { onChange(.text($0)) },
        )
    }

    private var flagBinding: Binding<Bool> {
        Binding(
            get: { value.flagValue },
            set: { onChange(.flag($0)) },
        )
    }
}

// MARK: - Card chrome shared by the custom tabs

/// One collapsible card (WebUI .set-card): the header shows the
/// summary line and the delete action; expanding reveals the fields.
struct SettingsCard<Header: View, Content: View>: View {
    let isOpen: Bool
    let onToggle: () -> Void
    @ViewBuilder var header: Header
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(Theme.muted)
                    .rotationEffect(.degrees(isOpen ? 90 : 0))
                header
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .contentShape(Rectangle())
            .onTapGesture(perform: onToggle)

            if isOpen {
                VStack(alignment: .leading, spacing: 10) {
                    content
                }
                .padding(.horizontal, 14)
                .padding(.bottom, 12)
                .padding(.top, 2)
            }
        }
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: 10))
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }
}

/// Section title shared by the custom tabs (.set-sec-title).
struct SettingsSectionTitle: View {
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.system(size: Theme.textMd, weight: .semibold))
            .foregroundStyle(Theme.fg)
    }
}

/// "+ 添加 xxx" secondary button (WebUI .btn-secondary.btn-sm.set-add).
struct SettingsAddButton: View {
    let title: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Label(title, systemImage: "plus")
                .font(.system(size: Theme.textSm, weight: .medium))
        }
        .buttonStyle(OutlineButtonStyle())
    }
}
