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

// MARK: - Config (server/handlers_config.go)

/// GET /v1/config response (configView): the whole config.yaml as a
/// schema-less map — the settings panel edits it by dot-path key.
struct ConfigEnvelope: Decodable, Sendable {
    let path: String
    let exists: Bool
    let revision: String
    let config: JSONValue?
}

/// PUT /v1/config response: the new revision plus the hot-apply report
/// classifying each changed section by when it takes effect
/// (app.ConfigApplyReport).
struct PutConfigResult: Decodable, Sendable {
    let path: String?
    let revision: String?
    let applied: ApplyReport?

    struct ApplyReport: Decodable, Sendable {
        let immediate: [String]?
        let nextTurn: [String]?
        let restart: [String]?

        enum CodingKeys: String, CodingKey {
            case immediate
            case nextTurn = "next_turn"
            case restart
        }
    }
}

/// POST /v1/config/reveal request (secretReveal): names one stored
/// secret by its structural location. The GET response only carries
/// SecretMask placeholders; plaintext is served on demand only.
struct SecretRef: Encodable, Sendable, Equatable {
    /// provider | tracing | mcp_header | knowledge_base
    var kind: String
    /// provider name / MCP server name; empty for tracing/knowledge_base.
    var name: String?
    /// tracing: public_key|secret_key; mcp_header: header name.
    var field: String?
}

/// Keep in sync with SecretMask in internal/config/edit.go.
let secretMask = "••••••••••"

// MARK: - Skills (app/skills.go SkillsOverview)

/// GET /v1/skills response: the aggregated skill catalog — the shared
/// user-scope group plus every workspace's repo-scope skills.
struct SkillsOverview: Decodable, Sendable {
    let enabled: Bool?
    let reason: String?
    let groups: [SkillGroup]?

    struct SkillGroup: Decodable, Sendable {
        let workspaceId: String?
        let workspaceName: String
        let root: String?
        let shared: Bool?
        let skills: [SkillInfo]?
        let issues: [String]?

        enum CodingKeys: String, CodingKey {
            case workspaceId = "workspace_id"
            case workspaceName = "workspace_name"
            case root, shared, skills, issues
        }
    }

    struct SkillInfo: Decodable, Sendable, Identifiable {
        let name: String
        let description: String?
        let scope: String?
        let path: String
        let disabled: Bool?

        var id: String { path }
    }
}

/// PUT /v1/skills/{name}/disabled response: the endpoint rewrote the
/// config file — the caller syncs revision and skills.disabled into its
/// own draft baseline (else a later settings save 409-conflicts).
struct SkillDisabledResult: Decodable, Sendable {
    let revision: String?
    let disabled: [String]?
}

// MARK: - MCP (server/handlers_mcp.go)

struct McpServersResponse: Decodable, Sendable {
    let servers: [McpServerStatus]?
}

struct McpServerStatus: Decodable, Sendable, Identifiable {
    let name: String
    let connected: Bool?
    let error: String?
    let tools: [McpTool]?

    var id: String { name }

    struct McpTool: Decodable, Sendable {
        let name: String
        let description: String?
    }
}

// MARK: - Rule packs (server/handlers_packs.go)

struct RulePacksResponse: Decodable, Sendable {
    let packs: [RulePack]?
}

struct RulePack: Decodable, Sendable, Identifiable {
    let id: String
    let name: String
    let risk: String?
    let description: String?
    let reason: String?
    let commands: [String]?
    let installed: Bool?
}

// MARK: - Environment report (GET /v1/meta/environment)

/// Read-only dev-toolchain discovery report (the system tab's card):
/// found/missing tools, PATH assembly per directory.
struct EnvironmentReport: Decodable, Sendable {
    let tools: [EnvTool]?
    let dirs: [EnvDir]?
    let effectivePath: String?

    enum CodingKeys: String, CodingKey {
        case tools, dirs
        case effectivePath = "effective_path"
    }

    struct EnvTool: Decodable, Sendable, Identifiable {
        let name: String
        let found: Bool?
        let path: String?

        var id: String { name }
    }

    struct EnvDir: Decodable, Sendable, Identifiable {
        let path: String
        let source: String?
        let status: String?

        var id: String { path }
    }
}
