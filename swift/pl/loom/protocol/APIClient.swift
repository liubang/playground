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

/// Thin REST adapter over `loom serve`'s /v1 surface (SERVE_DESIGN §5.3).
/// Bearer-token auth; typed error mapping onto the server error model.
/// The client holds no session state — stores own all semantics.
struct APIClient: Sendable {
    let baseURL: URL
    let token: String
    private let session: URLSession

    init(baseURL: URL, token: String, session: URLSession = .shared) {
        // Strip trailing slashes so path joins are stable.
        var base = baseURL.absoluteString
        while base.hasSuffix("/") {
            base.removeLast()
        }
        self.baseURL = URL(string: base)!
        self.token = token
        self.session = session
    }

    // MARK: - Meta

    func metaVersion() async throws -> MetaVersion {
        try await get("/v1/meta/version")
    }

    /// Model catalog for the composer's model picker.
    func metaModels() async throws -> MetaModels {
        try await get("/v1/meta/models")
    }

    // MARK: - Sessions

    func listSessions(
        workspaceId: String? = nil, limit: Int = 200, archived: Bool = false, cursor: String? = nil,
    ) async throws -> SessionListResponse {
        var query = [URLQueryItem(name: "limit", value: String(limit))]
        if let cursor, !cursor.isEmpty {
            query.append(URLQueryItem(name: "cursor", value: cursor))
        }
        if let workspaceId {
            query.append(URLQueryItem(name: "workspace_id", value: workspaceId))
        }
        if archived {
            query.append(URLQueryItem(name: "archived", value: "1"))
        }
        return try await get("/v1/sessions", query: query)
    }

    @discardableResult
    func createSession(resume: String? = nil, workspaceId: String? = nil) async throws -> CreateSessionResponse {
        var body: [String: String] = [:]
        if let resume {
            body["resume"] = resume
        }
        if let workspaceId {
            body["workspace_id"] = workspaceId
        }
        return try await post("/v1/sessions", json: body)
    }

    func deleteSession(_ id: String) async throws {
        try await requestNoContent("DELETE", "/v1/sessions/\(id)")
    }

    /// POST /v1/sessions/{id}/archive (WebUI archiveSession): archived
    /// sessions leave the default sidebar listing.
    func archiveSession(_ id: String, archived: Bool) async throws {
        struct ArchiveResponse: Decodable { let archived: Bool? }
        let _: ArchiveResponse = try await post(
            "/v1/sessions/\(id)/archive",
            json: ["archived": archived],
        )
    }

    func snapshot(_ id: String) async throws -> Snapshot {
        try await get("/v1/sessions/\(id)/snapshot")
    }

    func transcript(_ id: String, after: UInt64? = nil, limit: Int = 200) async throws -> TranscriptPage {
        var query = [URLQueryItem(name: "limit", value: String(limit))]
        if let after {
            query.append(URLQueryItem(name: "after", value: String(after)))
        }
        return try await get("/v1/sessions/\(id)/transcript", query: query)
    }

    /// Server-side execution projection: verdicts and detour classification
    /// are authoritative; clients only render this shape.
    func maze(_ id: String) async throws -> MazeData {
        try await get("/v1/sessions/\(id)/maze")
    }

    /// GET /v1/sessions/{id}/export — the raw event log (NDJSON) as
    /// bytes; the trace tab's "Session log" button saves it to disk.
    func exportSessionLog(_ id: String) async throws -> Data {
        var request = authorizedRequest("GET", "/v1/sessions/\(id)/export", query: [])
        request.timeoutInterval = 120
        let (data, response) = try await perform(request)
        try checkStatus(response, data: data)
        return data
    }

    // MARK: - Turn control

    @discardableResult
    func sendPrompt(_ id: String, prompt: String, idempotencyKey: String) async throws -> PromptResponse {
        try await post(
            "/v1/sessions/\(id)/prompts",
            json: ["prompt": prompt],
            headers: ["Idempotency-Key": idempotencyKey],
        )
    }

    func cancel(_ id: String) async throws {
        struct CancelResponse: Decodable { let status: String }
        let _: CancelResponse = try await postEmpty("/v1/sessions/\(id)/cancel")
    }

    func requestCompaction(_ id: String) async throws {
        struct CompactResponse: Decodable { let AlreadyPending: Bool? }
        let _: CompactResponse = try await postEmpty("/v1/sessions/\(id)/compact")
    }

    // MARK: - Turn change review / revert (turn-summary card)

    /// GET /v1/sessions/{id}/runs/{runID}/changes — per-path +/− stats
    /// and inline diffs, ledger-before vs CURRENT workspace content
    /// (git-free; stale after a revert — refetch). A run with no
    /// recorded changes answers an empty list, not an error.
    func runChanges(_ id: String, runId: String) async throws -> RunChangeStatsResponse {
        try await get("/v1/sessions/\(id)/runs/\(Self.pathEscaped(runId))/changes")
    }

    /// POST /v1/sessions/{id}/runs/{runID}/revert — restores the files
    /// one run mutated to their pre-turn contents; conflicts report
    /// external modifications that were overwritten (never silently
    /// clobbered). The session must be idle.
    func revertRun(_ id: String, runId: String) async throws -> RevertOutcome {
        try await postEmpty("/v1/sessions/\(id)/runs/\(Self.pathEscaped(runId))/revert")
    }

    private static func pathEscaped(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? value
    }

    // MARK: - Share links

    /// POST /v1/sessions/{id}/share — mints (or returns) the public
    /// read-only link. `url` is present only when the server's LAN
    /// share listener is up; otherwise the caller resolves `path`
    /// against its own base URL (WebUI: url || location.origin + path).
    func shareSession(_ id: String) async throws -> ShareLink {
        try await postEmpty("/v1/sessions/\(id)/share")
    }

    /// DELETE /v1/sessions/{id}/share — the link stops resolving
    /// immediately (WebUI shareSession shift-click).
    func revokeShare(_ id: String) async throws {
        try await requestNoContent("DELETE", "/v1/sessions/\(id)/share")
    }

    // MARK: - Approvals / questions

    /// Mirrors the WebUI's resolveApproval (api.ts): the server
    /// reconstructs the call's identity from the projected approval
    /// card; `ruleHint` carries only the trust flavor — present for
    /// "Always allow" (empty trust) and "Trust" (unsandboxed).
    func resolveApproval(
        _ sessionId: String, approvalId: String,
        callId: String, argsHash: String, decision: ApprovalDecision,
        ruleHint: ApprovalRuleHint? = nil,
    ) async throws {
        struct ApprovalResponse: Decodable { let note: String? }
        var body: [String: JSONValue] = [
            "call_id": .string(callId),
            "args_hash": .string(argsHash),
            "decision": .string(decision.rawValue),
            "client": .string("loom-native"),
        ]
        if let ruleHint {
            var hint: [String: JSONValue] = [:]
            if let trust = ruleHint.trust {
                hint["trust"] = .string(trust)
            }
            body["rule_hint"] = .object(hint)
        }
        let _: ApprovalResponse = try await post(
            "/v1/sessions/\(sessionId)/approvals/\(approvalId)", jsonValue: .object(body),
        )
    }

    func answerQuestion(
        _ sessionId: String, questionId: String,
        selected: [String], customText: String?, skipped: Bool,
    ) async throws {
        struct QuestionResponse: Decodable { let resolved: Bool? }
        var body: [String: JSONValue] = [:]
        if !selected.isEmpty {
            body["selected"] = .array(selected.map { .string($0) })
        }
        if let customText, !customText.isEmpty {
            body["custom_text"] = .string(customText)
        }
        if skipped {
            body["skipped"] = .bool(true)
        }
        let _: QuestionResponse = try await post(
            "/v1/sessions/\(sessionId)/questions/\(questionId)", jsonValue: .object(body),
        )
    }

    // MARK: - Session model / reasoning (composer pickers)

    /// POST /v1/sessions/{id}/model — takes effect on the next turn.
    func setModel(_ id: String, provider: String, model: String) async throws {
        struct SetModelResponse: Decodable { let modelName: String? }
        let response: SetModelResponse = try await post(
            "/v1/sessions/\(id)/model",
            json: ["provider": provider, "model": model],
        )
        _ = response
    }

    /// POST /v1/sessions/{id}/reasoning — effort: default/off/low/medium/high.
    func setReasoning(_ id: String, effort: String) async throws {
        struct SetReasoningResponse: Decodable { let effort: String? }
        let response: SetReasoningResponse = try await post(
            "/v1/sessions/\(id)/reasoning",
            json: ["effort": effort],
        )
        _ = response
    }

    // MARK: - Artifacts

    /// GET /v1/artifacts/{id}?size= — raw bytes plus the response's
    /// sniffed Content-Type (WebUI fetchArtifactURL; historical parts
    /// without media_type fall back to the sniffed type).
    func fetchArtifact(_ id: String, size: Int64?) async throws -> (data: Data, mediaType: String?) {
        var query: [URLQueryItem] = []
        if let size {
            query.append(URLQueryItem(name: "size", value: String(size)))
        }
        var request = authorizedRequest("GET", "/v1/artifacts/\(id)", query: query)
        request.timeoutInterval = 60
        let (data, response) = try await perform(request)
        try checkStatus(response, data: data)
        let sniffed = (response as? HTTPURLResponse)?
            .value(forHTTPHeaderField: "Content-Type")?
            .components(separatedBy: ";").first
        return (data, sniffed)
    }

    // MARK: - Workspaces

    func listWorkspaces() async throws -> WorkspaceListResponse {
        try await get("/v1/workspaces")
    }

    /// Registers a new workspace rooted at an existing directory.
    @discardableResult
    func registerWorkspace(rootPath: String, name: String) async throws -> Workspace {
        struct Response: Decodable { let workspace: Workspace }
        let response: Response = try await post(
            "/v1/workspaces",
            json: ["root_path": rootPath, "name": name],
        )
        return response.workspace
    }

    /// Deletes a workspace (cascades to its sessions; the directory on
    /// disk is left untouched). The default workspace cannot be deleted.
    func deleteWorkspace(_ id: String) async throws {
        try await requestNoContent("DELETE", "/v1/workspaces/\(id)")
    }

    /// Effective approval baseline (live override or config default).
    func workspaceApprovalMode(_ id: String) async throws -> String? {
        struct Response: Decodable { let mode: String? }
        let response: Response = try await get("/v1/workspaces/\(id)/approval-mode")
        return response.mode
    }

    /// Workspace-level approval override; effective next turn, not persisted.
    func setWorkspaceApprovalMode(_ id: String, mode: String) async throws {
        struct Response: Decodable { let mode: String? }
        let response: Response = try await post(
            "/v1/workspaces/\(id)/approval-mode",
            json: ["mode": mode],
        )
        _ = response
    }

    // MARK: - Config (settings panel)

    /// GET /v1/config — the whole config.yaml with secrets masked.
    func getConfig() async throws -> ConfigEnvelope {
        try await get("/v1/config")
    }

    /// PUT /v1/config — full replacement under the optimistic-locking
    /// revision; the response's apply report classifies each changed
    /// section by when it takes effect. 409 config_conflict means the
    /// file changed on disk (reload before re-saving).
    func putConfig(revision: String, config: JSONValue) async throws -> PutConfigResult {
        try await request(
            "PUT", "/v1/config", query: [],
            body: Self.configBodyData(revision: revision, config: config), headers: [:],
        )
    }

    /// Encodes the PUT /v1/config body without Foundation's \/ escape:
    /// the server decodes it with a YAML parser (config.DecodeFileJSON —
    /// "JSON is a YAML subset"), and gopkg.in/yaml.v3 rejects the \/
    /// that JSONEncoder emits for every forward slash ("found unknown
    /// escape character"). Stripping is unambiguous: a literal backslash
    /// is encoded \\, so the byte sequence \/ only ever means an escaped
    /// slash, and JSON structure never contains a bare slash.
    static func configBodyData(revision: String, config: JSONValue) throws -> Data {
        let body = JSONValue.object(["revision": .string(revision), "config": config])
        let text = try String(decoding: LoomJSON.encoder.encode(body), as: UTF8.self)
            .replacingOccurrences(of: "\\/", with: "/")
        return Data(text.utf8)
    }

    /// POST /v1/config/reveal — plaintext of one stored secret, served
    /// on demand so it never rides the whole-config response.
    func revealSecret(_ ref: SecretRef) async throws -> String {
        struct Response: Decodable { let value: String? }
        let response: Response = try await post("/v1/config/reveal", json: ref)
        return response.value ?? ""
    }

    // MARK: - Skills

    func listSkills() async throws -> SkillsOverview {
        try await get("/v1/skills")
    }

    /// Writes skills.disabled into the config file and hot-applies —
    /// the returned revision must be synced into the settings draft.
    func setSkillDisabled(_ name: String, disabled: Bool) async throws -> SkillDisabledResult {
        try await put("/v1/skills/\(name)/disabled", json: ["disabled": disabled])
    }

    /// DELETE /v1/skills — removes the skill's directory by its
    /// SKILL.md path; unrecoverable (the UI confirms first).
    func deleteSkill(path: String) async throws {
        struct Body: Encodable { let path: String }
        struct Response: Decodable { let deleted: Bool? }
        let _: Response = try await request(
            "DELETE", "/v1/skills", query: [],
            body: LoomJSON.encoder.encode(Body(path: path)), headers: ["Content-Type": "application/json"],
        )
    }

    // MARK: - MCP servers

    func listMcpServers() async throws -> [McpServerStatus] {
        let response: McpServersResponse = try await get("/v1/mcp/servers")
        return response.servers ?? []
    }

    @discardableResult
    func reconnectMcpServer(_ name: String) async throws -> McpServerStatus {
        try await postEmpty("/v1/mcp/servers/\(name)/reconnect")
    }

    // MARK: - Rule packs

    func listRulePacks() async throws -> [RulePack] {
        let response: RulePacksResponse = try await get("/v1/rules/packs")
        return response.packs ?? []
    }

    func installRulePack(_ id: String) async throws {
        struct Response: Decodable { let installed: Bool? }
        let _: Response = try await postEmpty("/v1/rules/packs/\(id)/install")
    }

    func uninstallRulePack(_ id: String) async throws {
        try await requestNoContent("DELETE", "/v1/rules/packs/\(id)")
    }

    // MARK: - Meta environment (dev toolchain report)

    func metaEnvironment() async throws -> EnvironmentReport {
        try await get("/v1/meta/environment")
    }

    // MARK: - HTTP plumbing

    private func get<T: Decodable>(_ path: String, query: [URLQueryItem] = []) async throws -> T {
        try await request("GET", path, query: query, body: nil, headers: [:])
    }

    private func put<T: Decodable>(_ path: String, json body: some Encodable) async throws -> T {
        try await request("PUT", path, body: LoomJSON.encoder.encode(body), headers: [:])
    }

    private func post<T: Decodable>(
        _ path: String, json body: some Encodable, headers: [String: String] = [:],
    ) async throws -> T {
        try await request("POST", path, body: LoomJSON.encoder.encode(body), headers: headers)
    }

    private func post<T: Decodable>(_ path: String, jsonValue: JSONValue) async throws -> T {
        try await request("POST", path, body: LoomJSON.encoder.encode(jsonValue), headers: [:])
    }

    private func postEmpty<T: Decodable>(_ path: String) async throws -> T {
        try await request("POST", path, body: nil, headers: [:])
    }

    private func requestNoContent(_ method: String, _ path: String) async throws {
        var request = authorizedRequest(method, path, query: [])
        request.timeoutInterval = 30
        let (data, response) = try await perform(request)
        try checkStatus(response, data: data)
    }

    private func request<T: Decodable>(
        _ method: String, _ path: String,
        query: [URLQueryItem] = [], body: Data?, headers: [String: String],
    ) async throws -> T {
        var request = authorizedRequest(method, path, query: query)
        request.timeoutInterval = 30
        if let body {
            request.httpBody = body
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        for (key, value) in headers {
            request.setValue(value, forHTTPHeaderField: key)
        }
        let (data, response) = try await perform(request)
        try checkStatus(response, data: data)
        do {
            return try LoomJSON.decoder.decode(T.self, from: data)
        } catch {
            throw LoomAPIError.decoding("\(method) \(path): \(error)")
        }
    }

    private func authorizedRequest(_ method: String, _ path: String, query: [URLQueryItem]) -> URLRequest {
        var components = URLComponents(url: baseURL.appendingPathComponent(path), resolvingAgainstBaseURL: false)!
        if !query.isEmpty {
            components.queryItems = query
        }
        var request = URLRequest(url: components.url!)
        request.httpMethod = method
        if !token.isEmpty {
            request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        }
        return request
    }

    private func perform(_ request: URLRequest) async throws -> (Data, URLResponse) {
        do {
            return try await session.data(for: request)
        } catch {
            throw LoomAPIError.transport(error.localizedDescription)
        }
    }

    private func checkStatus(_ response: URLResponse, data: Data) throws {
        guard let http = response as? HTTPURLResponse else { return }
        guard (200 ..< 300).contains(http.statusCode) else {
            let body = try? LoomJSON.decoder.decode(APIErrorBody.self, from: data)
            throw LoomAPIError.http(
                status: http.statusCode, code: body?.error.code, message: body?.error.message,
            )
        }
    }
}

struct ShareLink: Decodable, Sendable {
    let token: String
    let path: String
    let url: String?
}

enum ApprovalDecision: String, Sendable {
    case allow, deny
}

/// rule_hint payload: `trust == nil` remembers the rule for this
/// workspace ("Always allow"); `trust == "unsandboxed"` grants full
/// user permissions ("Trust (no sandbox)").
struct ApprovalRuleHint: Sendable {
    var trust: String?
}
