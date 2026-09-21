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

// The custom-rendered settings tabs (WebUI ProvidersTab / McpTab /
// SkillsTab) plus the permission tab's rule packs and the system tab's
// extras (workspaces + dev-environment report). Collapsible cards take
// the place of the WebUI's CSS-driven overview/detail navigation —
// same draft, same summaries (collectFields-based), same validation
// anchors.

// MARK: - Providers (模型)

struct ProvidersTabView: View {
    @Bindable var store: SettingsStore

    @State private var pendingDeleteProvider: ProviderDraft?
    @State private var pendingDeleteModel: (card: ProviderDraft, modelId: UUID)?
    @State private var advancedOpen: Set<UUID> = []

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("启动模型")
            FieldRow(
                spec: defaultModelField,
                value: store.draft.globals[defaultModelField.key] ?? .text(""),
                invalid: store.invalid == defaultModelField.key,
                onChange: { store.setGlobal(defaultModelField.key, $0) },
            )
            .id(defaultModelField.key)
        }

        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("模型提供方（至少一个）")
            ForEach(store.draft.providers) { card in
                providerCard(card)
            }
            SettingsAddButton(title: "添加 provider") { store.addProvider() }
                .id("add-provider")
        }
        .confirmationDialog(
            providerDeleteMessage,
            isPresented: .constant(pendingDeleteProvider != nil),
            titleVisibility: .visible,
        ) {
            Button("删除", role: .destructive) {
                if let card = pendingDeleteProvider {
                    store.deleteProvider(card)
                }
                pendingDeleteProvider = nil
            }
            Button("取消", role: .cancel) { pendingDeleteProvider = nil }
        }
        .confirmationDialog(
            "将删除该模型的配置。保存后生效，未保存前重新加载可恢复。",
            isPresented: .constant(pendingDeleteModel != nil),
            titleVisibility: .visible,
        ) {
            Button("删除", role: .destructive) {
                if let pending = pendingDeleteModel {
                    store.deleteModel(pending.card, modelId: pending.modelId)
                }
                pendingDeleteModel = nil
            }
            Button("取消", role: .cancel) { pendingDeleteModel = nil }
        }
    }

    private var providerDeleteMessage: String {
        guard let card = pendingDeleteProvider else { return "" }
        let name = card.fields["name"]?.textValue.trimmingCharacters(in: .whitespaces)
        return "将删除 provider「\(name?.isEmpty == false ? name! : "未命名")」"
            + (card.models.isEmpty ? "的配置" : "及其下 \(card.models.count) 个模型的配置")
            + "。保存后生效，未保存前重新加载可恢复。"
    }

    // MARK: Provider card

    private func providerCard(_ card: ProviderDraft) -> some View {
        let isOpen = store.openProviderId == card.id
        let summary = providerSummary(card)
        return SettingsCard(
            isOpen: isOpen,
            onToggle: {
                store.openProviderId = isOpen ? nil : card.id
                if !isOpen {
                    store.openModelId = nil
                }
            },
        ) {
            HStack(spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(summary.name)
                        .font(.system(size: Theme.textMd, weight: .semibold))
                        .foregroundStyle(summary.named ? Theme.fg : Theme.muted)
                    Text(summary.meta)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                Spacer()
                Button { pendingDeleteProvider = card } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 11))
                }
                .buttonStyle(GhostButtonStyle())
                .help("删除该 provider")
            }
        } content: {
            FieldRow(
                spec: FieldSpec(
                    "name", label: "名称", ph: "provider 名（全局唯一，必填）", required: true,
                ),
                value: card.fields["name"] ?? .text(""),
                invalid: store.invalid == "\(card.id.uuidString):name",
                onChange: { store.patchProvider(card.id, key: "name", $0) },
            )
            .id("\(card.id.uuidString):name")
            ForEach(providerBaseFields, id: \.key) { spec in
                FieldRow(
                    spec: spec,
                    value: card.fields[spec.key] ?? spec.emptyState,
                    invalid: store.invalid == "\(card.id.uuidString):\(spec.key)",
                    onChange: { store.patchProvider(card.id, key: spec.key, $0) },
                    onReveal: spec.key == "api_key"
                        ? {
                            await store.reveal(SecretRef(
                                kind: "provider",
                                name: card.fields["name"]?.textValue
                                    .trimmingCharacters(in: .whitespaces) ?? "",
                            ))
                        }
                        : nil,
                )
                .id("\(card.id.uuidString):\(spec.key)")
            }

            DisclosureGroup(
                isExpanded: Binding(
                    get: { advancedOpen.contains(card.id) },
                    set: { open in
                        if open {
                            advancedOpen.insert(card.id)
                        } else {
                            advancedOpen.remove(card.id)
                        }
                    },
                ),
            ) {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(providerAdvFields, id: \.key) { spec in
                        FieldRow(
                            spec: spec,
                            value: card.fields[spec.key] ?? spec.emptyState,
                            invalid: store.invalid == "\(card.id.uuidString):\(spec.key)",
                            onChange: { store.patchProvider(card.id, key: spec.key, $0) },
                        )
                        .id("\(card.id.uuidString):\(spec.key)")
                    }
                }
                .padding(.top, 8)
            } label: {
                Text("高级选项")
                    .font(.system(size: Theme.textSm, weight: .medium))
                    .foregroundStyle(Theme.muted)
            }

            Text("模型目录")
                .font(.system(size: Theme.textSm, weight: .semibold))
                .foregroundStyle(Theme.fg)
            ForEach(card.models) { model in
                modelCard(card, model)
            }
            SettingsAddButton(title: "添加模型") { store.addModel(card) }
                .id("\(card.id.uuidString):add-model")
        }
    }

    // MARK: Model card (nested)

    private func modelCard(_ card: ProviderDraft, _ model: CardDraft) -> some View {
        let isOpen = store.openModelId == model.id
        let summary = modelSummary(model)
        return SettingsCard(
            isOpen: isOpen,
            onToggle: { store.openModelId = isOpen ? nil : model.id },
        ) {
            HStack(spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(summary.name)
                        .font(.system(size: Theme.textSm, weight: .semibold))
                        .foregroundStyle(summary.named ? Theme.fg : Theme.muted)
                    Text(summary.meta)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                Text("model")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .overlay(
                        RoundedRectangle(cornerRadius: 4)
                            .strokeBorder(Theme.bg2, lineWidth: 1),
                    )
                Spacer()
                Button { pendingDeleteModel = (card, model.id) } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 11))
                }
                .buttonStyle(GhostButtonStyle())
                .help("删除该模型")
            }
        } content: {
            ForEach(modelFields, id: \.key) { spec in
                FieldRow(
                    spec: spec,
                    value: model.fields[spec.key] ?? spec.emptyState,
                    invalid: store.invalid == "\(model.id.uuidString):\(spec.key)",
                    onChange: { store.patchModel(card.id, modelId: model.id, key: spec.key, $0) },
                )
                .id("\(model.id.uuidString):\(spec.key)")
            }
        }
    }
}

// MARK: - MCP

struct McpTabView: View {
    @Bindable var store: SettingsStore

    @State private var pendingDelete: McpDraft?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("MCP 服务器")
            ForEach(store.draft.mcpServers) { card in
                mcpCard(card)
            }
            SettingsAddButton(title: "添加 MCP 服务器") { store.addMcpServer() }
        }
        .task { await store.loadMcpStatus() }
        .confirmationDialog(
            "将删除该 MCP 服务器的配置。保存后生效，未保存前重新加载可恢复。",
            isPresented: .constant(pendingDelete != nil),
            titleVisibility: .visible,
        ) {
            Button("删除", role: .destructive) {
                if let card = pendingDelete {
                    store.deleteMcpServer(card)
                }
                pendingDelete = nil
            }
            Button("取消", role: .cancel) { pendingDelete = nil }
        }
    }

    private func mcpCard(_ card: McpDraft) -> some View {
        let isOpen = store.openMcpId == card.id
        let status = store.mcpStatus.first { $0.name == card.name }
        return SettingsCard(
            isOpen: isOpen,
            onToggle: { store.openMcpId = isOpen ? nil : card.id },
        ) {
            HStack(spacing: 8) {
                Text(card.name.trimmingCharacters(in: .whitespaces).isEmpty
                    ? "（未命名服务器）" : card.name)
                    .font(.system(size: Theme.textMd, weight: .semibold))
                    .foregroundStyle(card.name.isEmpty ? Theme.muted : Theme.fg)
                Text(card.transport.rawValue)
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .overlay(
                        RoundedRectangle(cornerRadius: 4)
                            .strokeBorder(Theme.bg2, lineWidth: 1),
                    )
                statusBadge(status)
                Spacer()
                Button { pendingDelete = card } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 11))
                }
                .buttonStyle(GhostButtonStyle())
                .help("删除该服务器")
            }
        } content: {
            FieldRow(
                spec: FieldSpec("name", label: "名称", ph: "服务器名（全局唯一，必填）", required: true),
                value: .text(card.name),
                invalid: store.invalid == "\(card.id.uuidString):name",
                onChange: { store.patchMcpName(card.id, $0.textValue) },
            )
            .id("\(card.id.uuidString):name")

            HStack(spacing: 8) {
                Text("传输方式")
                    .font(.system(size: Theme.textMd))
                    .foregroundStyle(Theme.fg)
                    .frame(width: 168, alignment: .leading)
                Picker("", selection: Binding(
                    get: { card.transport },
                    set: { store.patchMcpTransport(card.id, $0) },
                )) {
                    Text("stdio（本地命令）").tag(McpDraft.Transport.stdio)
                    Text("http（远程服务）").tag(McpDraft.Transport.http)
                }
                .pickerStyle(.segmented)
                .frame(width: 280)
            }

            let transportFields = card.transport == .http ? mcpHTTPFields : mcpStdioFields
            ForEach(transportFields, id: \.key) { spec in
                FieldRow(
                    spec: spec,
                    value: (card.transport == .http ? card.http : card.stdio)[spec.key]
                        ?? spec.emptyState,
                    invalid: store.invalid == "\(card.id.uuidString):\(spec.key)",
                    onChange: { store.patchMcp(card.id, key: spec.key, $0) },
                )
                .id("\(card.id.uuidString):\(spec.key)")
            }
            ForEach(mcpCommonFields, id: \.key) { spec in
                FieldRow(
                    spec: spec,
                    value: card.common[spec.key] ?? spec.emptyState,
                    invalid: store.invalid == "\(card.id.uuidString):\(spec.key)",
                    onChange: { store.patchMcpCommon(card.id, key: spec.key, $0) },
                )
                .id("\(card.id.uuidString):\(spec.key)")
            }

            if !card.name.trimmingCharacters(in: .whitespaces).isEmpty {
                HStack(spacing: 8) {
                    Button("重新连接") {
                        Task { await store.reconnectMcpServer(card.name) }
                    }
                    .buttonStyle(OutlineButtonStyle())
                    .help("保存配置后手动重连该服务器")
                    if let error = status?.error, !error.isEmpty {
                        Text(error)
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(Theme.error)
                            .lineLimit(2)
                    }
                }
            }
        }
    }

    @ViewBuilder private func statusBadge(_ status: McpServerStatus?) -> some View {
        if let status {
            if status.connected == true {
                Label("已连接 · \(status.tools?.count ?? 0) 工具", systemImage: "circle.fill")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.success)
            } else if let error = status.error, !error.isEmpty {
                Label("连接失败", systemImage: "exclamationmark.triangle.fill")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.error)
                    .help(error)
            } else {
                Text("未连接")
                    .font(.system(size: Theme.textXs))
                    .foregroundStyle(Theme.muted)
            }
        }
    }
}

// MARK: - Skills

struct SkillsTabView: View {
    @Bindable var store: SettingsStore

    @State private var pendingDelete: SkillsOverview.SkillInfo?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("技能配置")
            ForEach(skillsConfigFields, id: \.key) { spec in
                FieldRow(
                    spec: spec,
                    value: store.draft.globals[spec.key] ?? spec.emptyState,
                    invalid: store.invalid == spec.key,
                    onChange: { store.setGlobal(spec.key, $0) },
                )
                .id(spec.key)
            }
        }

        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("已发现的技能")
            runtimeContent
        }
        .task { await store.loadSkills() }
        .confirmationDialog(
            "将从磁盘删除该 skill 的目录，不可恢复。",
            isPresented: .constant(pendingDelete != nil),
            titleVisibility: .visible,
        ) {
            Button("删除", role: .destructive) {
                if let skill = pendingDelete {
                    Task { await store.deleteSkill(path: skill.path) }
                }
                pendingDelete = nil
            }
            Button("取消", role: .cancel) { pendingDelete = nil }
        }
    }

    @ViewBuilder private var runtimeContent: some View {
        if let overview = store.skills {
            if overview.enabled == false {
                Label(overview.reason ?? "技能已禁用", systemImage: "exclamationmark.triangle")
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.warning)
            }
            let groups = overview.groups ?? []
            let anySkill = groups.contains { !($0.skills ?? []).isEmpty }
            if !anySkill {
                Text(skillsEmptyHint)
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.muted)
            }
            ForEach(Array(groups.enumerated()), id: \.offset) { _, group in
                VStack(alignment: .leading, spacing: 6) {
                    Text(group.shared == true ? "用户级（共享）" : group.workspaceName)
                        .font(.system(size: Theme.textSm, weight: .semibold))
                        .foregroundStyle(Theme.fg)
                    ForEach(group.issues ?? [], id: \.self) { issue in
                        Label(issue, systemImage: "exclamationmark.triangle")
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(Theme.warning)
                    }
                    ForEach(group.skills ?? []) { skill in
                        skillRow(skill)
                    }
                }
            }
        } else {
            Text(store.skillsLoaded ? skillsEmptyHint : "加载中…")
                .font(.system(size: Theme.textSm))
                .foregroundStyle(Theme.muted)
        }
    }

    private func skillRow(_ skill: SkillsOverview.SkillInfo) -> some View {
        HStack(alignment: .top, spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(skill.name)
                        .font(.system(size: Theme.textMd, weight: .medium))
                        .foregroundStyle(Theme.fg)
                    if let scope = skill.scope, !scope.isEmpty {
                        Text(scope)
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(Theme.muted)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 1)
                            .overlay(
                                RoundedRectangle(cornerRadius: 4)
                                    .strokeBorder(Theme.bg2, lineWidth: 1),
                            )
                    }
                }
                if let description = skill.description, !description.isEmpty {
                    Text(description)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                        .lineLimit(2)
                }
                Text(skill.path)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(Theme.muted.opacity(0.7))
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Toggle("禁用", isOn: Binding(
                get: { skill.disabled == true },
                set: { disabled in
                    Task { await store.setSkillDisabled(skill.name, disabled: disabled) }
                },
            ))
            .toggleStyle(.checkbox)
            .font(.system(size: Theme.textSm))
            Button { pendingDelete = skill } label: {
                Image(systemName: "trash")
                    .font(.system(size: 11))
            }
            .buttonStyle(GhostButtonStyle())
            .help("删除该 skill（不可恢复）")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
    }
}

// MARK: - Rule packs (permission tab extras)

struct RulePacksView: View {
    @Bindable var store: SettingsStore

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("规则包")
            if let packs = store.rulePacks {
                if packs.isEmpty {
                    Text("暂无可用规则包")
                        .font(.system(size: Theme.textSm))
                        .foregroundStyle(Theme.muted)
                }
                ForEach(packs) { pack in
                    packRow(pack)
                }
            } else {
                Text("加载中…")
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.muted)
            }
        }
        .task { await store.loadRulePacks() }
    }

    private func packRow(_ pack: RulePack) -> some View {
        HStack(alignment: .top, spacing: 10) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(pack.name)
                        .font(.system(size: Theme.textMd, weight: .medium))
                        .foregroundStyle(Theme.fg)
                    if let risk = pack.risk, !risk.isEmpty {
                        Text(risk)
                            .font(.system(size: Theme.textXs))
                            .foregroundStyle(riskColor(risk))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 1)
                            .overlay(
                                RoundedRectangle(cornerRadius: 4)
                                    .strokeBorder(riskColor(risk).opacity(0.5), lineWidth: 1),
                            )
                    }
                }
                if let description = pack.description, !description.isEmpty {
                    Text(description)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted)
                }
                if let reason = pack.reason, !reason.isEmpty {
                    Text(reason)
                        .font(.system(size: Theme.textXs))
                        .foregroundStyle(Theme.muted.opacity(0.8))
                }
            }
            Spacer()
            Button(pack.installed == true ? "卸载" : "安装") {
                Task { await store.installRulePack(pack.id, install: pack.installed != true) }
            }
            .buttonStyle(OutlineButtonStyle())
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusMd))
    }

    private func riskColor(_ risk: String) -> Color {
        switch risk {
        case "high", "critical": Theme.error
        case "medium": Theme.warning
        default: Theme.muted
        }
    }
}

// MARK: - System extras (workspaces + dev environment report)

struct SystemExtrasView: View {
    @Bindable var store: SettingsStore

    var body: some View {
        // Workspaces (config.yaml workspaces[]) — WebUI SystemExtras card.
        VStack(alignment: .leading, spacing: 10) {
            SettingsSectionTitle("工作区")
            ForEach(store.draft.workspaces) { card in
                workspaceCard(card)
            }
            SettingsAddButton(title: "添加工作区") { store.addWorkspaceCard() }
        }

        // Dev-environment runtime report (read-only).
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                SettingsSectionTitle("开发环境")
                Spacer()
                Button {
                    Task { await store.loadEnvironment() }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(GhostButtonStyle())
                .help("重新探测")
            }

            if let env = store.environment {
                if let tools = env.tools, !tools.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(tools) { tool in
                            HStack(spacing: 8) {
                                Image(systemName: tool.found == true
                                    ? "checkmark.circle.fill" : "xmark.circle")
                                    .font(.system(size: 11))
                                    .foregroundStyle(tool.found == true ? Theme.success : Theme.muted)
                                Text(tool.name)
                                    .font(.system(size: Theme.textSm, weight: .medium, design: .monospaced))
                                    .foregroundStyle(Theme.fg)
                                    .frame(width: 100, alignment: .leading)
                                Text(tool.found == true ? (tool.path ?? "") : "未找到")
                                    .font(.system(size: Theme.textXs, design: .monospaced))
                                    .foregroundStyle(Theme.muted)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                            }
                        }
                    }
                }
                if let dirs = env.dirs, !dirs.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("PATH 组装")
                            .font(.system(size: Theme.textSm, weight: .semibold))
                            .foregroundStyle(Theme.fg)
                        ForEach(dirs) { dir in
                            HStack(spacing: 8) {
                                Text(dir.path)
                                    .font(.system(size: Theme.textXs, design: .monospaced))
                                    .foregroundStyle(Theme.fg)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                Spacer()
                                Text([dir.source, dir.status].compactMap { $0 }.joined(separator: " · "))
                                    .font(.system(size: Theme.textXs))
                                    .foregroundStyle(Theme.muted)
                            }
                        }
                    }
                }
                if let path = env.effectivePath, !path.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("有效 PATH")
                            .font(.system(size: Theme.textSm, weight: .semibold))
                            .foregroundStyle(Theme.fg)
                        Text(path)
                            .font(.system(size: Theme.textXs, design: .monospaced))
                            .foregroundStyle(Theme.muted)
                            .textSelection(.enabled)
                            .padding(8)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Theme.bg1, in: RoundedRectangle(cornerRadius: Theme.radiusSm))
                    }
                }
            } else {
                Text("加载中…")
                    .font(.system(size: Theme.textSm))
                    .foregroundStyle(Theme.muted)
            }
        }
        .task { await store.loadEnvironment() }
    }

    private func workspaceCard(_ card: CardDraft) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(card.fields["name"]?.textValue.isEmpty == false
                    ? card.fields["name"]?.textValue ?? ""
                    : "（未命名工作区）")
                    .font(.system(size: Theme.textMd, weight: .semibold))
                    .foregroundStyle(Theme.fg)
                Spacer()
                Button { store.deleteWorkspaceCard(card) } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 11))
                }
                .buttonStyle(GhostButtonStyle())
                .help("移除该工作区（保存后生效）")
            }
            FieldRow(
                spec: FieldSpec("name", label: "名称"),
                value: card.fields["name"] ?? .text(""),
                onChange: { store.patchWorkspace(card.id, key: "name", $0) },
            )
            FieldRow(
                spec: FieldSpec("root", label: "根目录", ph: "/path/to/project", required: true),
                value: card.fields["root"] ?? .text(""),
                invalid: store.invalid == "\(card.id.uuidString):root",
                onChange: { store.patchWorkspace(card.id, key: "root", $0) },
            )
            .id("\(card.id.uuidString):root")
        }
        .padding(12)
        .background(Theme.bg1, in: RoundedRectangle(cornerRadius: 10))
        .overlay(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(Theme.bg2, lineWidth: 1),
        )
    }
}

// MARK: - Summaries (WebUI providerSummary / modelSummary)

private func providerSummary(_ card: ProviderDraft) -> (name: String, meta: String, named: Bool) {
    var p: [String: JSONValue] = [:]
    collectFields(providerBaseFields + providerAdvFields + [FieldSpec("name")], card.fields, into: &p)
    let name = p["name"]?.stringValue
    let meta = [
        p["type"]?.stringValue ?? "openai",
        p["base_url"]?.stringValue ?? "未配置 Base URL",
        "\(card.models.count) 个模型",
    ].joined(separator: " · ")
    return (name ?? "（未命名 provider）", meta, name != nil)
}

private func modelSummary(_ card: CardDraft) -> (name: String, meta: String, named: Bool) {
    var m: [String: JSONValue] = [:]
    collectFields(modelFields, card.fields, into: &m)
    var parts: [String] = []
    if let window = m["context_window"]?.numberText {
        parts.append("上下文 \(window)")
    }
    if let output = m["max_output_tokens"]?.numberText {
        parts.append("输出上限 \(output)")
    }
    if case let .array(modalities) = m["modalities"], modalities.contains(.string("image")) {
        parts.append("多模态")
    }
    let name = m["name"]?.stringValue
    return (
        name ?? "（未命名模型）",
        parts.isEmpty ? "跟随 provider / 全局默认" : parts.joined(separator: " · "),
        name != nil,
    )
}
