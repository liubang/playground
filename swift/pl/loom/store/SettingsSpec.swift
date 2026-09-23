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
    "未发现任何技能。目录约定：工作区 .loom/skills/ 和 .agents/skills/；用户级 ~/.loom/skills/ 和 ~/.agents/skills/。"

private let effortOptions: [(String, String)] = [
    ("", "默认（由 provider 决定）"),
    ("off", "off"),
    ("low", "low"),
    ("medium", "medium"),
    ("high", "high"),
]

let reasoningFields: [FieldSpec] = [
    FieldSpec("reasoning.effort", label: "推理强度", type: .select, options: effortOptions),
    FieldSpec(
        "reasoning.budget_tokens", label: "推理 Token 预算",
        hint: "显式预算；大于 0 时优先于按推理强度推导的值",
        ph: "0", type: .number,
    ),
]

let providerBaseFields: [FieldSpec] = [
    FieldSpec(
        "type", label: "协议类型", type: .select,
        options: [("openai", "openai（兼容网关）"), ("anthropic", "anthropic（Messages API）")],
    ),
    FieldSpec("base_url", label: "Base URL", ph: "https://api.deepseek.com/v1", required: true),
    FieldSpec(
        "api_key", label: "API 密钥",
        hint: "与「密钥环境变量」互斥，同时设置会报错",
        type: .password,
    ),
    FieldSpec(
        "api_key_env", label: "密钥环境变量",
        hint: "只保存变量名，变量值在启动时读取",
        ph: "如 DEEPSEEK_API_KEY",
    ),
    FieldSpec("default_model", label: "默认模型", ph: "留空 = 目录中的第一个模型"),
]

let providerAdvFields: [FieldSpec] = [
    FieldSpec(
        "wire_api", label: "Wire API", type: .select,
        options: [
            ("", "默认"),
            ("chat", "chat (Chat Completions)"),
            ("responses", "responses (Responses API)"),
            ("messages", "messages（仅 anthropic）"),
        ],
    ),
    FieldSpec(
        "auth_type", label: "认证头",
        hint: "仅 anthropic 类型",
        type: .select,
        options: [("", "默认（x-api-key）"), ("x-api-key", "x-api-key"), ("bearer", "bearer")],
    ),
    FieldSpec("api_version", label: "API 版本头", hint: "仅 anthropic 类型；留空 = 内置版本"),
    FieldSpec("max_retries", label: "最大重试次数", ph: "2", type: .number),
] + reasoningFields

/// Every field of a provider card (base + advanced + name), the full
/// set walked by fill/validate/collect.
let providerAllFields: [FieldSpec] = providerBaseFields + providerAdvFields + [FieldSpec("name")]

let modelFields: [FieldSpec] = [
    FieldSpec("name", label: "模型名称", ph: "如 deepseek-chat", required: true),
    FieldSpec("context_window", label: "上下文窗口", ph: "如 65536", type: .number),
    FieldSpec("max_output_tokens", label: "最大输出 Token 数", ph: "如 8192", type: .number),
    FieldSpec(
        "modalities", label: "图像输入（多模态）",
        hint: "仅在模型确实支持图像输入时勾选（写入 modalities: [text, image]）；勾选后可在输入框粘贴/拖入图片——纯文本模型会被网关拒绝图像输入",
        type: .flagList,
        flagValue: ["text", "image"],
    ),
    FieldSpec(
        "wire_api", label: "Wire API 覆盖", type: .select,
        options: [("", "跟随 provider"), ("chat", "chat"), ("responses", "responses")],
    ),
    FieldSpec("window_utilization", label: "窗口利用率覆盖", ph: "跟随全局", type: .number),
] + reasoningFields

let mcpStdioFields: [FieldSpec] = [
    FieldSpec("command", label: "命令", ph: "如 npx", required: true),
    FieldSpec("args", label: "参数", hint: "每行一个参数", type: .listText),
    FieldSpec("env", label: "环境变量", hint: "每行一个 KEY=VALUE（追加到进程环境）", type: .kvText),
    FieldSpec("cwd", label: "工作目录", hint: "留空 = 继承 loom 的工作目录"),
]

let mcpHTTPFields: [FieldSpec] = [
    FieldSpec("url", label: "URL", ph: "https://mcp.example.com/mcp", required: true),
    FieldSpec(
        "headers", label: "请求头",
        hint: "每行一个 KEY=VALUE；值支持 ${VAR} 引用",
        type: .kvText,
    ),
]

let mcpCommonFields: [FieldSpec] = [
    FieldSpec("startup_timeout_sec", label: "启动超时（秒）", ph: "30", type: .number),
    FieldSpec("tool_timeout_sec", label: "工具调用超时（秒）", ph: "300", type: .number),
    FieldSpec("enabled_tools", label: "工具允许列表", hint: "留空 = 注册全部工具", type: .listText),
    FieldSpec("disabled_tools", label: "工具拒绝列表", type: .listText),
]

let skillsConfigFields: [FieldSpec] = [
    FieldSpec("skills.enabled", label: "启用技能", type: .tristate),
    FieldSpec(
        "skills.extra_roots", label: "额外搜索目录",
        hint: "每行一个目录；开头的 ~ 会展开为用户主目录",
        type: .listText,
    ),
]

let defaultModelField = FieldSpec(
    "default", label: "默认模型",
    hint: "留空 = 第一个 provider 的默认模型",
    ph: "provider/model",
)

let settingsTabs: [TabSpec] = [
    TabSpec(id: "providers", label: "模型", icon: "square.stack.3d.up"),
    TabSpec(id: "limits", label: "限额与保护", icon: "shield", sections: [
        ("运行预算", [
            FieldSpec(
                "limits.max_input_tokens", label: "兜底上下文窗口",
                hint: "模型未声明 context_window 时使用",
                ph: "200000", type: .number,
            ),
            FieldSpec("limits.max_output_tokens", label: "最大输出 Token 数", ph: "16384", type: .number),
            FieldSpec(
                "limits.max_cost_usd", label: "费用上限（USD）",
                hint: "单会话累计预估费用；0 = 不限（需要费用速率追踪）",
                ph: "5.0", type: .number,
            ),
            FieldSpec(
                "limits.max_tokens", label: "总 Token 预算",
                hint: "单会话累计 token；0 = 不限",
                ph: "0", type: .number,
            ),
            FieldSpec("limits.max_tool_output_bytes", label: "工具输出保留字节数", ph: "49152", type: .number),
            FieldSpec("limits.max_artifact_bytes", label: "产物最大字节数", ph: "104857600", type: .number),
        ]),
        ("上下文压缩", [
            FieldSpec("context.utilization", label: "窗口利用率", ph: "0.95", type: .number),
            FieldSpec("context.compact_trigger_ratio", label: "压缩触发比例", ph: "0.80", type: .number),
            FieldSpec(
                "context.compact_target_ratio", label: "压缩目标比例",
                hint: "必须低于触发比例",
                ph: "0.50", type: .number,
            ),
            FieldSpec(
                "context.notice_levels", label: "占用提醒档位",
                hint: "逗号分隔，递增且低于触发比例",
                ph: "0.60, 0.75", type: .floatList,
            ),
        ]),
        ("失控检测", [
            FieldSpec("runaway.max_repeated_calls", label: "最大重复调用次数", ph: "3", type: .number),
            FieldSpec("runaway.max_consecutive_failures", label: "最大连续失败次数", ph: "5", type: .number),
            FieldSpec(
                "runaway.stall_warn_turns", label: "停滞提醒轮数",
                hint: "0 = 关闭",
                ph: "10", type: .number,
            ),
            FieldSpec(
                "runaway.stall_timeout", label: "停滞看门狗",
                hint: "Go duration 语法；0 = 关闭",
                ph: "15m",
            ),
        ]),
    ]),
    TabSpec(id: "permission", label: "权限与审批", icon: "lock", sections: [
        ("审批基线", [
            FieldSpec(
                "approval.mode", label: "审批模式",
                hint: "没有规则或记忆匹配时的决策策略",
                type: .select,
                options: [
                    ("", "默认（on-request）"),
                    ("on-request", "on-request · 沙盒/工作区内自由执行"),
                    ("danger-only", "danger-only · 仅危险操作弹窗"),
                    ("never", "never · 无人值守"),
                ],
                optionHints: [
                    "": "on-request（默认）：沙盒内的命令和工作区内的读写自动允许；沙盒提权、工作区外写入、外部网络访问以及危险信号会弹窗确认",
                    "on-request": "沙盒内的命令和工作区内的读写自动允许；沙盒提权、工作区外写入、外部网络访问以及危险信号会弹窗确认",
                    "danger-only": "仅明确危险的操作弹窗：危险站点拒绝列表、危险模式（curl|sh、凭证/启动文件写入等）、破坏性或有共享状态后果的操作（删除关键目标、git push 等）；开发命令、普通站点/API 以及沙盒提权自动允许",
                    "never": "无人值守：沙盒内允许；提权、工作区外写入、破坏性/共享状态操作直接拒绝——永远不会阻塞等待审批",
                ],
            ),
        ]),
        ("规则分层", [
            FieldSpec("rules.enabled", label: "启用规则", type: .tristate),
            FieldSpec("rules.builtin", label: "内置只读命令", type: .tristate),
            FieldSpec("rules.project", label: "项目规则", type: .tristate),
            FieldSpec(
                "rules.project_allow", label: "允许项目层的 allow 规则",
                hint: "不受信任的仓库只能收紧，不能放宽",
                type: .tristate,
                def: "关",
            ),
            FieldSpec(
                "rules.persist_remembered", label: "持久化「始终允许」",
                hint: "写入用户级规则文件，供后续会话继承",
                type: .tristate,
            ),
        ]),
    ]),
    TabSpec(id: "agent", label: "智能体", icon: "brain", sections: [
        ("系统提示词", [
            FieldSpec("prompt.extra", label: "附加指令", hint: "追加到内置系统提示词的末尾", type: .textarea),
            FieldSpec("prompt.disable_builtin", label: "禁用内置提示词", type: .bool),
            FieldSpec("prompt.managed.name", label: "托管提示词名称", hint: "Langfuse 托管的提示词（需要开启追踪）"),
            FieldSpec("prompt.managed.label", label: "托管提示词标签", ph: "production"),
        ]),
        ("子智能体", [
            FieldSpec("subagent.enabled", label: "启用子智能体", type: .tristate),
            FieldSpec("subagent.model", label: "固定模型", hint: "留空 = 跟随当前轮次的模型", ph: "provider/model"),
            FieldSpec(
                "subagent.max_tokens", label: "Token 上限",
                hint: "0 = 继承运行预算",
                ph: "0", type: .number,
            ),
            FieldSpec("subagent.max_output_tokens", label: "最大输出 Token 数", ph: "8192", type: .number),
        ]),
        ("长期记忆", [
            FieldSpec("memory.enabled", label: "启用记忆", type: .tristate),
            FieldSpec(
                "memory.extract_model", label: "提取模型",
                hint: "建议使用便宜快速的模型；留空 = 跟随默认模型",
                ph: "provider/model",
            ),
            FieldSpec("memory.consolidation_model", label: "整合模型", ph: "provider/model"),
            FieldSpec("memory.max_jobs_per_run", label: "每次运行最大任务数", ph: "8", type: .number),
            FieldSpec(
                "memory.run_interval", label: "流水线间隔",
                hint: "0 = 仅在启动时运行一次",
                ph: "30m",
            ),
            FieldSpec("memory.min_session_idle", label: "会话空闲阈值", ph: "1h"),
            FieldSpec("memory.max_session_age", label: "会话最大保留时长", ph: "720h"),
        ]),
        ("会话归档", [
            FieldSpec(
                "sessions.auto_archive_after", label: "自动归档",
                hint: "空闲超过该时长的会话会自动归档（只读，可随时取消归档）；留空或 0 = 关闭",
                ph: "如 720h",
            ),
            FieldSpec(
                "sessions.gc_archived_after", label: "归档保留时长",
                hint: "归档超过该时长的会话将被永久删除（包括事件、检查点和文件变更历史）；留空或 0 = 永久保留",
                ph: "如 720h",
            ),
        ]),
        ("文生图", [
            FieldSpec(
                "image.enabled", label: "启用文生图",
                hint: "默认：provider 和模型都设置时启用",
                type: .tristate,
                def: "自动",
            ),
            FieldSpec("image.provider", label: "凭证 Provider", hint: "复用其 base_url/api_key（必须是 openai 类型的 provider）"),
            FieldSpec("image.model", label: "图像模型"),
            FieldSpec("image.size", label: "默认尺寸", ph: "如 1024x1024"),
            FieldSpec(
                "image.quality", label: "默认质量", type: .select,
                options: [("", "自动"), ("low", "low"), ("medium", "medium"), ("high", "high")],
            ),
        ]),
    ]),
    TabSpec(id: "skills", label: "技能", icon: "puzzlepiece"),
    TabSpec(id: "mcp", label: "MCP", icon: "cable.connector"),
    TabSpec(id: "kb", label: "知识库", icon: "cylinder", sections: [
        ("连接", [
            FieldSpec(
                "knowledge_base.enabled", label: "启用知识库",
                hint: "自动 = 设置 base_url 后启用；修改需要重启生效",
                type: .tristate,
                def: "自动",
            ),
            FieldSpec(
                "knowledge_base.base_url", label: "服务地址",
                hint: "minisearch v2 REST 地址",
                ph: "http://127.0.0.1:8200",
                required: true,
            ),
            FieldSpec(
                "knowledge_base.api_key", label: "API 密钥",
                hint: "minisearch bearer token（msk_…）；--auth=off 时留空",
                type: .password,
                revealRef: SecretRef(kind: "knowledge_base"),
            ),
            FieldSpec(
                "knowledge_base.timeout_ms", label: "请求超时（毫秒）",
                hint: "范围 1000–60000",
                ph: "10000", type: .number,
            ),
        ]),
        ("检索", [
            FieldSpec(
                "knowledge_base.default_top_k", label: "默认 Top K",
                hint: "范围 1–20",
                ph: "5", type: .number,
            ),
            FieldSpec("knowledge_base.default_collection", label: "默认集合", ph: "留空 = 第一个集合"),
            FieldSpec(
                "knowledge_base.collections", label: "集合",
                hint: "至少一个；描述会写入工具 schema，帮助模型按主题路由",
                ph: "名称: 描述（每行一条）",
                type: .pairList,
                rows: 4,
                required: true,
            ),
        ]),
    ]),
    TabSpec(id: "system", label: "系统", icon: "gear", sections: [
        ("开发工具链", [
            FieldSpec(
                "tools.path_extra", label: "额外 PATH 目录",
                hint: "每行一个绝对路径（支持 ~/ 前缀）；优先于所有内置候选目录；保存时热生效",
                ph: "~/corp/bin",
                type: .listText,
                rows: 3,
            ),
        ]),
        ("Langfuse 追踪", [
            FieldSpec("tracing.host", label: "服务地址", ph: "https://langfuse.internal"),
            FieldSpec(
                "tracing.public_key", label: "Public Key", type: .password,
                revealRef: SecretRef(kind: "tracing", field: "public_key"),
            ),
            FieldSpec("tracing.public_key_env", label: "Public Key 环境变量"),
            FieldSpec(
                "tracing.secret_key", label: "Secret Key", type: .password,
                revealRef: SecretRef(kind: "tracing", field: "secret_key"),
            ),
            FieldSpec("tracing.secret_key_env", label: "Secret Key 环境变量"),
            FieldSpec("tracing.environment", label: "环境标签", ph: "dev"),
            FieldSpec("tracing.include_content", label: "发送对话内容", type: .tristate),
            FieldSpec("tracing.user", label: "归属用户", hint: "留空 = 依次尝试 git user.email、$USER"),
            FieldSpec("tracing.cost_input_usd_per_mtok", label: "输入费率（USD/Mtok）", ph: "0", type: .number),
            FieldSpec("tracing.cost_output_usd_per_mtok", label: "输出费率（USD/Mtok）", ph: "0", type: .number),
        ]),
        ("局域网分享", [
            FieldSpec(
                "share.enabled", label: "启用局域网分享",
                hint: "保存后立即生效（热更新）；监听器只暴露只读分享页，不暴露管理 API",
                type: .tristate,
                def: "关",
            ),
            FieldSpec(
                "share.listen", label: "监听地址",
                hint: "固定端口可让分享链接跨重启存活；0.0.0.0 = 所有网卡，也可绑定指定网卡 IP",
                ph: "0.0.0.0:7681",
            ),
        ]),
        ("日志", [
            FieldSpec("logging.max_file_mb", label: "单个日志文件上限（MiB）", ph: "2048", type: .number),
            FieldSpec("logging.max_total_mb", label: "日志总大小上限（MiB）", ph: "10240", type: .number),
        ]),
        ("浏览器", [
            FieldSpec(
                "browser.enabled", label: "启用浏览器工具",
                hint: "默认启用；关闭后不注册浏览器工具",
                type: .tristate,
                def: "开",
            ),
            FieldSpec(
                "browser.chrome_path", label: "Chrome 路径",
                hint: "Chrome/Chromium 二进制路径；留空 = 自动检测常见系统位置",
                ph: "留空 = 自动检测",
            ),
            FieldSpec(
                "browser.cdp_url", label: "CDP 远程地址",
                hint: "远程 Chrome DevTools Protocol 地址；连接外部 Chrome 而不是启动本地实例（可绕过反爬检测）",
                ph: "ws://127.0.0.1:9222",
            ),
            FieldSpec(
                "browser.idle_ttl", label: "空闲 TTL",
                hint: "空闲超过该时长的浏览器实例会自动关闭（Go duration 语法）",
                ph: "5m",
            ),
            FieldSpec(
                "browser.nav_timeout_ms", label: "导航超时（毫秒）",
                hint: "页面导航超时，范围 5000–120000",
                ph: "30000", type: .number,
            ),
            FieldSpec(
                "browser.screenshot_quality", label: "截图质量",
                hint: "JPEG 质量，范围 10–100",
                ph: "80", type: .number,
            ),
            FieldSpec("browser.viewport_width", label: "视口宽度", ph: "1280", type: .number),
            FieldSpec("browser.viewport_height", label: "视口高度", ph: "720", type: .number),
        ]),
        ("终端 UI（TUI）", [
            FieldSpec(
                "ui.icons", label: "图标集", type: .select,
                options: [("", "默认（nerd）"), ("nerd", "nerd (Nerd Font)"), ("plain", "plain（纯文本）")],
            ),
            FieldSpec("ui.alt_screen", label: "使用备用屏幕", hint: "退出时恢复滚动缓冲区", type: .bool),
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

/// Returns a field-specific error before lossy collection can omit invalid input.
/// Empty values still mean "use the default".
func invalidInput(_ spec: FieldSpec, _ state: ControlState) -> String? {
    let text = state.textValue.trimmingCharacters(in: .whitespacesAndNewlines)
    switch spec.type {
    case .number:
        let numberText = state.textValue.trimmingCharacters(in: .whitespacesAndNewlines)
        if !numberText.isEmpty, Int64(numberText) == nil, !(Double(numberText)?.isFinite ?? false) {
            return "请输入有效数字"
        }
    case .kvText:
        for (index, line) in state.textValue.components(separatedBy: .newlines).enumerated() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty else { continue }
            guard let equal = trimmed.firstIndex(of: "="),
                  !trimmed[..<equal].trimmingCharacters(in: .whitespaces).isEmpty
            else {
                return "第 \(index + 1) 行应为 KEY=VALUE"
            }
        }
    case .pairList:
        for (index, line) in state.textValue.components(separatedBy: .newlines).enumerated() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty else { continue }
            let name = trimmed.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)[0]
                .trimmingCharacters(in: .whitespaces)
            if name.isEmpty {
                return "第 \(index + 1) 行缺少名称"
            }
        }
    case .tristate:
        if !text.isEmpty, text != "true", text != "false" {
            return "请选择自动、开启或关闭"
        }
    case .floatList:
        if !text.isEmpty {
            for part in text.split(separator: ",", omittingEmptySubsequences: false) {
                let items = part.split(whereSeparator: \.isWhitespace)
                if items.isEmpty || items.contains(where: { !(Double($0)?.isFinite ?? false) }) {
                    return "请输入以逗号或空格分隔的有效数字"
                }
            }
        }
    default:
        break
    }
    return nil
}

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
        let trimmed = state.textValue.trimmingCharacters(in: .whitespacesAndNewlines)
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
        let value = state.textValue.trimmingCharacters(in: .whitespacesAndNewlines)
        if value == "true" || value == "false" {
            setPath(&obj, spec.key, .bool(value == "true"))
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
        for line in state.textValue.components(separatedBy: .newlines) {
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
