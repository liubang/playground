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

    var body: some View {
        VStack(spacing: 0) {
            header
            Hairline(axis: .horizontal)
            bodyContent
            Hairline(axis: .horizontal)
            footer
        }
        .frame(minWidth: 720, idealWidth: 860, maxWidth: 960,
               minHeight: 480, idealHeight: 640, maxHeight: 800)
        .background(Theme.bg1)
        // Toasts surface above the sheet (the window-level host sits
        // underneath it). Clears the header row, like the WebUI's
        // #toasts top: 56px.
        .overlay(alignment: .topTrailing) {
            ToastHost()
                .padding(.top, 52)
                .padding(.trailing, 16)
        }
        // The sheet-level confirm-dialog host: it registers AFTER the
        // window root's, so it renders the dialog while the sheet is up
        // (ConfirmCenter: topmost host wins).
        .overlay { ConfirmDialogHost() }
        .environment(
            \.secretIdentity,
            [store.revision, String(store.dirty)] + store.draft.providers.flatMap { card in
                [card.id.uuidString, card.fields["name"]?.textValue ?? ""]
            },
        )
        .task { await store.load() }
    }

    /// Dirty-state guard for close/reload: confirm via the in-app
    /// dialog, then run the pending action.
    private func confirmDiscard(_ action: DiscardAction) {
        ConfirmCenter.shared.ask(ConfirmRequest(
            title: "放弃未保存的修改？",
            message: "这些修改将会丢失。",
            confirmTitle: "放弃修改",
            cancelTitle: "继续编辑",
        ) {
            switch action {
            case .close:
                onClose()
            case .reload:
                Task { await store.load(manual: true) }
            }
        })
    }

    // MARK: Header (.settings-head: compact title + path + close)

    private var header: some View {
        HStack(spacing: 10) {
            Text("设置")
                .font(.system(size: Theme.textLg, weight: .semibold))
                .foregroundStyle(Theme.fg)
            Text(store.cfgPath)
                .font(Theme.monoXs)
                .foregroundStyle(Theme.muted)
                .lineLimit(1)
                .truncationMode(.middle)
                .help(store.cfgPath)
            Spacer(minLength: 12)
            GhostButton(action: attemptClose) {
                Image(systemName: "xmark")
                    .font(.system(size: 12, weight: .semibold))
            }
            .help("关闭 (Esc)")
            .accessibilityLabel("关闭设置")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    // MARK: Body (tab nav + content)

    private var bodyContent: some View {
        HStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(settingsTabs, id: \.id) { tab in
                        SettingsTabButton(tab: tab, active: store.activeTab == tab.id) {
                            store.activeTab = tab.id
                        }
                    }
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 12)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(width: 132)

            Hairline(axis: .vertical)

            switch store.loadState {
            case .idle, .loading:
                VStack(spacing: 10) {
                    ProgressView()
                    Text("正在加载配置…")
                        .font(.system(size: Theme.textMd))
                        .foregroundStyle(Theme.muted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            case let .failed(error):
                VStack(spacing: 10) {
                    Label("配置加载失败：\(error)", systemImage: "exclamationmark.triangle")
                        .font(.system(size: Theme.textMd))
                        .foregroundStyle(Theme.error)
                    Button("重试") { Task { await store.load() } }
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
            ScrollView(.vertical) {
                VStack(alignment: .leading, spacing: 12) {
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
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 20)
                .padding(.vertical, 16)
            }
            .id(store.activeTab)
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
            Button {
                if store.dirty {
                    confirmDiscard(.reload)
                } else {
                    Task { await store.load(manual: true) }
                }
            } label: {
                Label("重新加载", systemImage: "arrow.clockwise")
            }
            .buttonStyle(SettingsSecondaryButtonStyle())
            Spacer(minLength: 8)
            // .settings-msg: width-capped, right-aligned against the buttons.
            if !store.msg.isEmpty {
                Text(store.msg)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(store.msgIsError ? Theme.error : Theme.muted)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .help(store.msg)
                    .frame(maxWidth: 320, alignment: .trailing)
            }
            Button("取消", action: attemptClose)
                .buttonStyle(SettingsSecondaryButtonStyle())
                .keyboardShortcut(.cancelAction)
            Button { Task { await store.save() } } label: {
                Label("保存设置", systemImage: "checkmark")
            }
            .buttonStyle(PrimaryButtonStyle())
            .disabled(store.saving)
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm + 2)
                    .strokeBorder(
                        store.flashSave ? Theme.success
                            : (store.dirty ? Theme.warning : Color.clear),
                        lineWidth: 2,
                    )
                    .padding(-2),
            )
            .help(store.dirty ? "有未保存的修改" : "保存到 config.yaml")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private func attemptClose() {
        if store.dirty {
            confirmDiscard(.close)
        } else {
            onClose()
        }
    }
}

/// WebUI .settings-tab: muted by default, fg + bg2 wash on hover,
/// primary + semibold when active.
private struct SettingsTabButton: View {
    let tab: TabSpec
    let active: Bool
    let action: () -> Void
    @State private var hovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: tab.icon)
                    .font(.system(size: 12.5, weight: .medium))
                    .frame(width: 16)
                    .opacity(active || hovered ? 1 : 0.85)
                Text(tab.label)
                    .font(.system(size: Theme.textMd, weight: active ? .semibold : .regular))
                Spacer()
            }
            .foregroundStyle(active ? Theme.primary : (hovered ? Theme.fg : Theme.muted))
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(
                active || hovered ? Theme.bg2 : Color.clear,
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovered = $0 }
    }
}

/// ui.css .btn-secondary: transparent with a muted outline; pressed
/// fills fg at 8%.
private struct SettingsSecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Theme.textMd, weight: .medium))
            .foregroundStyle(Theme.fg)
            .padding(.horizontal, 14)
            .padding(.vertical, 7)
            .background(
                Theme.fg.opacity(configuration.isPressed ? 0.08 : 0),
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(Theme.muted, lineWidth: 1),
            )
    }
}

/// ui.css .btn-secondary.btn-sm: the compact variant used by the
/// "+ 添加 xxx" buttons (.set-add).
private struct SettingsSmallSecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Theme.textSm, weight: .medium))
            .foregroundStyle(Theme.fg)
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(
                Theme.fg.opacity(configuration.isPressed ? 0.08 : 0),
                in: RoundedRectangle(cornerRadius: Theme.radiusSm),
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(Theme.muted, lineWidth: 1),
            )
    }
}

// MARK: - Sections tab (spec-driven, WebUI SectionsTab)

struct SectionsTabView: View {
    let sections: [(String, [FieldSpec])]
    @Bindable var store: SettingsStore

    var body: some View {
        ForEach(Array(sections.enumerated()), id: \.offset) { _, section in
            SettingsSection(section.0) {
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

private extension EnvironmentValues {
    @Entry var secretIdentity: [String] = []
}

struct FieldRow: View {
    @Environment(\.secretIdentity) private var secretIdentity
    let spec: FieldSpec
    let value: ControlState
    var invalid = false
    let onChange: (ControlState) -> Void
    var onReveal: (() async -> String?)?

    @State private var revealedSecret: String?
    @State private var revealedValue: ControlState?
    @State private var revealedIdentity: [String] = []
    @State private var revealGeneration = 0
    @State private var revealPending = false

    private var revealed: String? {
        guard revealedValue == value, revealedIdentity == secretIdentity else { return nil }
        return revealedSecret
    }

    static let labelWidth: CGFloat = 172
    static let columnSpacing: CGFloat = 12

    var body: some View {
        HStack(alignment: .top, spacing: Self.columnSpacing) {
            if let label = spec.label {
                VStack(alignment: .leading, spacing: 3) {
                    HStack(alignment: .firstTextBaseline, spacing: 3) {
                        Text(label)
                            .foregroundStyle(Theme.fg)
                        if spec.required {
                            Text("*")
                                .foregroundStyle(Theme.error)
                        }
                    }
                    .font(.system(size: Theme.textMd, weight: .medium))
                    if let def = spec.def {
                        Text("默认：\(def)")
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(Theme.muted)
                    }
                }
                .frame(width: Self.labelWidth, alignment: .leading)
                .padding(.top, 6)
            }
            VStack(alignment: .leading, spacing: 5) {
                control
                    .frame(maxWidth: .infinity, alignment: .leading)
                if let hint = effectiveHint {
                    Text(hint)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .onChange(of: value) { _, _ in hideSecret() }
        .onChange(of: secretIdentity) { _, _ in hideSecret() }
        .onDisappear { hideSecret() }
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
                .tint(Theme.primary)
        case .tristate:
            Picker("", selection: textBinding) {
                Text("开").tag("true")
                Text("关").tag("false")
                Text("自动").tag("")
            }
            .pickerStyle(.segmented)
            .frame(width: 210)
        case .select:
            // .set-input.sel: dropdowns don't span the full row.
            Picker("", selection: textBinding) {
                ForEach(spec.options ?? [], id: \.0) { option in
                    Text(option.1).tag(option.0)
                }
            }
            .pickerStyle(.menu)
            .frame(maxWidth: 320, alignment: .leading)
        case .textarea, .listText, .kvText, .pairList:
            multiline(text: textBinding, rows: spec.rows ?? (spec.type == .textarea ? 4 : 3))
        case .password:
            passwordControl
        default:
            input(TextField(spec.ph ?? "", text: textBinding))
        }
    }

    /// Masked secret field: the eye button fetches the plaintext on
    /// demand and swaps the editor for a selectable reveal (WebUI
    /// reveal button).
    private var passwordControl: some View {
        HStack(spacing: 6) {
            if let revealed {
                input(Text(revealed).textSelection(.enabled).lineLimit(1))
                GhostButton {
                    guard let revealed = self.revealed else { return }
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(revealed, forType: .string)
                } label: {
                    Image(systemName: "doc.on.doc")
                        .font(.system(size: 11))
                }
                .help("复制已保存的密钥")
                .accessibilityLabel("复制已保存的密钥")
            } else {
                input(SecureField(spec.ph ?? "", text: textBinding))
            }
            if onReveal != nil {
                GhostButton {
                    if revealed != nil || revealPending {
                        hideSecret()
                    } else {
                        revealPending = true
                        revealGeneration += 1
                        let generation = revealGeneration
                        let requestedValue = value
                        let requestedIdentity = secretIdentity
                        let reveal = onReveal
                        Task {
                            let secret = await reveal?()
                            guard generation == revealGeneration else { return }
                            revealPending = false
                            guard requestedValue == value, requestedIdentity == secretIdentity else {
                                return
                            }
                            revealedValue = requestedValue
                            revealedIdentity = requestedIdentity
                            revealedSecret = secret
                        }
                    }
                } label: {
                    Image(systemName: revealed == nil ? "eye" : "eye.slash")
                        .font(.system(size: 11))
                }
                .help(revealed == nil ? "查看已保存的密钥" : "隐藏")
                .accessibilityLabel(revealed == nil ? "查看已保存的密钥" : "隐藏密钥")
            }
        }
    }

    private func hideSecret() {
        revealGeneration += 1
        revealPending = false
        revealedSecret = nil
        revealedValue = nil
    }

    private func input(_ field: some View) -> some View {
        SettingsControlShell(invalid: invalid) {
            field
                .textFieldStyle(.plain)
        }
    }

    private func multiline(text: Binding<String>, rows: Int) -> some View {
        SettingsControlShell(invalid: invalid) {
            TextEditor(text: text)
                .lineSpacing(2)
                .scrollContentBackground(.hidden)
                .frame(height: CGFloat(rows) * 20 + 10)
        }
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

/// WebUI .set-input: bg0 field with a bg2 border (radius-sm); the
/// border flips to primary while focused, is-invalid pins it to error.
struct SettingsControlShell<Content: View>: View {
    var invalid = false
    @ViewBuilder var content: Content
    @FocusState private var focused: Bool

    var body: some View {
        content
            .font(.system(size: Theme.textMd))
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.bg0, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
            .overlay(
                RoundedRectangle(cornerRadius: Theme.radiusSm)
                    .strokeBorder(
                        invalid ? Theme.error : (focused ? Theme.primary : Theme.bg2),
                        lineWidth: 1,
                    ),
            )
            .focused($focused)
    }
}

/// One form section as a card with the title floating on the top-left
/// border (WebUI .set-sec-card, fieldset/legend style): the title chip
/// paints over the border to form the "gap". Sections with a corner
/// action (e.g. the environment report's refresh) keep it in the
/// card's top-right corner so it doesn't sit on the border line.
struct SettingsSection<Content: View, Trailing: View>: View {
    let title: String
    @ViewBuilder var content: Content
    @ViewBuilder var trailing: Trailing

    init(_ title: String, @ViewBuilder content: () -> Content) where Trailing == EmptyView {
        self.title = title
        self.content = content()
        trailing = EmptyView()
    }

    init(
        _ title: String,
        @ViewBuilder content: () -> Content,
        @ViewBuilder trailing: () -> Trailing,
    ) {
        self.title = title
        self.content = content()
        self.trailing = trailing()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 16)
        .padding(.top, 16)
        .padding(.bottom, 14)
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
        .overlay(alignment: .topTrailing) {
            trailing
                .padding(.trailing, 8)
                .padding(.top, 6)
        }
        .overlay(alignment: .topLeading) {
            Text(title)
                .font(.system(size: Theme.textMd, weight: .semibold))
                .foregroundStyle(Theme.fg)
                .padding(.horizontal, 6)
                .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                .offset(x: 10, y: -9)
        }
        // Reserve space for the floating title (WebUI margin-top: 10px).
        .padding(.top, 10)
    }
}

/// One collapsible card (WebUI .set-card): bg0 at 45% over the bg1
/// panel with a bg2 border; nested cards (models inside a provider)
/// use a solid bg0 (.set-card.is-nested). The header shows the summary
/// line and the delete action; expanding reveals the fields.
struct SettingsCard<Header: View, Content: View>: View {
    let isOpen: Bool
    var nested = false
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
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .contentShape(Rectangle())
            .onTapGesture(perform: onToggle)

            if isOpen {
                VStack(alignment: .leading, spacing: 10) {
                    content
                }
                .padding(.horizontal, 14)
                .padding(.bottom, 12)
            }
        }
        .background(
            nested ? Theme.bg0 : Theme.bg0.opacity(0.45),
            in: RoundedRectangle(cornerRadius: Theme.radiusMd),
        )
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMd)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }
}

/// Muted uppercase tag on card headers (WebUI .set-card-tag).
struct SettingsCardTag: View {
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.system(size: Theme.textXs, weight: .semibold))
            .foregroundStyle(Theme.muted)
            .tracking(0.5)
            .textCase(.uppercase)
    }
}

/// "+ 添加 xxx" secondary button (WebUI .btn-secondary.btn-sm.set-add).
struct SettingsAddButton: View {
    let title: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Label(title, systemImage: "plus")
        }
        .buttonStyle(SettingsSmallSecondaryButtonStyle())
    }
}
