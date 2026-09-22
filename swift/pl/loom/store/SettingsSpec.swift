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

// Declarative field specs for the settings panel — a faithful port of
// the WebUI's settings/spec.ts + convert.ts + cfgpath.ts. `key` is the
// dot-path in config.yaml; the same spec drives rendering (FieldRow)
// and collection (save). Empty-value semantics match the file:
// blank = key not written (omitempty); all defaults are implicit.

// MARK: - Control state (convert.ts ControlState)

/// Control state: text-like types are always text; bool/flag-list are
/// flag. Kept raw (unparsed) so editing never loses precision.
enum ControlState: Equatable, Sendable {
    case text(String)
    case flag(Bool)

    var textValue: String {
        if case let .text(value) = self {
            return value
        }
        return ""
    }

    var flagValue: Bool {
        if case let .flag(value) = self {
            return value
        }
        return false
    }
}

// MARK: - Field spec (spec.ts FieldSpec)

struct FieldSpec: Sendable {
    enum FieldType: Sendable {
        case text
        case password
        case number
        case bool
        case tristate
        case select
        case textarea
        case listText // one item per line → []string
        case kvText // one k=v per line → map
        case pairList // one "name: description" per line → [{name, description}]
        case floatList // comma-separated → []number
        case flagList // checked → write the fixed []string (flagValue)
    }

    let key: String
    var label: String?
    var hint: String?
    var ph: String?
    var type: FieldType = .text
    var options: [(String, String)]?
    var optionHints: [String: String]?
    var rows: Int?
    /// Default-value label shown in the row ("auto" / "off" / …).
    var def: String?
    var required = false
    var revealRef: SecretRef?
    var flagValue: [String]?

    init(
        _ key: String,
        label: String? = nil,
        hint: String? = nil,
        ph: String? = nil,
        type: FieldType = .text,
        options: [(String, String)]? = nil,
        optionHints: [String: String]? = nil,
        rows: Int? = nil,
        def: String? = nil,
        required: Bool = false,
        revealRef: SecretRef? = nil,
        flagValue: [String]? = nil,
    ) {
        self.key = key
        self.label = label
        self.hint = hint
        self.ph = ph
        self.type = type
        self.options = options
        self.optionHints = optionHints
        self.rows = rows
        self.def = def
        self.required = required
        self.revealRef = revealRef
        self.flagValue = flagValue
    }
}

struct TabSpec: Sendable {
    let id: String
    let label: String
    /// SF Symbol name.
    let icon: String
    var sections: [(String, [FieldSpec])]?
}

// MARK: - Spec data (spec.ts)

let skillsEmptyHint =
    "No skills found. Directory conventions: workspace .loom/skills/ and .agents/skills/; user-level ~/.loom/skills/ and ~/.agents/skills/."

private let effortOptions: [(String, String)] = [
    ("", "default (provider decides)"),
    ("off", "off"),
    ("low", "low"),
    ("medium", "medium"),
    ("high", "high"),
]

let reasoningFields: [FieldSpec] = [
    FieldSpec("reasoning.effort", label: "Reasoning Effort", type: .select, options: effortOptions),
    FieldSpec(
        "reasoning.budget_tokens", label: "Reasoning Token Budget",
        hint: "explicit budget; takes precedence over the effort-derived value when > 0",
        ph: "0", type: .number,
    ),
]

let providerBaseFields: [FieldSpec] = [
    FieldSpec(
        "type", label: "Protocol Type", type: .select,
        options: [("openai", "openai (compatible gateway)"), ("anthropic", "anthropic (Messages API)")],
    ),
    FieldSpec("base_url", label: "Base URL", ph: "https://api.deepseek.com/v1", required: true),
    FieldSpec(
        "api_key", label: "API Key",
        hint: "mutually exclusive with \"Key Env Var\"; setting both is an error",
        type: .password,
    ),
    FieldSpec(
        "api_key_env", label: "Key Env Var",
        hint: "stores the variable name only; the value is read at startup",
        ph: "e.g. DEEPSEEK_API_KEY",
    ),
    FieldSpec("default_model", label: "Default Model", ph: "empty = first model in the catalog"),
]

let providerAdvFields: [FieldSpec] = [
    FieldSpec(
        "wire_api", label: "Wire API", type: .select,
        options: [
            ("", "default"),
            ("chat", "chat (Chat Completions)"),
            ("responses", "responses (Responses API)"),
            ("messages", "messages (anthropic only)"),
        ],
    ),
    FieldSpec(
        "auth_type", label: "Auth Header",
        hint: "anthropic type only",
        type: .select,
        options: [("", "default (x-api-key)"), ("x-api-key", "x-api-key"), ("bearer", "bearer")],
    ),
    FieldSpec("api_version", label: "API Version Header", hint: "anthropic type only; empty = built-in version"),
    FieldSpec("max_retries", label: "Max Retries", ph: "2", type: .number),
] + reasoningFields

/// Every field of a provider card (base + advanced + name), the full
/// set walked by fill/validate/collect.
let providerAllFields: [FieldSpec] = providerBaseFields + providerAdvFields + [FieldSpec("name")]

let modelFields: [FieldSpec] = [
    FieldSpec("name", label: "Model Name", ph: "e.g. deepseek-chat", required: true),
    FieldSpec("context_window", label: "Context Window", ph: "e.g. 65536", type: .number),
    FieldSpec("max_output_tokens", label: "Max Output Tokens", ph: "e.g. 8192", type: .number),
    FieldSpec(
        "modalities", label: "Image Input (multimodal)",
        hint: "check only when the model truly accepts image input (writes modalities: [text, image]); enables pasting/dropping images into the composer — gateways reject image input for text-only models",
        type: .flagList,
        flagValue: ["text", "image"],
    ),
    FieldSpec(
        "wire_api", label: "Wire API Override", type: .select,
        options: [("", "follow provider"), ("chat", "chat"), ("responses", "responses")],
    ),
    FieldSpec("window_utilization", label: "Window Utilization Override", ph: "follow global", type: .number),
] + reasoningFields

let mcpStdioFields: [FieldSpec] = [
    FieldSpec("command", label: "Command", ph: "e.g. npx", required: true),
    FieldSpec("args", label: "Arguments", hint: "one argument per line", type: .listText),
    FieldSpec("env", label: "Environment", hint: "one KEY=VALUE per line (appended to the process environment)", type: .kvText),
    FieldSpec("cwd", label: "Working Directory", hint: "empty = inherit loom's working directory"),
]

let mcpHTTPFields: [FieldSpec] = [
    FieldSpec("url", label: "URL", ph: "https://mcp.example.com/mcp", required: true),
    FieldSpec(
        "headers", label: "Headers",
        hint: "one KEY=VALUE per line; values support ${VAR} references",
        type: .kvText,
    ),
]

let mcpCommonFields: [FieldSpec] = [
    FieldSpec("startup_timeout_sec", label: "Startup Timeout (s)", ph: "30", type: .number),
    FieldSpec("tool_timeout_sec", label: "Tool Call Timeout (s)", ph: "300", type: .number),
    FieldSpec("enabled_tools", label: "Tool Allowlist", hint: "empty = register all tools", type: .listText),
    FieldSpec("disabled_tools", label: "Tool Denylist", type: .listText),
]

let skillsConfigFields: [FieldSpec] = [
    FieldSpec("skills.enabled", label: "Enable Skills", type: .tristate),
    FieldSpec(
        "skills.extra_roots", label: "Extra Search Roots",
        hint: "one directory per line; a leading ~ expands to the home directory",
        type: .listText,
    ),
]

let defaultModelField = FieldSpec(
    "default", label: "Default Model",
    hint: "empty = the first provider's default model",
    ph: "provider/model",
)

let settingsTabs: [TabSpec] = [
    TabSpec(id: "providers", label: "Models", icon: "square.stack.3d.up"),
    TabSpec(id: "limits", label: "Limits & Guards", icon: "shield", sections: [
        ("Run Budget", [
            FieldSpec(
                "limits.max_input_tokens", label: "Fallback Context Window",
                hint: "used when the model declares no context_window",
                ph: "200000", type: .number,
            ),
            FieldSpec("limits.max_output_tokens", label: "Max Output Tokens", ph: "16384", type: .number),
            FieldSpec(
                "limits.max_cost_usd", label: "Cost Limit (USD)",
                hint: "per-session cumulative estimated cost; 0 = unlimited (requires cost-rate tracking)",
                ph: "5.0", type: .number,
            ),
            FieldSpec(
                "limits.max_tokens", label: "Total Token Budget",
                hint: "per-session cumulative tokens; 0 = unlimited",
                ph: "0", type: .number,
            ),
            FieldSpec("limits.max_tool_output_bytes", label: "Tool Output Retained Bytes", ph: "49152", type: .number),
            FieldSpec("limits.max_artifact_bytes", label: "Max Artifact Bytes", ph: "104857600", type: .number),
        ]),
        ("Context Compaction", [
            FieldSpec("context.utilization", label: "Window Utilization", ph: "0.95", type: .number),
            FieldSpec("context.compact_trigger_ratio", label: "Compact Trigger Ratio", ph: "0.80", type: .number),
            FieldSpec(
                "context.compact_target_ratio", label: "Compact Target Ratio",
                hint: "must be below the trigger ratio",
                ph: "0.50", type: .number,
            ),
            FieldSpec(
                "context.notice_levels", label: "Occupancy Notice Levels",
                hint: "comma-separated, ascending and below the trigger ratio",
                ph: "0.60, 0.75", type: .floatList,
            ),
        ]),
        ("Runaway Detection", [
            FieldSpec("runaway.max_repeated_calls", label: "Max Repeated Calls", ph: "3", type: .number),
            FieldSpec("runaway.max_consecutive_failures", label: "Max Consecutive Failures", ph: "5", type: .number),
            FieldSpec(
                "runaway.stall_warn_turns", label: "Stall Warning Turns",
                hint: "0 = off",
                ph: "10", type: .number,
            ),
            FieldSpec(
                "runaway.stall_timeout", label: "Stall Watchdog",
                hint: "Go duration syntax; 0 = off",
                ph: "15m",
            ),
        ]),
    ]),
    TabSpec(id: "permission", label: "Permissions & Approvals", icon: "lock", sections: [
        ("Approval Baseline", [
            FieldSpec(
                "approval.mode", label: "Approval Mode",
                hint: "decision policy when no rule or memory matches",
                type: .select,
                options: [
                    ("", "default (on-request)"),
                    ("on-request", "on-request · free within sandbox/workspace"),
                    ("danger-only", "danger-only · prompt only for dangerous operations"),
                    ("never", "never · unattended"),
                ],
                optionHints: [
                    "": "on-request (default): commands inside the sandbox and reads/writes inside the workspace are auto-allowed; sandbox escalation, out-of-workspace writes, external network, and danger signals prompt",
                    "on-request": "commands inside the sandbox and reads/writes inside the workspace are auto-allowed; sandbox escalation, out-of-workspace writes, external network, and danger signals prompt",
                    "danger-only": "only explicitly dangerous operations prompt: dangerous-site denylist, dangerous patterns (curl|sh, credential/startup-file writes, …), and destructive or shared-state consequences (rm of key targets, git push, …); development commands, normal sites/APIs, and sandbox escalation are auto-allowed",
                    "never": "unattended: allowed inside the sandbox; escalation, out-of-workspace writes, and destructive/shared-state operations are denied outright — never blocks waiting for approval",
                ],
            ),
        ]),
        ("Rule Layers", [
            FieldSpec("rules.enabled", label: "Enable Rules", type: .tristate),
            FieldSpec("rules.builtin", label: "Built-in Read-only Commands", type: .tristate),
            FieldSpec("rules.project", label: "Project Rules", type: .tristate),
            FieldSpec(
                "rules.project_allow", label: "Allow Rules from the Project Layer",
                hint: "untrusted repos may only tighten, never loosen",
                type: .tristate,
                def: "off",
            ),
            FieldSpec(
                "rules.persist_remembered", label: "Persist \"Always Allow\"",
                hint: "written to the user-level rules file for future sessions to inherit",
                type: .tristate,
            ),
        ]),
    ]),
    TabSpec(id: "agent", label: "Agent", icon: "brain", sections: [
        ("System Prompt", [
            FieldSpec("prompt.extra", label: "Extra Instructions", hint: "appended to the end of the built-in system prompt", type: .textarea),
            FieldSpec("prompt.disable_builtin", label: "Disable Built-in Prompt", type: .bool),
            FieldSpec("prompt.managed.name", label: "Managed Prompt Name", hint: "Langfuse-managed prompt (requires tracing)"),
            FieldSpec("prompt.managed.label", label: "Managed Prompt Label", ph: "production"),
        ]),
        ("Subagents", [
            FieldSpec("subagent.enabled", label: "Enable Subagents", type: .tristate),
            FieldSpec("subagent.model", label: "Pinned Model", hint: "empty = follow the current turn's model", ph: "provider/model"),
            FieldSpec(
                "subagent.max_tokens", label: "Token Limit",
                hint: "0 = inherit the run budget",
                ph: "0", type: .number,
            ),
            FieldSpec("subagent.max_output_tokens", label: "Max Output Tokens", ph: "8192", type: .number),
        ]),
        ("Long-term Memory", [
            FieldSpec("memory.enabled", label: "Enable Memory", type: .tristate),
            FieldSpec(
                "memory.extract_model", label: "Extraction Model",
                hint: "a cheap, fast model is recommended; empty = follow the default model",
                ph: "provider/model",
            ),
            FieldSpec("memory.consolidation_model", label: "Consolidation Model", ph: "provider/model"),
            FieldSpec("memory.max_jobs_per_run", label: "Max Jobs per Run", ph: "8", type: .number),
            FieldSpec(
                "memory.run_interval", label: "Pipeline Interval",
                hint: "0 = run once at startup only",
                ph: "30m",
            ),
            FieldSpec("memory.min_session_idle", label: "Session Idle Threshold", ph: "1h"),
            FieldSpec("memory.max_session_age", label: "Max Session Age", ph: "720h"),
        ]),
        ("Session Archiving", [
            FieldSpec(
                "sessions.auto_archive_after", label: "Auto-archive After",
                hint: "sessions idle longer than this are archived automatically (read-only; can be unarchived at any time); empty or 0 = off",
                ph: "e.g. 720h",
            ),
            FieldSpec(
                "sessions.gc_archived_after", label: "Archived Retention",
                hint: "sessions archived longer than this are permanently deleted (including events, checkpoints, and file-change history); empty or 0 = keep forever",
                ph: "e.g. 720h",
            ),
        ]),
        ("Text-to-image", [
            FieldSpec(
                "image.enabled", label: "Enable Text-to-image",
                hint: "default: enabled when both provider and model are set",
                type: .tristate,
                def: "auto",
            ),
            FieldSpec("image.provider", label: "Credential Provider", hint: "reuses its base_url/api_key (must be an openai-type provider)"),
            FieldSpec("image.model", label: "Image Model"),
            FieldSpec("image.size", label: "Default Size", ph: "e.g. 1024x1024"),
            FieldSpec(
                "image.quality", label: "Default Quality", type: .select,
                options: [("", "auto"), ("low", "low"), ("medium", "medium"), ("high", "high")],
            ),
        ]),
    ]),
    TabSpec(id: "skills", label: "Skills", icon: "puzzlepiece"),
    TabSpec(id: "mcp", label: "MCP", icon: "cable.connector"),
    TabSpec(id: "kb", label: "Knowledge Base", icon: "cylinder", sections: [
        ("Connection", [
            FieldSpec(
                "knowledge_base.enabled", label: "Enable Knowledge Base",
                hint: "auto = enabled when base_url is set; changes require a restart",
                type: .tristate,
                def: "auto",
            ),
            FieldSpec(
                "knowledge_base.base_url", label: "Service URL",
                hint: "minisearch v2 REST address",
                ph: "http://127.0.0.1:8200",
                required: true,
            ),
            FieldSpec(
                "knowledge_base.api_key", label: "API Key",
                hint: "minisearch bearer token (msk_…); leave empty with --auth=off",
                type: .password,
                revealRef: SecretRef(kind: "knowledge_base"),
            ),
            FieldSpec(
                "knowledge_base.timeout_ms", label: "Request Timeout (ms)",
                hint: "range 1000–60000",
                ph: "10000", type: .number,
            ),
        ]),
        ("Retrieval", [
            FieldSpec(
                "knowledge_base.default_top_k", label: "Default Top K",
                hint: "range 1–20",
                ph: "5", type: .number,
            ),
            FieldSpec("knowledge_base.default_collection", label: "Default Collection", ph: "empty = the first collection"),
            FieldSpec(
                "knowledge_base.collections", label: "Collections",
                hint: "at least one; descriptions go into the tool schema to help the model route by topic",
                ph: "name: description (one per line)",
                type: .pairList,
                rows: 4,
                required: true,
            ),
        ]),
    ]),
    TabSpec(id: "system", label: "System", icon: "gear", sections: [
        ("Dev Toolchain", [
            FieldSpec(
                "tools.path_extra", label: "Extra PATH Directories",
                hint: "one absolute path per line (~/ prefix supported); takes precedence over all built-in candidate directories; hot-applied on save",
                ph: "~/corp/bin",
                type: .listText,
                rows: 3,
            ),
        ]),
        ("Langfuse Tracing", [
            FieldSpec("tracing.host", label: "Service URL", ph: "https://langfuse.internal"),
            FieldSpec(
                "tracing.public_key", label: "Public Key", type: .password,
                revealRef: SecretRef(kind: "tracing", field: "public_key"),
            ),
            FieldSpec("tracing.public_key_env", label: "Public Key Env Var"),
            FieldSpec(
                "tracing.secret_key", label: "Secret Key", type: .password,
                revealRef: SecretRef(kind: "tracing", field: "secret_key"),
            ),
            FieldSpec("tracing.secret_key_env", label: "Secret Key Env Var"),
            FieldSpec("tracing.environment", label: "Environment Tag", ph: "dev"),
            FieldSpec("tracing.include_content", label: "Send Conversation Content", type: .tristate),
            FieldSpec("tracing.user", label: "Owning User", hint: "empty = git user.email, then $USER"),
            FieldSpec("tracing.cost_input_usd_per_mtok", label: "Input Rate (USD/Mtok)", ph: "0", type: .number),
            FieldSpec("tracing.cost_output_usd_per_mtok", label: "Output Rate (USD/Mtok)", ph: "0", type: .number),
        ]),
        ("LAN Sharing", [
            FieldSpec(
                "share.enabled", label: "Enable LAN Sharing",
                hint: "takes effect immediately on save (hot-applied); the listener only exposes the read-only share page, no admin API",
                type: .tristate,
                def: "off",
            ),
            FieldSpec(
                "share.listen", label: "Listen Address",
                hint: "a fixed port keeps share links alive across restarts; 0.0.0.0 = all interfaces, or bind a specific interface IP",
                ph: "0.0.0.0:7681",
            ),
        ]),
        ("Logging", [
            FieldSpec("logging.max_file_mb", label: "Max Log File Size (MiB)", ph: "2048", type: .number),
            FieldSpec("logging.max_total_mb", label: "Max Total Log Size (MiB)", ph: "10240", type: .number),
        ]),
        ("Browser", [
            FieldSpec(
                "browser.enabled", label: "Enable Browser Tools",
                hint: "enabled by default; browser tools are not registered when off",
                type: .tristate,
                def: "on",
            ),
            FieldSpec(
                "browser.chrome_path", label: "Chrome Path",
                hint: "path to the Chrome/Chromium binary; empty = auto-detect common system locations",
                ph: "empty = auto-detect",
            ),
            FieldSpec(
                "browser.cdp_url", label: "CDP Remote URL",
                hint: "remote Chrome DevTools Protocol address; connects to an external Chrome instead of launching a local one (can bypass anti-bot checks)",
                ph: "ws://127.0.0.1:9222",
            ),
            FieldSpec(
                "browser.idle_ttl", label: "Idle TTL",
                hint: "browser instances idle longer than this are closed automatically (Go duration syntax)",
                ph: "5m",
            ),
            FieldSpec(
                "browser.nav_timeout_ms", label: "Navigation Timeout (ms)",
                hint: "page navigation timeout, range 5000–120000",
                ph: "30000", type: .number,
            ),
            FieldSpec(
                "browser.screenshot_quality", label: "Screenshot Quality",
                hint: "JPEG quality, range 10–100",
                ph: "80", type: .number,
            ),
            FieldSpec("browser.viewport_width", label: "Viewport Width", ph: "1280", type: .number),
            FieldSpec("browser.viewport_height", label: "Viewport Height", ph: "720", type: .number),
        ]),
        ("Terminal UI (TUI)", [
            FieldSpec(
                "ui.icons", label: "Icon Set", type: .select,
                options: [("", "default (nerd)"), ("nerd", "nerd (Nerd Font)"), ("plain", "plain (text only)")],
            ),
            FieldSpec("ui.alt_screen", label: "Use Alternate Screen", hint: "restores the scrollback on exit", type: .bool),
        ]),
    ]),
]

/// Registry of all global-scope fields (collected by spec on save):
/// the providers tab's startup model, the skills config section, and
/// every sections-driven tab field.
func globalFieldSpecs() -> [FieldSpec] {
    var out: [FieldSpec] = [defaultModelField] + skillsConfigFields
    for tab in settingsTabs {
        for (_, fields) in tab.sections ?? [] {
            out.append(contentsOf: fields)
        }
    }
    return out
}

// MARK: - Key paths (cfgpath.ts)

func getPath(_ obj: [String: JSONValue], _ path: String) -> JSONValue? {
    var current: JSONValue? = .object(obj)
    for key in path.split(separator: ".") {
        guard let value = current?[String(key)] else { return nil }
        current = value
    }
    return current
}

func setPath(_ obj: inout [String: JSONValue], _ path: String, _ value: JSONValue) {
    var keys = path.split(separator: ".").map(String.init)
    guard let last = keys.popLast() else { return }
    setPath(&obj, keys: keys, last: last, value: value)
}

private func setPath(
    _ obj: inout [String: JSONValue], keys: [String], last: String, value: JSONValue,
) {
    guard let head = keys.first else {
        obj[last] = value
        return
    }
    var child: [String: JSONValue] = if case let .object(existing) = obj[head] {
        existing
    } else {
        [:]
    }
    setPath(&child, keys: Array(keys.dropFirst()), last: last, value: value)
    obj[head] = .object(child)
}

/// UI-unmanaged config paths carried back verbatim on save (merge
/// semantics: "unprovided key = removed from the file"); top-level keys
/// outside KNOWN_TOP_KEYS (future config sections) are likewise always
/// preserved — UI completeness is not a precondition of correctness.
let preservePaths = ["ui.keymap", "skills.disabled"]

let knownTopKeys: Set<String> = [
    "default", "providers", "limits", "context", "runaway", "prompt", "skills",
    "rules", "approval", "tracing", "share", "logging", "ui", "subagent",
    "memory", "sessions", "image", "browser", "knowledge_base", "mcp_servers",
    "workspaces",
]

func preserveUnmanaged(_ cfg: inout [String: JSONValue], orig: [String: JSONValue]) {
    for (key, value) in orig where !knownTopKeys.contains(key) && cfg[key] == nil {
        cfg[key] = value
    }
    for path in preservePaths {
        if let value = getPath(orig, path), getPath(cfg, path) == nil {
            setPath(&cfg, path, value)
        }
    }
}

// MARK: - Fill / collect (convert.ts)

/// Converts a config value to control state (fills on load).
func fillValue(_ spec: FieldSpec, _ value: JSONValue?) -> ControlState {
    guard let value, value != .null else {
        return spec.type == .bool || spec.type == .flagList ? .flag(false) : .text("")
    }
    switch spec.type {
    case .bool:
        if case let .bool(b) = value {
            return .flag(b)
        }
        return .flag(false)
    case .flagList:
        if case let .array(items) = value {
            return .flag(!items.isEmpty)
        }
        return .flag(false)
    case .tristate:
        if case let .bool(b) = value {
            return .text(b ? "true" : "false")
        }
        if let s = value.stringValue {
            return .text(s)
        }
        return .text("")
    case .listText:
        if case let .array(items) = value {
            return .text(items.compactMap(\.stringValue).joined(separator: "\n"))
        }
        return .text("")
    case .pairList:
        if case let .array(items) = value {
            let lines = items.compactMap { item -> String? in
                guard let name = item["name"]?.stringValue else { return nil }
                if let desc = item["description"]?.stringValue, !desc.isEmpty {
                    return "\(name): \(desc)"
                }
                return name
            }
            return .text(lines.joined(separator: "\n"))
        }
        return .text("")
    case .kvText:
        if case let .object(map) = value {
            return .text(map
                .sorted { $0.key < $1.key }
                .map { "\($0.key)=\($0.value.stringValue ?? "")" }
                .joined(separator: "\n"))
        }
        return .text("")
    case .floatList:
        if case let .array(items) = value {
            return .text(items.compactMap(\.numberText).joined(separator: ", "))
        }
        return .text("")
    default:
        if let s = value.stringValue {
            return .text(s)
        }
        if let n = value.numberText {
            return .text(n)
        }
        return .text("")
    }
}

/// Collects control state into obj[key] (on save); empty values do not
/// write the key.
func collectValue(_ spec: FieldSpec, _ state: ControlState, into obj: inout [String: JSONValue]) {
    switch spec.type {
    case .password:
        // Secrets are not trimmed; the masked placeholder rides back
        // verbatim and the server restores the real value on its side.
        if !state.textValue.isEmpty {
            setPath(&obj, spec.key, .string(state.textValue))
        }
    case .number:
        let trimmed = state.textValue.trimmingCharacters(in: .whitespaces)
        if !trimmed.isEmpty {
            if let int = Int64(trimmed) {
                setPath(&obj, spec.key, .int(int))
            } else if let double = Double(trimmed) {
                setPath(&obj, spec.key, .double(double))
            }
        }
    case .bool:
        // false = default, not written.
        if state.flagValue {
            setPath(&obj, spec.key, .bool(true))
        }
    case .flagList:
        // Unchecked = default (key omitted).
        if state.flagValue {
            setPath(&obj, spec.key, .array((spec.flagValue ?? []).map { .string($0) }))
        }
    case .tristate:
        if !state.textValue.isEmpty {
            setPath(&obj, spec.key, .bool(state.textValue == "true"))
        }
    case .select:
        if !state.textValue.isEmpty {
            setPath(&obj, spec.key, .string(state.textValue))
        }
    case .listText:
        let items = lines(state.textValue)
        if !items.isEmpty {
            setPath(&obj, spec.key, .array(items.map { .string($0) }))
        }
    case .pairList:
        let items: [JSONValue] = lines(state.textValue).map { line in
            if let index = line.firstIndex(of: ":"), index > line.startIndex {
                let name = line[line.startIndex ..< index].trimmingCharacters(in: .whitespaces)
                let desc = line[line.index(after: index)...].trimmingCharacters(in: .whitespaces)
                return .object(["name": .string(name), "description": .string(desc)])
            }
            return .object(["name": .string(line)])
        }
        if !items.isEmpty {
            setPath(&obj, spec.key, .array(items))
        }
    case .kvText:
        var map: [String: JSONValue] = [:]
        for line in state.textValue.split(separator: "\n", omittingEmptySubsequences: false) {
            guard let index = line.firstIndex(of: "="), index > line.startIndex else { continue }
            let key = line[line.startIndex ..< index].trimmingCharacters(in: .whitespaces)
            guard !key.isEmpty else { continue }
            map[key] = .string(line[line.index(after: index)...].trimmingCharacters(in: .whitespaces))
        }
        if !map.isEmpty {
            setPath(&obj, spec.key, .object(map))
        }
    case .floatList:
        let nums = state.textValue
            .split { $0 == "," || $0.isWhitespace }
            .compactMap { Double($0) }
        if !nums.isEmpty {
            setPath(&obj, spec.key, .array(nums.map { .double($0) }))
        }
    default:
        let trimmed = state.textValue.trimmingCharacters(in: .whitespaces)
        if !trimmed.isEmpty {
            setPath(&obj, spec.key, .string(trimmed))
        }
    }
}

/// collectFields: a group of fields in the same scope (cards/groups).
func collectFields(_ specs: [FieldSpec], _ states: [String: ControlState], into obj: inout [String: JSONValue]) {
    for spec in specs {
        let state = states[spec.key]
            ?? (spec.type == .bool || spec.type == .flagList ? .flag(false) : .text(""))
        collectValue(spec, state, into: &obj)
    }
}

private func lines(_ text: String) -> [String] {
    text.split(separator: "\n", omittingEmptySubsequences: false)
        .map { $0.trimmingCharacters(in: .whitespaces) }
        .filter { !$0.isEmpty }
}

extension JSONValue {
    /// JS String(number) parity for fill: 5 → "5", 0.95 → "0.95".
    var numberText: String? {
        switch self {
        case let .int(i): return "\(i)"
        case let .double(d):
            if d == d.rounded(), abs(d) < 1e15 {
                return "\(Int64(d))"
            }
            return "\(d)"
        default: return nil
        }
    }
}
