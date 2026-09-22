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

import Foundation

// MARK: - Draft model (settings/draft.ts)

struct CardDraft: Identifiable, Equatable, Sendable {
    let id = UUID()
    var fields: [String: ControlState] = [:]
}

struct ProviderDraft: Identifiable, Equatable, Sendable {
    let id = UUID()
    var fields: [String: ControlState] = [:]
    var models: [CardDraft] = []
}

struct McpDraft: Identifiable, Equatable, Sendable {
    enum Transport: String, Sendable {
        case stdio, http
    }

    let id = UUID()
    var name = ""
    var transport: Transport = .stdio
    var stdio: [String: ControlState] = [:]
    var http: [String: ControlState] = [:]
    var common: [String: ControlState] = [:]
}

struct SettingsDraft: Equatable, Sendable {
    var globals: [String: ControlState] = [:]
    var providers: [ProviderDraft] = []
    var mcpServers: [McpDraft] = []
    var workspaces: [CardDraft] = []
}

/// Validation failure location (SettingsPanel InvalidTarget): the panel
/// switches tab, enters the card, and highlights the field.
struct InvalidTarget: Sendable {
    let msg: String
    let tab: String
    let fieldId: String
    var providerCardId: UUID?
    var modelCardId: UUID?
    var mcpCardId: UUID?
}

// MARK: - Settings store (SettingsPanel.tsx state machine)

/// The config.yaml graphical editor: fill once on load, edits write the
/// draft directly, collect on save — fields of tabs never opened are
/// never lost by construction. Secrets stay masked in the draft; the
/// server restores them from the file on PUT.
@MainActor
@Observable
final class SettingsStore {
    enum LoadState: Equatable {
        case idle
        case loading
        case loaded
        case failed(String)
    }

    private let api: APIClient

    private(set) var loadState: LoadState = .idle
    private(set) var revision = ""
    private(set) var origCfg: [String: JSONValue] = [:]
    private(set) var cfgPath = ""
    var draft = SettingsDraft()
    var dirty = false
    var saving = false
    /// Footer status line (WebUI settings-msg).
    private(set) var msg = ""
    private(set) var msgIsError = false
    /// Green flash on the save button after a successful save.
    var flashSave = false
    /// Currently highlighted field after a failed validation
    /// (globals: spec.key; cards: "cardId:key").
    var invalid: String?
    var activeTab = "providers"
    /// Accordion state survives tab switches (WebUI keeps it in the
    /// panel, which stays mounted while open).
    var openProviderId: UUID?
    var openModelId: UUID?
    var openMcpId: UUID?

    // Runtime (non-config) data for the custom tabs.
    private(set) var skills: SkillsOverview?
    private(set) var skillsLoaded = false
    private(set) var mcpStatus: [McpServerStatus] = []
    private(set) var rulePacks: [RulePack]?
    private(set) var environment: EnvironmentReport?

    /// Config-dependent UI refresh hook (WebUI controller.refreshModelCatalog):
    /// wired by the caller to reload the composer's model catalog.
    var onConfigSaved: (@MainActor () -> Void)?

    init(api: APIClient) {
        self.api = api
    }

    private func showMsg(_ text: String, isError: Bool = false) {
        msg = text
        msgIsError = isError
    }

    func markDirty() {
        dirty = true
        invalid = nil
    }

    // MARK: Load (fill)

    func load() async {
        if loadState != .loaded {
            loadState = .loading
        }
        let prevRevision = revision
        let wasDirty = dirty
        do {
            let envelope = try await api.getConfig()
            let config: [String: JSONValue] = if case let .object(map) = envelope.config {
                map
            } else {
                [:]
            }
            revision = envelope.revision
            origCfg = config
            cfgPath = envelope.exists ? envelope.path : "\(envelope.path) (not created yet; written on save)"
            dirty = false
            showMsg(envelope.exists ? "" : "First-time setup: add at least one provider on the Models tab")
            // Skip rebuilding the draft when nothing changed: scroll
            // position and expansion state survive a no-op reload.
            if loadState != .loaded || wasDirty || envelope.revision != prevRevision {
                draft = Self.buildDraft(from: config)
                loadState = .loaded
            } else {
                await loadMcpStatus()
            }
        } catch {
            showMsg("Failed to load configuration: \(error.localizedDescription)", isError: true)
            if loadState != .loaded {
                loadState = .failed(error.localizedDescription)
            }
        }
    }

    private static func buildDraft(from config: [String: JSONValue]) -> SettingsDraft {
        var draft = SettingsDraft()
        for spec in globalFieldSpecs() {
            draft.globals[spec.key] = fillValue(spec, getPath(config, spec.key))
        }
        if case let .array(providers) = config["providers"] {
            draft.providers = providers.compactMap { value in
                guard case let .object(p) = value else { return nil }
                var fields: [String: ControlState] = [:]
                for spec in providerAllFields {
                    fields[spec.key] = fillValue(spec, p[spec.key])
                }
                var models: [CardDraft] = []
                if case let .array(items) = p["models"] {
                    models = items.compactMap { item in
                        guard case let .object(m) = item else { return nil }
                        var mf: [String: ControlState] = [:]
                        for spec in modelFields {
                            mf[spec.key] = fillValue(spec, m[spec.key])
                        }
                        return CardDraft(fields: mf)
                    }
                }
                return ProviderDraft(fields: fields, models: models)
            }
        }
        if case let .object(servers) = config["mcp_servers"] {
            draft.mcpServers = servers
                .sorted { $0.key < $1.key }
                .map { name, value in
                    let srv: [String: JSONValue] = if case let .object(map) = value {
                        map
                    } else {
                        [:]
                    }
                    let fill = { (specs: [FieldSpec]) in
                        specs.reduce(into: [String: ControlState]()) { out, spec in
                            out[spec.key] = fillValue(spec, srv[spec.key])
                        }
                    }
                    return McpDraft(
                        name: name,
                        transport: srv["url"] != nil ? .http : .stdio,
                        stdio: fill(mcpStdioFields),
                        http: fill(mcpHTTPFields),
                        common: fill(mcpCommonFields),
                    )
                }
        }
        if case let .array(workspaces) = config["workspaces"] {
            draft.workspaces = workspaces.compactMap { value in
                guard case let .object(ws) = value else { return nil }
                return CardDraft(fields: [
                    "name": fillValue(FieldSpec("name"), ws["name"]),
                    "root": fillValue(FieldSpec("root"), ws["root"]),
                ])
            }
        }
        return draft
    }

    // MARK: Draft editing helpers (all mark dirty, like setGlobal)

    func setGlobal(_ key: String, _ value: ControlState) {
        draft.globals[key] = value
        markDirty()
    }

    func patchProvider(_ cardId: UUID, key: String, _ value: ControlState) {
        guard let index = draft.providers.firstIndex(where: { $0.id == cardId }) else { return }
        draft.providers[index].fields[key] = value
        markDirty()
    }

    func patchModel(_ cardId: UUID, modelId: UUID, key: String, _ value: ControlState) {
        guard let card = draft.providers.firstIndex(where: { $0.id == cardId }),
              let model = draft.providers[card].models.firstIndex(where: { $0.id == modelId })
        else { return }
        draft.providers[card].models[model].fields[key] = value
        markDirty()
    }

    func addProvider() {
        let card = ProviderDraft(fields: ["type": .text("openai")])
        draft.providers.append(card)
        markDirty()
        openProviderId = card.id
        openModelId = nil
    }

    func deleteProvider(_ card: ProviderDraft) {
        if openProviderId == card.id {
            openModelId = nil
            openProviderId = nil
        }
        draft.providers.removeAll { $0.id == card.id }
        markDirty()
    }

    func addModel(_ card: ProviderDraft) {
        guard let index = draft.providers.firstIndex(where: { $0.id == card.id }) else { return }
        let model = CardDraft()
        draft.providers[index].models.append(model)
        markDirty()
        openProviderId = card.id
        openModelId = model.id
    }

    func deleteModel(_ card: ProviderDraft, modelId: UUID) {
        if openModelId == modelId {
            openModelId = nil
        }
        guard let index = draft.providers.firstIndex(where: { $0.id == card.id }) else { return }
        draft.providers[index].models.removeAll { $0.id == modelId }
        markDirty()
    }

    func addMcpServer() {
        let card = McpDraft()
        draft.mcpServers.append(card)
        markDirty()
        openMcpId = card.id
    }

    func deleteMcpServer(_ card: McpDraft) {
        if openMcpId == card.id {
            openMcpId = nil
        }
        draft.mcpServers.removeAll { $0.id == card.id }
        markDirty()
    }

    func patchMcpName(_ cardId: UUID, _ name: String) {
        guard let index = draft.mcpServers.firstIndex(where: { $0.id == cardId }) else { return }
        draft.mcpServers[index].name = name
        markDirty()
    }

    /// Switching transport keeps both sides' fields (WebUI McpDraft).
    func patchMcpTransport(_ cardId: UUID, _ transport: McpDraft.Transport) {
        guard let index = draft.mcpServers.firstIndex(where: { $0.id == cardId }) else { return }
        draft.mcpServers[index].transport = transport
        markDirty()
    }

    func patchMcp(_ cardId: UUID, key: String, _ value: ControlState) {
        guard let index = draft.mcpServers.firstIndex(where: { $0.id == cardId }) else { return }
        if draft.mcpServers[index].transport == .http {
            draft.mcpServers[index].http[key] = value
        } else {
            draft.mcpServers[index].stdio[key] = value
        }
        markDirty()
    }

    func patchMcpCommon(_ cardId: UUID, key: String, _ value: ControlState) {
        guard let index = draft.mcpServers.firstIndex(where: { $0.id == cardId }) else { return }
        draft.mcpServers[index].common[key] = value
        markDirty()
    }

    func addWorkspaceCard() {
        draft.workspaces.append(CardDraft())
        markDirty()
    }

    func deleteWorkspaceCard(_ card: CardDraft) {
        draft.workspaces.removeAll { $0.id == card.id }
        markDirty()
    }

    func patchWorkspace(_ cardId: UUID, key: String, _ value: ControlState) {
        guard let index = draft.workspaces.firstIndex(where: { $0.id == cardId }) else { return }
        draft.workspaces[index].fields[key] = value
        markDirty()
    }

    // MARK: Validation (SettingsPanel firstInvalid)

    func firstInvalid() -> InvalidTarget? {
        var namedProviders = 0
        for card in draft.providers {
            var p: [String: JSONValue] = [:]
            collectFields(providerAllFields, card.fields, into: &p)
            guard let name = p["name"]?.stringValue, !name.isEmpty else {
                if !p.isEmpty || !card.models.isEmpty {
                    return InvalidTarget(
                        msg: "A provider is missing its name", tab: "providers",
                        fieldId: "\(card.id.uuidString):name", providerCardId: card.id,
                    )
                }
                continue
            }
            namedProviders += 1
            if p["base_url"]?.stringValue?.isEmpty != false {
                return InvalidTarget(
                    msg: "Provider \"\(name)\" is missing its Base URL", tab: "providers",
                    fieldId: "\(card.id.uuidString):base_url", providerCardId: card.id,
                )
            }
            if p["api_key"] != nil, p["api_key_env"] != nil {
                return InvalidTarget(
                    msg: "Provider \"\(name)\": API key and key env var are mutually exclusive", tab: "providers",
                    fieldId: "\(card.id.uuidString):api_key_env", providerCardId: card.id,
                )
            }
            var namedModels = 0
            for modelCard in card.models {
                var m: [String: JSONValue] = [:]
                collectFields(modelFields, modelCard.fields, into: &m)
                if m["name"]?.stringValue?.isEmpty == false {
                    namedModels += 1
                } else if !m.isEmpty {
                    return InvalidTarget(
                        msg: "Provider \"\(name)\" has a model missing its name", tab: "providers",
                        fieldId: "\(modelCard.id.uuidString):name",
                        providerCardId: card.id, modelCardId: modelCard.id,
                    )
                }
            }
            if namedModels == 0 {
                return InvalidTarget(
                    msg: "Provider \"\(name)\" needs at least one model", tab: "providers",
                    fieldId: "\(card.id.uuidString):add-model", providerCardId: card.id,
                )
            }
        }
        if namedProviders == 0 {
            return InvalidTarget(
                msg: "Add at least one provider on the Models tab first", tab: "providers",
                fieldId: "add-provider",
            )
        }
        for card in draft.mcpServers {
            var srv: [String: JSONValue] = [:]
            collectFields(mcpCommonFields, card.common, into: &srv)
            collectFields(
                card.transport == .http ? mcpHTTPFields : mcpStdioFields,
                card.transport == .http ? card.http : card.stdio,
                into: &srv,
            )
            let name = card.name.trimmingCharacters(in: .whitespaces)
            if name.isEmpty {
                if !srv.isEmpty {
                    return InvalidTarget(
                        msg: "An MCP server is missing its name", tab: "mcp",
                        fieldId: "\(card.id.uuidString):name", mcpCardId: card.id,
                    )
                }
                continue
            }
            if card.transport == .stdio, srv["command"]?.stringValue?.isEmpty != false {
                return InvalidTarget(
                    msg: "MCP server \"\(name)\" is missing its command", tab: "mcp",
                    fieldId: "\(card.id.uuidString):command", mcpCardId: card.id,
                )
            }
            if card.transport == .http, srv["url"]?.stringValue?.isEmpty != false {
                return InvalidTarget(
                    msg: "MCP server \"\(name)\" is missing its URL", tab: "mcp",
                    fieldId: "\(card.id.uuidString):url", mcpCardId: card.id,
                )
            }
        }
        for card in draft.workspaces {
            var ws: [String: JSONValue] = [:]
            collectFields([FieldSpec("name"), FieldSpec("root")], card.fields, into: &ws)
            if ws["root"] == nil, let name = ws["name"]?.stringValue {
                return InvalidTarget(
                    msg: "Workspace \"\(name)\" is missing its root directory", tab: "system",
                    fieldId: "\(card.id.uuidString):root",
                )
            }
        }
        return nil
    }

    /// Locate the failing field: switch tab + enter the card (WebUI
    /// locate); the view scrolls the field into view by its anchor id.
    func locate(_ target: InvalidTarget) {
        activeTab = target.tab
        if let providerCardId = target.providerCardId {
            openModelId = nil
            openProviderId = providerCardId
        }
        if let modelCardId = target.modelCardId {
            openModelId = modelCardId
        }
        if let mcpCardId = target.mcpCardId {
            openMcpId = mcpCardId
        }
        invalid = target.fieldId
    }

    // MARK: Save

    private func applyMsg(_ result: PutConfigResult) -> String {
        guard let applied = result.applied else { return "Saved" }
        var parts: [String] = []
        if let immediate = applied.immediate, !immediate.isEmpty {
            parts.append("effective immediately: " + immediate.joined(separator: ", "))
        }
        if let nextTurn = applied.nextTurn, !nextTurn.isEmpty {
            parts.append("effective next turn: " + nextTurn.joined(separator: ", "))
        }
        if let restart = applied.restart, !restart.isEmpty {
            parts.append("effective after restart: " + restart.joined(separator: ", "))
        }
        return parts.isEmpty ? "Saved (no changes)" : "Saved — " + parts.joined(separator: "; ")
    }

    func save() async {
        if saving {
            return
        } // a repeated PUT carries the old revision and inevitably 409s
        saving = true
        defer { saving = false }
        if let bad = firstInvalid() {
            locate(bad)
            showMsg(bad.msg, isError: true)
            return
        }
        var cfg: [String: JSONValue] = [:]
        var skippedCards = 0
        for spec in globalFieldSpecs() {
            let state = draft.globals[spec.key]
                ?? (spec.type == .bool || spec.type == .flagList ? .flag(false) : .text(""))
            collectValue(spec, state, into: &cfg)
        }
        var providers: [JSONValue] = []
        for card in draft.providers {
            var p: [String: JSONValue] = [:]
            collectFields(providerAllFields, card.fields, into: &p)
            var models: [JSONValue] = []
            for modelCard in card.models {
                var m: [String: JSONValue] = [:]
                collectFields(modelFields, modelCard.fields, into: &m)
                if m["name"] != nil {
                    models.append(.object(m))
                } else if !m.isEmpty {
                    skippedCards += 1
                }
            }
            if !models.isEmpty {
                p["models"] = .array(models)
            }
            if p["name"] != nil {
                providers.append(.object(p))
            } else if !p.isEmpty {
                skippedCards += 1
            }
        }
        if !providers.isEmpty {
            cfg["providers"] = .array(providers)
        }
        var servers: [String: JSONValue] = [:]
        for card in draft.mcpServers {
            let name = card.name.trimmingCharacters(in: .whitespaces)
            if name.isEmpty {
                continue
            }
            var srv: [String: JSONValue] = [:]
            collectFields(mcpCommonFields, card.common, into: &srv)
            collectFields(
                card.transport == .http ? mcpHTTPFields : mcpStdioFields,
                card.transport == .http ? card.http : card.stdio,
                into: &srv,
            )
            if servers[name] != nil {
                skippedCards += 1
            }
            servers[name] = .object(srv)
        }
        if !servers.isEmpty {
            cfg["mcp_servers"] = .object(servers)
        }
        var wss: [JSONValue] = []
        for card in draft.workspaces {
            var ws: [String: JSONValue] = [:]
            collectFields([FieldSpec("name"), FieldSpec("root")], card.fields, into: &ws)
            if ws["root"] != nil {
                wss.append(.object(ws))
            } else if ws["name"] != nil {
                skippedCards += 1
            }
        }
        if !wss.isEmpty {
            cfg["workspaces"] = .array(wss)
        }
        if skippedCards > 0 {
            showMsg("\(skippedCards) card(s) were not saved (missing required name/root)", isError: true)
        }

        preserveUnmanaged(&cfg, orig: origCfg)
        showMsg("Saving… (MCP changes need to connect; may take a few seconds)")
        do {
            let result = try await api.putConfig(revision: revision, config: .object(cfg))
            revision = result.revision ?? revision
            let pathExtraChanged = getPath(cfg, "tools.path_extra") != getPath(origCfg, "tools.path_extra")
            origCfg = cfg
            dirty = false
            showMsg(applyMsg(result))
            flashSave = true
            Task { [weak self] in
                try? await Task.sleep(for: .milliseconds(1300))
                self?.flashSave = false
            }
            await loadMcpStatus()
            if pathExtraChanged {
                await loadEnvironment()
            }
            onConfigSaved?()
        } catch let LoomAPIError.http(_, code, _) where code == "config_conflict" {
            showMsg("The config file was modified externally — Reload, then save again", isError: true)
        } catch {
            showMsg("Save failed: \(error.localizedDescription)", isError: true)
        }
    }

    // MARK: Secrets

    /// Reveal one stored secret (WebUI ctx.reveal): the plaintext is
    /// fetched on demand; failures surface on the footer status line.
    func reveal(_ ref: SecretRef) async -> String? {
        if (ref.name ?? "").isEmpty, ref.kind == "provider" || ref.kind == "mcp_header" {
            showMsg("Fill in the name and save the configuration before revealing", isError: true)
            return nil
        }
        do {
            return try await api.revealSecret(ref)
        } catch let LoomAPIError.http(status, _, _) where status == 404 {
            showMsg("No secret saved here (save the configuration first)", isError: true)
            return nil
        } catch {
            showMsg("Failed to reveal secret: \(error.localizedDescription)", isError: true)
            return nil
        }
    }

    // MARK: Runtime data (skills / mcp / packs / environment)

    func loadSkills() async {
        do {
            skills = try await api.listSkills()
            skillsLoaded = true
        } catch {
            showMsg("Failed to load skills: \(error.localizedDescription)", isError: true)
        }
    }

    /// The disable endpoint rewrote the config file: sync revision and
    /// skills.disabled into the baseline, or a later settings save will
    /// 409-conflict / roll the disabled list back (WebUI onDisabledChanged).
    func setSkillDisabled(_ name: String, disabled: Bool) async {
        do {
            let result = try await api.setSkillDisabled(name, disabled: disabled)
            if let rev = result.revision {
                revision = rev
            }
            if let disabled = result.disabled {
                setPath(&origCfg, "skills.disabled", .array(disabled.map { .string($0) }))
            }
            await loadSkills()
        } catch {
            showMsg("Failed to update skill state: \(error.localizedDescription)", isError: true)
        }
    }

    func deleteSkill(path: String) async {
        do {
            try await api.deleteSkill(path: path)
            // The deletion rewrote the file's revision too — reload the
            // whole config so the baseline never goes stale.
            await load()
            await loadSkills()
        } catch {
            showMsg("Failed to delete skill: \(error.localizedDescription)", isError: true)
        }
    }

    func loadMcpStatus() async {
        mcpStatus = await (try? api.listMcpServers()) ?? mcpStatus
    }

    func reconnectMcpServer(_ name: String) async {
        do {
            try await api.reconnectMcpServer(name)
        } catch {
            showMsg("Reconnect failed: \(error.localizedDescription)", isError: true)
        }
        await loadMcpStatus()
    }

    func loadRulePacks() async {
        do {
            rulePacks = try await api.listRulePacks()
        } catch {
            showMsg("Failed to load rule packs: \(error.localizedDescription)", isError: true)
        }
    }

    func installRulePack(_ id: String, install: Bool) async {
        do {
            if install {
                try await api.installRulePack(id)
            } else {
                try await api.uninstallRulePack(id)
            }
            await loadRulePacks()
        } catch {
            showMsg("Rule pack operation failed: \(error.localizedDescription)", isError: true)
        }
    }

    func loadEnvironment() async {
        environment = try? await api.metaEnvironment()
    }
}
