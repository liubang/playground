# Loom 配置文件设计（多 Provider / 多模型）

| 项目 | 内容 |
|------|------|
| 状态 | v3.3（P1 已实现；v3.2 新增 `LOOM_CONFIG` 定位变量、headless 复用 Bootstrap；v3.3 删除废弃 env warn 机制与全部迁移叙述注释——开发期无历史用户） |
| 日期 | 2026-07-26 |
| 关联文档 | `DESIGN.md`（§4 总体架构）、`TUI_DESIGN.md`（Slash Command）、`SERVE_DESIGN.md`（§2.2 G8 装配逻辑两份） |
| 目标读者 | loom 运行时与前端贡献者 |

---

## 1. 背景与目标

loom 目前的全部配置都来自环境变量：模型接入只有一组 `LOOM_MODEL` / `LOOM_BASE_URL` / `LOOM_API_KEY` / `LOOM_WIRE_API`，即**单 provider、单模型**；其余行为开关（limits、prompt、skills、rules、tracing、UI 偏好）也均为 env。这带来三个问题：

1. **无法多模型切换**：日常需要在不同网关/模型间切换（如 deepseek 做日常、gpt 做难题），env 方式必须改环境重启；
2. **配置不可沉淀**：一组相关的 env（如某个 provider 的 base_url + key + 模型列表）无法作为一个整体被命名、版本化、分享；
3. **env 面持续膨胀**：已有 30+ 个 `LOOM_*` 变量，缺乏结构和校验，拼写错误静默无效。

本设计引入 **YAML 配置文件作为唯一配置来源**，目标：

1. **多 provider、多模型**：配置文件中定义多个 provider，每个 provider 下挂多个可选模型；`/model` 在运行时切换（复用已落地的 `Controller.SetModel` 通道）；
2. **配置统一收口**：现有通过 env 传递的配置项（除系统级标准变量外）全部迁入配置文件；**`LOOM_*` 配置 env 及其消费代码整体删除**，没有兼容层、没有覆盖层，代码只有一条配置路径；
3. **分层与信任边界**：用户层（`~/.loom/`）可定义 provider 与密钥引用；项目层（`<workspace>/.loom/`）**只能选择与覆盖参数，禁止定义新 provider 与密钥**，防止恶意仓库投毒 `base_url` 窃取对话与密钥（P2）；
4. **无历史包袱**：项目尚无外部用户，breaking change 一次性做净，优于长期背兼容层。

### 1.1 与非目标

**非目标（本期不做）**：

- 配置热加载（启动时加载一次；改动需重启，见 §12 开放问题）；
- 非 OpenAI-compatible 协议（Anthropic 原生等）。schema 以 `providers[].type` 留门，首版只有 `type: openai`；
- `api_key_cmd`（执行外部命令取密钥，如 `gopass`/`vault`）。密钥引用首版仅支持环境变量（见 §5）；
- 模型能力标记（reasoning / supports_tools / vision）。agent loop 当前假设模型均支持 tool call，接弱模型时再补；
- CLI flag（`--model` / `--provider`），后续按需再加；
- env 兼容层/覆盖层（**明确删除而非废弃**：不读、不映射、不兜底）；
- 项目层覆盖（P2 阶段，见 §11）。

---

## 2. 现状评估

### 2.1 env 配置面全清单

| 类别 | 变量 | 消费位置 | 迁移目标 |
|------|------|----------|-----------|
| 模型接入 | `LOOM_MODEL` / `LOOM_BASE_URL` / `LOOM_API_KEY` / `LOOM_WIRE_API` | `bootstrap.go` / `main.go` | 迁移：由 providers 取代 |
| 模型窗口 | `LOOM_CONTEXT_WINDOW` | `main.go` | 迁移：下沉为模型级元数据 |
| 运行预算 | `LOOM_MAX_TURNS` / `LOOM_MAX_TOOL_CALLS` / `LOOM_MAX_INPUT_TOKENS` / `LOOM_MAX_OUTPUT_TOKENS` / `LOOM_MAX_COST_USD` / `LOOM_MAX_WALL_TIME` / `LOOM_MAX_TOOL_OUTPUT_BYTES` / `LOOM_MAX_ARTIFACT_BYTES` / `LOOM_MAX_REPEATED_ACTIONS` | `domain/limits.go` `LimitsFromEnv` | 迁移：`limits.*` |
| 系统提示 | `LOOM_SYSTEM_PROMPT_EXTRA` / `LOOM_DISABLE_SYSTEM_PROMPT` | `bootstrap.go` / `main.go` | 迁移：`prompt.*` |
| Skills | `LOOM_SKILLS` / `LOOM_SKILLS_EXTRA_ROOTS` | `app/skills.go` | 迁移：`skills.*` |
| 权限规则 | `LOOM_RULES` / `LOOM_BUILTIN_RULES` / `LOOM_PROJECT_RULES` / `LOOM_PROJECT_RULES_ALLOW` / `LOOM_RULES_PERSIST` | `permission/policy.go` / `app/controller.go` | 迁移：`rules.*` |
| 追踪 | `LOOM_LANGFUSE_HOST`（fallback `LANGFUSE_HOST` / `LANGFUSE_BASE_URL`）/ `LOOM_LANGFUSE_PUBLIC_KEY` / `LOOM_LANGFUSE_SECRET_KEY` / `LOOM_LANGFUSE_ENVIRONMENT` / `LOOM_TRACE_CONTENT` / `LOOM_TRACE_USER` / `LOOM_COST_INPUT_USD_PER_MTOK` / `LOOM_COST_OUTPUT_USD_PER_MTOK` | `trace/config.go` | 迁移：`tracing.*` |
| 托管提示词 | `LOOM_PROMPT_NAME` / `LOOM_PROMPT_LABEL` | `bootstrap.go` | 迁移：`prompt.managed.*` |
| 存储 | `LOOM_SESSION_DB` | `main.go` | 迁移：`storage.session_db` |
| TUI | `LOOM_ICONS` / `LOOM_ALT_SCREEN` | `main.go` | 迁移：`ui.*` |
| 发布标记 | `LOOM_VERSION` | `main.go` / `process/types.go` / `trace` | 不迁移：非用户配置；自我戳版机制改为装配层显式传入（§8） |
| 配置定位 | `LOOM_CONFIG`（v3.2 新增） | `main.go` | 不迁移：配置文件**路径**定位器（类比 `KUBECONFIG`），非配置本身；默认 `~/.loom/config.yaml` |
| 系统标准 | `XDG_STATE_HOME` / `NO_COLOR` / `TERM` / `SHELL` / `USER` / `HOME` | 多处 | 不迁移：社区/OS 标准，保持 env |
| 归因注入 | `LOOM_SESSION_ID` / `LOOM_AGENT_NAME` / `LOOM_AGENT_VERSION`（由 loom 注入 spawned 命令） | `process/` | 不迁移：运行时输出，非输入 |

### 2.2 已有地基

| 能力 | 位置 | 说明 |
|------|------|------|
| Provider 抽象 | `domain.Model`（`internal/domain/interfaces.go`） | `Stream(ctx, ModelRequest)`，**模型名按请求携带**，provider 实例与模型名天然解耦——多模型不需要多实例，多 provider 也只是多实例化 |
| Provider 实现 | `internal/model/openai`、`internal/model/anthropic` | `openai.New(Config{BaseURL, APIKey, WireAPI, MaxRetries})`；OpenAI-compatible 网关（deepseek/豆包/vLLM 等）均可复用；`anthropic.New(Config{BaseURL, APIKey, Version, MaxRetries})` 覆盖 Claude Messages API |
| 运行时切换通道 | `app.Controller.SetModel` / TUI `/model` | 已落地：controller 经 `cmdCh` 串行化换模型名，下一 turn 生效；本设计把它升级为 provider-aware |
| YAML 依赖 | `gopkg.in/yaml.v3` | 已在 `MODULE.bazel` 与 `internal/skill/parse.go` 使用，**零新增依赖** |
| 用户数据目录约定 | `~/.loom/`（config.yaml、sessions/、memories/、rules/、skills/、logs/） | `permission.RulesDirUser()`、`preparePrivateDataDirectory`（0700 私有目录）直接复用 |
| 项目层信任先例 | `permission/policy.go` | 项目层 rules 默认不收 allow 规则，需 `LOOM_PROJECT_RULES_ALLOW=1` 显式开启——配置文件的信任边界沿用同一哲学 |
| 密钥不入库先例 | `run_cmd` 沙箱 env allowlist | spawned 命令只放行白名单 env，配置文件的密钥引用解析发生在进程内，不扩散 |

### 2.3 差距清单

| # | 差距 | 根因 | 后果（若不改） |
|---|------|------|----------------|
| G1 | 无配置文件加载层 | 配置散落各消费点直接读 env | env 面继续膨胀，无校验、无结构 |
| G2 | 单 provider 装配 | `Bootstrap.Model`/`ModelName` 单值；`openai.New` 启动时调一次 | 无法多 provider 切换 |
| G3 | 模型元数据全局唯一 | `LOOM_CONTEXT_WINDOW` 单值，进 `agent.Loop.ContextWindow` 与状态栏 | 切模型后 ctx 分母/compaction 阈值错误 |
| G4 | `/model` 只知模型名 | `Controller.SetModel(name)` 只换字符串 | 跨 provider 同名模型无法区分；换 provider 需重启 |
| G5 | 密钥只认 env | `LOOM_API_KEY` 直读 | 多 provider 需要 N 个 key 时命名冲突、无法归组 |
| G6 | 装配逻辑两份 | `runChat` 走 `Bootstrap`，`runAgent` 手工装配（`main.go`） | 配置解析若再各写一份，腐化加剧（与 SERVE_DESIGN.md G8 同源） |

---

## 3. 总体设计

### 3.1 配置来源：一份文件，无覆盖链

```text
项目层 <workspace>/.loom/config.yaml（P2；信任边界 §3.3）
   └─> 用户层 ~/.loom/config.yaml
        └─> 内置默认值（仅行为开关有默认；providers 无默认，为必填节）
```

**没有 env 层**。所有 `LOOM_*` 配置变量及其消费代码（`os.Getenv` 读取点、`LimitsFromEnv`、`trace.ConfigFromEnv` 等）整体删除；每个配置项只有一个读取路径：`config.Load` → `ResolvedConfig` → 注入消费方。

**无配置文件时 fail fast**：报错信息直接内嵌最小可用示例供拷贝（见 §9），不猜测、不静默默认。密钥的 `api_key_env` 引用不是配置层，是运行时密钥解析（见 §5），不受此约束。

### 3.2 加载时机与解析管线

```text
┌────────────────────────────────────────────────────────────────────┐
│ cmd/loom/main.go（chat / run / resume 共用）                        │
│                                                                    │
│  config.Load(userPath, projectPath?)                               │
│    ├─ 读取 + yaml 解析 + 结构校验（§7）                              │
│    ├─ 项目层信任边界过滤（§3.3，P2）                                 │
│    └─ 密钥引用解析（api_key_env → 值，§5）                           │
│         ↓                                                          │
│  config.ResolvedConfig（扁平、已校验、可直接消费）                    │
│    ├─ Providers []ResolvedProvider（含预建 domain.Model 实例所需一切）│
│    ├─ Default   ProviderModelRef                                   │
│    ├─ Limits / Prompt / Skills / Rules / Tracing / UI / Storage    │
│         ↓                                                          │
│  app.NewBootstrap(ctx, resolved) ← 直接消费 ResolvedConfig          │
└────────────────────────────────────────────────────────────────────┘
```

新增 `internal/config` 包承载全部解析逻辑，`runChat`/`runAgent` 共用（顺带收敛 G6，为 SERVE_DESIGN.md 的 server 模式铺路）。解析失败**快速报错退出**（fail fast），绝不静默回退——配置错误静默无效正是 env 模式的最大痛点。

### 3.3 项目层信任边界（P2）

恶意仓库可以提交 `.loom/config.yaml`。若项目层能定义 provider，`base_url` 可指向攻击者端点，对话内容与密钥随请求外泄。因此：

| 字段类别 | 用户层 | 项目层 |
|----------|--------|--------|
| `default`（选择已定义的 provider/model） | 允许 | 允许（必须解析到用户层已定义的引用，否则报错） |
| 非敏感覆盖：`limits.*` / `prompt.extra` / `ui.*` | 允许 | 允许 |
| `providers[]`（含 `base_url` / `wire_api` / 任何密钥字段） | 允许 | 禁止，出现即报错 |
| `tracing.*`（含 host 与密钥） | 允许 | 禁止，出现即报错（追踪数据外泄面等同 base_url） |
| `storage.session_db` | 允许 | 禁止，出现即报错（路径劫持可污染会话存储） |

边界由 `config.Load` 在合并前强制执行，规则与字段白名单集中在一张表内，新增字段时强制分类。

---

## 4. 配置 Schema

### 4.1 完整示例（用户层 `~/.loom/config.yaml`）

```yaml
# 默认选择："provider/model" 形式；裸模型名要求全局唯一
default: deepseek/deepseek-chat

providers:
  - name: deepseek
    type: openai                            # 首版仅支持 openai（OpenAI-compatible）
    base_url: https://api.deepseek.com/v1
    api_key: sk-xxxxxxxx                    # 明文密钥；也可用 api_key_env 引用环境变量，见 §5
    wire_api: chat                          # provider 级默认：chat | responses
    max_retries: 2                          # 可选，默认 2
    default_model: deepseek-chat            # 可选；缺省取 models[0]
    models:
      - name: deepseek-chat
        context_window: 65536
        max_output_tokens: 8192             # 可选
      - name: deepseek-reasoner
        context_window: 65536
        wire_api: responses                 # 模型级覆盖 provider 默认

  - name: openai
    type: openai
    base_url: https://api.openai.com/v1
    api_key_env: OPENAI_API_KEY
    models:
      - name: gpt-5
        context_window: 400000

limits:                                     # 对应 domain.Limits，键名 snake_case
  max_turns: 50
  max_tool_calls: 100
  max_input_tokens: 200000
  max_output_tokens: 8192
  max_cost_usd: 5.0
  max_wall_time: 30m                        # Go duration 字符串
  max_tool_output_bytes: 65536
  max_artifact_bytes: 10485760
  max_repeated_actions: 3

prompt:
  extra: |                                  # LOOM_SYSTEM_PROMPT_EXTRA
    Always reply in Chinese.
  disable_builtin: false                    # LOOM_DISABLE_SYSTEM_PROMPT
  managed:                                  # Langfuse 托管提示词
    name: loom-system                       # LOOM_PROMPT_NAME
    label: production                       # LOOM_PROMPT_LABEL

skills:
  enabled: true                             # LOOM_SKILLS=0 ↔ false
  extra_roots: [/Users/me/skills]           # LOOM_SKILLS_EXTRA_ROOTS（列表，不再 ':' 分隔）

rules:                                      # 各开关 ↔ LOOM_RULES*；true=加载/允许
  enabled: true                             # LOOM_RULES=0 ↔ false
  builtin: true                             # LOOM_BUILTIN_RULES=0 ↔ false
  project: true                             # LOOM_PROJECT_RULES=0 ↔ false
  project_allow: false                      # LOOM_PROJECT_RULES_ALLOW=1 ↔ true
  persist_remembered: true                  # LOOM_RULES_PERSIST=0 ↔ false

tracing:                                    # Langfuse；密钥字段规则同 §5
  host: https://langfuse.internal
  public_key: pk-lf-xxxxxxxx
  secret_key: sk-lf-xxxxxxxx
  environment: dev
  include_content: true                     # LOOM_TRACE_CONTENT=0 ↔ false
  user: liubang                             # LOOM_TRACE_USER
  cost_input_usd_per_mtok: 0.15
  cost_output_usd_per_mtok: 0.60

storage:
  session_db: /Users/me/.loom/sessions/sessions.db   # 空则默认 ~/.loom/sessions/sessions.db

ui:
  icons: nerd                               # LOOM_ICONS：nerd | plain
  alt_screen: false                         # LOOM_ALT_SCREEN=1 ↔ true

subagent:                                   # delegate_task 子 Agent（docs/SUBAGENT_DESIGN.md §7）
  enabled: true                             # false = 不注册 delegate_task
  max_tokens: 0                             # 子 run 累计 token 上限；0 = 继承 limits.max_tokens
  max_output_tokens: 8192                   # 子 Agent 单次响应输出上限；0 = 继承 limits.max_output_tokens
  model: ""                                 # 可选 "provider/model"；空 = 跟随当前 turn 模型
```

### 4.2 字段规则

- **unknown 字段即错误**：yaml 解码开启 `KnownFields(true)`，拼写错误 fail fast；
- **除 `providers` 外所有字段可选**（回退内置默认）；空文件按无文件处理：fail fast 并内嵌示例（§9）；
- **命名唯一性**：`providers[].name` 全局唯一；同一 provider 内 `models[].name` 唯一；跨 provider 允许同名模型（靠 `provider/model` 消歧）；
- **`default` 解析**（按序）：① 含 `/` → `provider/model` 精确匹配；② 裸名 → 先按 provider 名匹配（取该 provider 的 `default_model`），再按模型名全局唯一匹配；③ 任一步歧义或不命中即报错并列出候选；
- **`default` 缺省**：取 `providers[0]` 的 `default_model`（其再缺省取 `models[0]`）——单 provider 场景可以不写 `default`；
- **`base_url` 必填**：`openai.New` 对空 `BaseURL` 静默回退官方端点（`provider.go` `defaultBaseURL`）——deepseek 配置漏写会把密钥与对话发往 `api.openai.com`。显式必填消除这整类静默错配；
- **`storage.session_db` 缺省**：默认 `~/.loom/sessions/sessions.db`（`main.go` `defaultLoomHome`）；旧版本平台状态目录布局已废弃，不再推导；
- **duration / 数字**：duration 用 Go 字符串（`30m`、`1h`）；token/字节数用整数（不支持 `10k` 之类后缀，保持简单）。

---

## 5. 密钥管理

密钥直接放配置文件是**正式支持**的方式（loom 是单机个人工具，配置文件本身就是私有数据，与 `~/.loom/sessions/sessions.db` 同目录取决于同一文件系统权限）：

| 方式 | 语法 | 说明 |
|------|------|------|
| 明文 | `api_key: sk-...` | 直接书写，最简单 |
| 环境变量引用 | `api_key_env: DEEPSEEK_API_KEY` | 配置里只存变量名，加载时 `os.LookupEnv` 解析；适合 CI、或配置文件需要分享/入仓的场景。变量未设置即报错（指名道姓） |

- 同一字段两种形式互斥，同时出现即错误；
- 适用字段：`providers[].api_key*`、`tracing.public_key*`、`tracing.secret_key*`（即 `*_key` / `*_key_env` 成对）；
- 含明文密钥且文件权限非 `0600` 时打一次 warn 日志提醒（**不阻断**）；
- `api_key_env` 是运行时密钥解析，不是 env 配置面，不受“单一配置来源”约束；
- 密钥解析只发生在 `config.Load` 内部，`ResolvedConfig` 持有解析后的值，不回流到任何日志/事件/trace payload；spawned 命令的 env allowlist 不因此扩大，密钥不进子进程；
- `api_key_cmd`（外部命令取密钥）留作后续扩展，schema 不预留字段，避免半成品承诺。

---

## 6. 运行时模型解析与切换

### 6.1 Provider 注册表

```go
// internal/app/provider.go（新增）

// Provider 是一个已装配的模型提供方：一个 domain.Model 实例加其可选模型表。
type Provider struct {
    Name    string
    Model   domain.Model // 启动时预建；构建很轻（HTTP client 配置），全部预建切换零成本
    Models  []ModelMeta
    Default string       // provider 级默认模型名
}

// ModelMeta 是模型级元数据，随切换进入 Loop 与状态栏。
type ModelMeta struct {
    Name            string
    ContextWindow   int64          // 0 = 回退 Limits.MaxInputTokens（现有语义）
    MaxOutputTokens int64          // 请求参数（模型能力上限）；0 = 不下发
    WireAPI         openai.WireAPI // "" = 继承 provider
}

// ProviderModelRef 标识一次确定的选择。
type ProviderModelRef struct{ Provider, Model string }
```

两个 `max_output_tokens` 不冲突：`limits.max_output_tokens` 是 agent 运行预算（护栏，超额 abort turn），`models[].max_output_tokens` 是发给模型的请求参数（能力上限）；构造请求时取两者较小值，为 0 的项不参与。

`Bootstrap` 从 `Model domain.Model` + `ModelName string` 升级为：

```go
Providers map[string]*Provider  // 键为 provider 名
Current  ProviderModelRef       // 启动默认，即 config.default 解析结果
```

### 6.2 Controller 升级

已落地的 `Controller.SetModel(ctx, name)` 升级为引用解析：

- `SetModel(ctx, ref string)`：`ref` 为 `provider/model` 精确匹配，或裸模型名全局唯一匹配（歧义返回候选列表的错误）；在 controller 的 `cmdCh` 上串行化，**进行中的 turn 不受影响，下一 turn 生效**（沿用现有语义）。切换成功返回新模型的 `ModelMeta`：TUI 的 `modelChangedMsg` 透传它，状态栏 ctx 分母与下一次 `Loop.ContextWindow` 即时更新，不必等 snapshot；`Snapshot.ContextWindow` 作为会话切换/事件流重连时的兜底；
- controller 内部状态从单个 `modelName string` 扩为 `current ProviderModelRef`；
- `Snapshot` 增加 `ProviderName string` 与 `ContextWindow int64`；`SessionOpenedPayload.Model` 上报 `provider/model` 形式；
- `runTurn` 用当前选择的 provider 实例构建 `agent.Loop`，`Loop.ContextWindow` 取当前模型的元数据（G3）。

### 6.3 TUI `/model` 升级

| 输入 | 行为 |
|------|------|
| `/model` | 状态栏显示当前 `provider/model` |
| `/model <model>` | 全局唯一匹配则切换；歧义则错误提示并列出候选（`deepseek/deepseek-chat`、`other/deepseek-chat`） |
| `/model <provider>/<model>` | 精确切换 |
| `/model` + picker（P2） | 仿 `SessionPicker` 弹层：列出全部 `provider/model` + context window，↑↓ 选择，Enter 确认 |

状态栏标题从 `Loom · <model>` 变为 `Loom · <provider>/<model>`。切换 ack 沿用 `modelChangedMsg` 通道。

---

## 7. 校验与错误处理

加载期（fail fast，退出码非零，错误信息指明文件、字段、原因）：

1. yaml 语法错误；unknown 字段；
2. provider/model 重名；`default` 无法解析或歧义；
3. `type` 非 `openai`；`wire_api` 非 `chat|responses`；
4. provider 的 `models` 为空；`default_model` 不在其 `models` 列表中；`base_url` 为空（必填，理由见 §4.2）；
5. `api_key` 与 `api_key_env` 同时出现；`api_key_env` 指向的环境变量未设置；
6. provider 装配失败（非法 `base_url` 等 `openai.New` 返回的错误）——与配置错误同级，fail fast，不允许“跳过坏 provider 继续跑”（部分可用状态会让 `default` 解析结果随失败集合变化，行为不可预期）；
7. 项目层出现禁止字段（§3.3）；
8. `limits`/`context_window` 等为负；duration 无法解析。

运行期：切换 `/model` 引用不存在或歧义 → 状态栏错误提示，草稿保留（沿用现有交互）。

---

## 8. 对现有代码的改动面

| 位置 | 改动 |
|------|------|
| `internal/config/`（**新增包**） | `load.go`（读取/合并/信任边界）、`schema.go`（yaml 结构 + `KnownFields` 校验）、`resolve.go`（密钥引用解析、`ResolvedConfig`）、`provider.go`（装配 `domain.Model` 实例） |
| `internal/app/bootstrap.go` | `NewBootstrap` 直接消费 `ResolvedConfig`；`Bootstrap` 持 `Providers`/`Current`；**删除** `DefaultBootstrapConfig` 及其 env 读取 |
| `internal/app/provider.go`（**新增**） | §6.1 类型 + 引用解析（`Resolve(ref) (ProviderModelRef, error)`） |
| `internal/app/controller.go` | `SetModel` 升级为引用解析；`Snapshot` 加 `ProviderName`/`ContextWindow`；`runTurn` 用当前选择；`LOOM_RULES_PERSIST` 读取点**删除**，改为 `Bootstrap` 字段注入 |
| `cmd/loom/main.go` | `runChat`/`runAgent` 统一走 `config.Load` → `ResolvedConfig`；**删除全部** `LOOM_MODEL`/`LOOM_BASE_URL`/`LOOM_API_KEY`/`LOOM_WIRE_API`/`LOOM_CONTEXT_WINDOW`/`LOOM_SYSTEM_PROMPT_EXTRA`/`LOOM_DISABLE_SYSTEM_PROMPT`/`LOOM_ICONS`/`LOOM_ALT_SCREEN`/`LOOM_SESSION_DB` 读取点；手工装配段收敛（G6） |
| `internal/domain/limits.go` | **删除** `LimitsFromEnv` 与 `Env*` 常量；`Limits` 结构保留，由 `config` 包构造 |
| `internal/trace/config.go` | **删除** `ConfigFromEnv` 与 `LANGFUSE_*` fallback 链；`Config` 由 `ResolvedConfig.Tracing` 构造；`defaultUserID`（git email → `$USER`）作为默认值推导保留 |
| `internal/permission/policy.go` | **删除** `LOOM_RULES`/`LOOM_BUILTIN_RULES`/`LOOM_PROJECT_RULES`/`LOOM_PROJECT_RULES_ALLOW` 读取点；`AttachRules` 改为接收 `rules.*` 解析后的 `LoadOptions` |
| `internal/app/skills.go` | `WireSkills` 的 `getenv` 参数改为解析后的 `skills.*` 结构（testability 注入口保留）；其 `contextWindow` 参数取启动默认模型的窗口——catalog 是启动时静态注入，切换模型不重建（可接受，catalog 规模远小于窗口） |
| `internal/ui`（`update.go`/`view.go`） | `/model` 引用语法 + 歧义提示；状态栏 `provider/model`；`LOOM_ICONS`/`LOOM_ALT_SCREEN` 改由 `ResolvedConfig.UI` 经 `InitOptions` 传入 |
| `internal/process/types.go`、`internal/trace` | `LOOM_VERSION` 改为装配层显式传入（版本戳是内部机制，非用户配置面）；loom 注入子进程的 `LOOM_SESSION_ID`/`LOOM_AGENT_*` 是**输出**，保留 |
| 保留不动 | 系统/社区标准：`XDG_STATE_HOME`/`NO_COLOR`/`TERM`/`SHELL`/`USER`/`HOME`；bazel 环境的 `BUILD_WORKSPACE_DIRECTORY` |
| `go.mod` / `MODULE.bazel` | 无新增依赖（`yaml.v3` 已有） |

---

## 9. 破坏性变更与迁移

这是一次 **breaking change**，不设兼容期：

- **`LOOM_*` 配置 env 全部失效**（§2.1 表中标记“迁移”的行），不读、不映射、不提示——开发期项目无历史用户，不留迁移辅助代码；
- **无配置文件时 fail fast**：错误信息内嵌最小可用示例，拷贝改密钥引用即可用：

  ```yaml
  default: deepseek/deepseek-chat
  providers:
    - name: deepseek
      type: openai
      base_url: https://api.deepseek.com/v1
      api_key: sk-xxxxxxxx          # 或 api_key_env: DEEPSEEK_API_KEY
      models:
        - name: deepseek-chat
          context_window: 65536
  ```

- **headless（`loom run`/`resume`）**：同一 `config.Load` 管线，CI 脚本需把 env 改写为配置文件（CI 里密钥可走 `api_key_env` 注入，无需落盘）；
- **辅助迁移**：`loom config init` 生成骨架（P2，见 §11）；首版靠 fail fast 的内嵌示例已足够。

---

## 10. 测试策略

| 层 | 内容 |
|----|------|
| `internal/config` 单测 | schema 校验全分支（§7 逐条）；密钥明文与引用两种形式及互斥（`t.Setenv`）；`0600` warn（`t.TempDir` + `chmod`）；项目层信任边界白名单 |
| `internal/app` | `Resolve` 精确/裸名/歧义/不存在；`SetModel` 跨 provider 后下一 turn 的 provider 实例与模型名正确（`fakes.FakeModel.Calls()` 已有断言模式）；Snapshot 新字段 |
| `internal/ui` | `/model` 无参/裸名/带 provider/歧义/失败草稿恢复（沿用刚落地的测试模式） |
| env 删除 | 原有经 `t.Setenv` 注入 `LOOM_*` 的测试全部改走 `ResolvedConfig` 构造；全量测试绿即证明删除干净 |

---

## 11. 分阶段实施

| 阶段 | 内容 | 验收 |
|------|------|------|
| **P1**（本期） | `internal/config` 包；用户层配置；provider registry；`SetModel` 引用解析；每模型 `context_window`；装配统一走 `ResolvedConfig`；`/model` 引用语法；**`LOOM_*` 配置 env 与消费代码整体删除**；无配置 fail fast（内嵌示例） | 全量测试在配置文件下绿；`grep -r "LOOM_MODEL\|LOOM_BASE_URL\|LOOM_API_KEY" --include='*.go'` 无配置读取点；配置文件端到端切换 provider |
| **P2** | 项目层配置（§3.3 信任边界）；`/model` picker；`loom config init` | 恶意项目层样本全部报错；picker UI 测试 |
| **P3**（待定） | `api_key_cmd`；配置热加载；模型能力标记；非 openai `type` | 按需求驱动 |

---

## 12. 开放问题

1. **项目层可否放宽 `limits`**？收紧无风险，放宽涉及额度。当前设计把 `limits.*` 整体放行给项目层（§3.3），是否应只允许收紧（逐项取 min/max）？倾向：P2 实现时加“只收紧”校验，schema 不变。
2. **热加载**：`SIGHUP` 或文件监听？切换 provider 是运行时操作，但 limits/rules 热改涉及在途 turn 的一致性。倾向：不做，重启进程成本足够低。
3. **同一 provider 多份 key**（如主备 key 轮换）：超纲，真实需求出现时以 `api_keys_env` 列表扩展，schema 暂不预留。
4. ~~废弃 env 的 warn 检测是否长期保留~~（已裁决：开发期无历史用户，检测机制与变量名单一并删除，不保留过渡代码）。
