# Loom：生产级 Coding Agent 设计

- 状态：Draft（架构评审修订版 1）
- 目标版本：1.0
- 语言：Go
- 形态：本地优先 CLI，核心可复用于 IDE 和服务端
- 构建：Go Modules + Bazel

## 1. 概述

Loom 是面向软件工程任务的通用 Coding Agent。它以大语言模型作为决策器，通过受控工具读取和修改代码、执行命令、验证结果，并在有限预算内迭代，直到完成任务、请求用户介入或安全终止。

Loom 的目标不是模型 API 的命令行包装器，而是生产级 Agent Harness：

- 模型无关的消息、流式事件和工具协议；
- 可恢复、可取消、受预算约束的 Agent 状态机；
- 安全、可审计、可扩展的 Tool Runtime；
- 面向大型仓库的上下文选择与压缩；
- 编辑、构建、测试和诊断组成的验证闭环；
- 会话持久化、检查点、可观测性和系统化评测；
- 通过 MCP、子进程和稳定 RPC 协议扩展工具与前端。

核心模式为：

> 单主 Agent + ReAct 工具循环 + 动态计划 + 确定性约束 + 按需子 Agent

主 Agent 持有目标和最终决策权；程序负责权限、安全、预算、执行和持久化。模型输出与外部内容一律视为不可信输入。

## 2. 目标、非目标与生产标准

### 2.1 目标

1. 能探索陌生仓库、定位问题、跨文件修改、运行构建和测试，并根据失败继续修复。
2. 所有副作用由策略控制；高风险操作需要审批；默认保护工作区外路径、秘密、用户改动和 Git 历史。
3. 支持取消、超时、重试、预算、会话恢复、上下文压缩和崩溃恢复。
4. 核心不绑定模型厂商和终端 UI，支持多个 Provider，并可供 IDE 调用。
5. 模型调用、工具调用、审批、文件变化、费用和验证结论可审计。
6. 支持内置工具、MCP 和受控子进程扩展，后续增加 LSP 和子 Agent。
7. 具备单元测试、协议契约测试、故障注入、安全测试和真实仓库任务评测。

### 2.2 典型场景

- 解释代码、追踪调用关系；
- 修复编译、Lint 和测试失败；
- 实现中小型需求；
- 跨文件重构；
- 生成并运行测试；
- Review Git diff；
- 调查问题并输出带证据的结论。

### 2.3 非目标

1. 无人监管地操作生产环境。
2. 默认执行 commit、push、部署、数据库写入或提权。
3. 替代完整 IDE，或训练、托管大模型。
4. 1.0 默认启用复杂多 Agent 群体协作。
5. 自研 Git、Shell、搜索引擎或向量数据库。
6. 承诺第三方 MCP Server 可信。
7. 在没有验证证据时保证生成代码正确。

### 2.4 生产级标准

- **正确性**：协议、状态迁移、调用关联和恢复逻辑有自动化测试。
- **安全性**：最小权限、审批、路径隔离、秘密脱敏和命令策略默认开启。
- **可靠性**：外部调用可超时、取消和有限重试，异常不破坏会话一致性。
- **可恢复性**：进程退出后可由事件和检查点继续。
- **可观测性**：任务、轮次、模型、工具、费用和错误有结构化记录。
- **可维护性**：核心领域类型不依赖供应商 SDK，模块通过小接口协作。
- **可验证性**：最终结果明确列出修改、测试和未验证风险。
- **兼容性**：优先支持 macOS、Linux，后续支持 Windows。

## 3. 设计原则

1. **模型负责判断，程序负责约束**：参数校验、权限、超时、文件冲突、预算、持久化和验证由程序执行。
2. **单主 Agent 优先**：子 Agent 是预算受限、默认只读的高级工具。
3. **事件是事实来源**：重要状态先形成持久化事件，再更新投影和 UI。
4. **副作用集中管理**：文件、进程、网络和外部扩展只能经过 Tool Runtime。
5. **默认安全、逐步授权**：权限按能力、作用域和风险授予，模型不能提升权限。
6. **上下文有预算**：完整输出保存为 Artifact，模型只接收相关且有界的视图。
7. **先确定性、再智能化**：哈希、diff、退出码、校验、权限和恢复不用模型判断。
8. **可替换而不过度抽象**：稳定抽象 Provider、Store、Tool、Approver、Sandbox；早期内部细节保持简单。

## 4. 总体架构

```text
┌─────────────────────────────────────────────────────────┐
│ Frontend: CLI / TUI / future IDE / JSON-RPC client      │
└─────────────────────────┬───────────────────────────────┘
                          │ command + event stream
┌─────────────────────────▼───────────────────────────────┐
│ Application: lifecycle / config / session / approval    │
└──────────────┬──────────────────────────────┬───────────┘
               │                              │
┌──────────────▼────────────────┐  ┌──────────▼───────────┐
│ Agent Runtime                 │  │ Session & Event      │
│ state / loop / plan / limits  │  │ WAL / checkpoint     │
└──────┬───────────┬────────────┘  │ artifact / audit      │
       │           │               └──────────────────────┘
┌──────▼──────┐ ┌──▼──────────────────────────────────────┐
│ Model       │ │ Context Engine                          │
│ Gateway     │ │ rules / retrieval / budget / compact   │
└─────────────┘ └──────────────────┬──────────────────────┘
                                  │
┌─────────────────────────────────▼───────────────────────┐
│ Tool Runtime                                            │
│ registry / validation / policy / approval / scheduling  │
│ file / edit / search / shell / git / MCP / subagent     │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│ Platform: workspace / process / sandbox / network / OS  │
└─────────────────────────────────────────────────────────┘
```

依赖规则：

- `domain` 不依赖 Provider、数据库、UI 或具体工具。
- `agent` 仅依赖领域接口。
- Provider 只适配模型协议，不保存会话或执行工具。
- Context Engine 不产生副作用。
- Tool 必须经统一权限层访问平台。
- UI 只提交命令和消费事件，不直接修改 Runtime 状态。

建议目录：

```text
go/pl/loom/
├── cmd/loom/                  # CLI 入口
├── internal/
│   ├── app/                   # 依赖组装和生命周期
│   ├── domain/                # 稳定领域类型、错误和接口
│   ├── agent/                 # 状态机、循环、计划和预算
│   ├── model/provider/        # Provider 适配
│   ├── context/               # 上下文构建、检索和压缩
│   ├── tool/builtin/          # 内置工具
│   ├── tool/runtime/          # 校验、审批、调度
│   ├── workspace/             # 路径安全、规则和快照
│   ├── process/               # 进程、PTY、取消和输出
│   ├── permission/            # Policy 和 Approver
│   ├── session/               # Store、事件、检查点
│   ├── artifact/              # 大输出内容寻址存储
│   ├── mcp/                   # MCP Client
│   ├── rpc/                   # 未来 IDE/daemon 协议
│   ├── telemetry/             # 日志、Metrics、Trace
│   └── ui/                    # CLI/TUI 渲染
└── testdata/                  # 脱敏、确定性夹具
```

目录随实现逐步创建，不预先制造空包。

## 5. 核心领域模型

### 5.1 标识与层级

持久化对象使用稳定 ID：`SessionID`、`RunID`、`TurnID`、`MessageID`、`ToolCallID`、`EventID`、`ArtifactID`、`CheckpointID`。

```text
Session                         可恢复会话
 ├─ immutable config snapshot
 ├─ Runs[]                      一次用户请求到终态
 │   ├─ Turns[]                 一次模型决策及工具阶段
 │   └─ Outcome
 ├─ Workspace identity
 ├─ Active plan
 └─ Accumulated usage
```

时间统一为 UTC，并由可注入 `Clock` 提供。

### 5.2 消息

消息不能简化为字符串，因为一条 Assistant 消息可包含文本和多个工具调用。

```go
type Message struct {
    ID        MessageID
    Role      Role
    Parts     []ContentPart
    CreatedAt time.Time
    Metadata  map[string]string
}

type ContentPart struct {
    Kind       PartKind
    Text       string
    ToolCall   *ToolCall
    ToolResult *ToolResult
    Artifact   *ArtifactRef
}
```

`PartKind` 至少支持 `text`、`tool_call`、`tool_result`、`artifact_ref`，后续增加图片。模型私有推理不写入持久化记录。

### 5.3 工具

```go
type ToolDefinition struct {
    Name         string
    Description  string
    InputSchema  json.RawMessage
    OutputSchema json.RawMessage
    Capabilities []Capability
    Source       ToolSource
}

type ToolCall struct {
    ID        ToolCallID
    Name      string
    Arguments json.RawMessage
}

type ToolResult struct {
    CallID     ToolCallID
    Status     ToolStatus
    Content    []ContentPart
    Error      *ToolError
    StartedAt  time.Time
    FinishedAt time.Time
    Metadata   map[string]string
}
```

工具错误包含稳定 `code`、安全 `message`、`retryable` 和完整日志引用。

### 5.4 状态机

```text
created → preparing → calling_model
                         │
              ┌──────────┴──────────┐
              │                     │
       awaiting_approval      executing_tools
              │                     │
              └──────────┬──────────┘
                         ↓
                    compacting
                         ↓
                    preparing

任意非终态 → suspended / failed / cancelled
无工具调用且完成判定通过 → completed
```

终态为 `completed`、`failed`、`cancelled`。`suspended` 表示等待用户、凭证或外部条件，可恢复但不占执行资源。

### 5.5 预算

```go
type Limits struct {
    MaxTurns            int
    MaxToolCalls        int
    MaxParallelTools    int
    MaxInputTokens      int64
    MaxOutputTokens     int64
    MaxEstimatedCostUSD float64
    MaxWallTime         time.Duration
    MaxToolOutputBytes  int64
    MaxArtifactBytes    int64
    MaxRepeatedActions  int
}
```

达到软阈值时提示模型收敛或压缩；达到硬阈值时安全终止并允许恢复。

## 6. Agent Runtime

### 6.1 主循环

```text
1. 创建/恢复 Run、Workspace、Policy、Plan 和预算
2. Context Engine 构造请求
3. Model Gateway 返回统一流事件
4. 聚合为 Assistant Message
5. 无 Tool Call：执行完成判定
6. 有 Tool Call：
   a. Schema 和语义校验
   b. 计算 Capability、作用域和风险
   c. 必要时挂起等待审批
   d. 按依赖和冲突调度
   e. 裁剪并保存结果
7. 更新预算、计划和工作区版本
8. 必要时压缩上下文
9. 进入下一轮或终态
```

每个状态迁移必须先以乐观并发方式追加事件，成功后更新内存投影并发布 UI。恢复时通过事件版本避免两个 Runtime 同时推进会话。

### 6.2 完成判定

“模型没有工具调用”不等于成功。判定需要考虑：

- 模型停止原因；
- 未完成计划项；
- 未处理工具错误；
- 修改后是否验证；
- 任务是信息问答还是代码修改；
- 是否预算耗尽、取消或等待审批。

Run Outcome：

- `succeeded`：完成且必要验证通过；
- `completed_unverified`：完成但未充分验证；
- `needs_user`：缺少信息、凭证或审批；
- `budget_exhausted`：预算耗尽，可恢复；
- `failed`；
- `cancelled`。

### 6.3 动态计划

计划是复杂任务的外部状态，不是不可修改的工作流。简单任务可无计划；同一时间最多一个 `in_progress` 项；完成项关联 diff、测试或读取证据；模型提出更新，Runtime 校验状态迁移后应用。

### 6.4 停滞检测

为动作计算指纹：工具名、规范化参数、工作区版本和结果摘要。检测：

- 无环境变化时重复相同调用；
- 编辑在两个版本间震荡；
- 连续相同模型/工具错误；
- 多轮没有新增证据、修改或计划进展。

首次触发时提示模型改变策略，超过阈值后转为 `needs_user` 或失败。

### 6.5 取消

根 `context.Context` 贯穿模型、工具、Store 和 UI。第一次 Ctrl+C 优雅取消，第二次强制退出；取消 Shell 时终止进程组；退出前保存一致检查点。已成功原子写入的文件不静默撤销，最终明确列出。

## 7. Model Gateway

```go
type Model interface {
    Stream(context.Context, ModelRequest) (ModelStream, error)
}

type ModelStream interface {
    Recv() (ModelEvent, error)
    Close() error
}
```

统一事件覆盖消息开始/结束、文本增量、工具调用及参数增量、Usage、警告和错误。Provider 只负责：

- 统一消息与供应商格式转换；
- HTTP、认证、SSE、连接复用；
- 工具参数流聚合；
- 停止原因和 Usage 解析；
- 错误映射、能力矩阵和请求幂等键。

能力矩阵包括 Tool Calling、并行调用、Streaming、Vision、Prompt Cache、上下文和输出上限。

重试规则：连接重置、429、明确 5xx 和未产生响应前的超时可指数退避重试；认证失败、参数错误、上下文超限不可盲目重试；部分响应后状态不明时谨慎处理。所有重试产生事件并计入预算。

秘密优先来自 OS Keychain，其次环境变量；禁止将 Key 写入 Session、Artifact、日志或 Trace。

## 8. Context Engine

### 8.1 组成与优先级

1. 系统安全规则；
2. Provider 协议要求；
3. 用户偏好；
4. 工作区规则（`LOOM.md`、`AGENTS.md`、`CLAUDE.md`）；
5. 用户目标与约束；
6. 计划、预算和验证状态；
7. 最近对话；
8. 相关代码与搜索结果；
9. 最近工具结果；
10. 历史压缩摘要。

### 8.2 预算

\[
B_{context}=B_{model}-B_{output}-B_{safety}
\]

再分配给规则、目标、最近消息、证据和摘要。优先使用 Provider Tokenizer，不可用时采用保守估算。

### 8.3 渐进探索

默认采用：仓库地图 → 文件/精确搜索 → 符号搜索 → 局部读取 → 引用追踪 → 修改 → 诊断和测试。早期优先 `.gitignore` 感知遍历、`rg`、Git 和构建文件；不默认建立全仓 Embedding。

### 8.4 输出裁剪

每个工具返回：

- `model_view`：有界、脱敏摘要；
- `user_view`：UI 视图；
- `artifact_ref`：完整原始输出；
- `metadata`：退出码、大小、截断原因和哈希。

保留错误首尾、路径、行号、测试名、退出码、重现命令、总匹配数和展示范围。当前 `run_cmd` 对 stdout/stderr 分别维护固定内存的 head/tail 预览，并在序列化后再次执行总字节预算校验；Artifact 引用保留在 Canonical Tool Result 中，但 Provider 只向模型发送有界文本视图，避免引用结构重复膨胀上下文。

命令完整输出采用独立的 stdout/stderr 内容寻址 Artifact。执行前创建私有 staging writer，进程运行时边排空 pipe、边计算 SHA-256、边写入临时文件；完成后 `fsync` 并以无覆盖方式原子提交。达到 Artifact 配额后继续排空输出但停止持久化，分别记录观察字节数、保存字节数、预览截断和 Artifact 截断。Artifact 必须先提交，再由 Tool Result/Event 引用；提交后、事件落盘前崩溃产生的无引用 Blob 由后续 GC 清理，绝不先写悬空引用。

### 8.5 压缩

接近软阈值时生成结构化摘要：用户目标、约束、已确认事实及证据、读写文件、Git 状态、验证结果、未解决问题、计划、下一步和 Artifact 引用。

压缩不删除原始事件；摘要记录来源范围；保留最近轮次和当前编辑片段；模型压缩失败时使用确定性裁剪。

### 8.6 Prompt Injection

代码、文档、命令输出、网页和 MCP 结果均不可信：

- 用明确边界分隔规则和外部内容；
- 标记来源与信任级别；
- 外部内容不能改变权限；
- 敏感数据不因内容要求自动读取；
- 网络结果不能直接触发高风险写操作；
- 防线在宿主权限系统，而非仅靠关键词过滤。

## 9. Tool Runtime

### 9.1 生命周期

```text
解析调用 → Schema 校验 → 语义校验/规范化
→ 计算能力、作用域、风险和读写集 → Policy
→ 用户审批 → 获取资源锁 → 执行
→ 限制/脱敏/保存输出 → Tool Result + 审计
```

```go
type Tool interface {
    Definition() ToolDefinition
    Prepare(context.Context, ToolCall) (PreparedCall, error)
    Execute(context.Context, PreparedCall) ToolResult
}
```

`Prepare` 必须无副作用，负责参数、路径、命令、风险、资源读写集和审批描述。`Execute` 只接受带授权上下文且参数哈希未变化的 `PreparedCall`。

### 9.2 内置工具

命名规范：动词 + 短名词、snake_case、优先日常词汇；`git_` 为命名空间前缀。

当前集合（12 个）：

| 工具 | 风险 | 职责 |
|---|---:|---|
| `read_file` | R1 | 分页读取 UTF-8 文件（带行号，offset/limit，二进制拒绝；编辑前必须先读） |
| `list_dir` | R1 | 单层目录罗列（kind/size/mode/mtime，字典序，200 条截断，不递归） |
| `glob` | R1 | 按 glob 模式发现文件（如 `**/*.go`），字典序，200 条截断 |
| `search` | R1 | 正则/字面内容搜索（path/glob/type/context/case/fixed_strings）；`rg --json` 引擎优先，Go 实现回退，`.gitignore` 默认生效 |
| `write` | R2 | 创建（自动建父目录）或整文件覆写；审批展示路径/字节数/创建或覆盖；堵 `run_cmd` + heredoc 旁路 |
| `edit` | R2 | `old_string`/`new_string` 精确替换（唯一匹配或 `replace_all`）；陈旧检测内部化（文件自上次读取后被外部修改则报可行动错误）；`expected_hash` 仅作可选高级校验 |
| `run_cmd` | R2/R3 | 直接执行程序（非 shell）；仅 `program` 必填，其余参数均有默认值；需要 shell 语法时显式 `sh -c` 并承担更高审批风险 |
| `git_status` | R1 | 仓库状态（porcelain v2，`repo_root` 默认 `"."`） |
| `git_diff` | R1 | 变更内容（`repo_root` 默认 `"."`，可选 `base`） |
| `git_log` | R1 | 提交历史（`limit` 分页） |
| `lint` | R2 | 项目代码诊断：按标记文件确定性检测引擎（go.mod → golangci-lint/go vet，package.json → eslint，pyproject.toml → ruff，compile_commands.json → clang-tidy），沙箱内执行，输出归一化结构化 diagnostics |
| `web_fetch` | R3 | HTTP/HTTPS GET 抓取网页：HTML 转 markdown（可 text/raw），SSRF 拨号时防护（默认拒绝私网/环回），重定向限 5 跳，大小截断走 artifact 溢出，成功响应进程内缓存 15 分钟 |

分工边界（写入 system prompt）：找文件用 `glob`，找内容用 `search`，看目录用 `list_dir`，读文件用 `read_file`，新建/覆写用 `write`，局部修改用 `edit`，构建/测试/任意程序用 `run_cmd`，仓库信息用 `git_*`，代码诊断用 `lint`，网页内容用 `web_fetch`。

合并与退役：`replace_text` 与 `apply_patch` 合并为 `edit`；`search_text` 重构为 `search`；`list_directory` 更名 `list_dir`；`run_command` 更名 `run_cmd`。旧 Session 中的已退役工具名在恢复时按 `unknown_tool` 语义处理。

进程工具与长期进程分开建模，避免一次 Tool Call 永久阻塞循环：后续 `start_process`、`poll_process`、`stop_process` 独立演进。

### 9.3 并发

仅在模型返回独立调用、工具允许并发、读写集不冲突且不依赖前一结果时并发。多个读取可并行；同文件写串行；Git index 全局串行；写集未知的 Shell 保守地持有工作区写锁；未知 MCP 能力默认串行。

### 9.4 失败语义

普通文件不存在、测试失败等作为结构化 Tool Error 返回模型，不直接终止 Runtime。只有安全边界破坏、持久化一致性失败、进程无法回收、用户取消、硬预算或持续不可用才终止 Run。

## 10. Workspace 与编辑一致性

### 10.1 工作区身份

保存规范化根路径、文件系统标识、Git 根、分支、初始 HEAD 和工作树摘要。恢复时身份不符、危险分支变化或差异过大需用户确认。

### 10.2 路径安全

每次访问清理 `.`/`..`、解析符号链接、确认最终路径位于允许根内、检查敏感策略，并在打开后再次确认以降低 TOCTOU 风险。默认拒绝 `.git` 可变内部文件、凭证目录、设备文件、Socket、命名管道、超大或二进制文件。

### 10.3 乐观并发与原子写

读取返回内容哈希、大小、修改时间和可用的文件 ID。编辑携带 `expected_hash`；不匹配则返回 `FILE_CHANGED`，必须重新读取，禁止覆盖用户新修改。

写入使用同目录临时文件、权限保留、可选 `fsync` 和原子 rename，完成后记录新哈希。多文件无法获得通用事务，因此采用变更集检查点和补偿回滚；回滚也验证版本，绝不覆盖后续用户编辑。

### 10.4 Git 保护

禁止默认执行 `reset --hard`、`clean -fd`、强推、自动 commit、rebase 或切分支。最终结果区分 Loom 修改、会话前已有修改、会话期间外部修改和无法归因修改。

## 11. Shell、进程与沙箱

优先使用 `Program + Args`，只有明确需要管道或重定向时才通过 Shell 执行并提高风险等级。

```go
type CommandSpec struct {
    Program       string
    Args          []string
    WorkingDir    string
    EnvAllowlist  []string
    Timeout       time.Duration
    MaxOutput     int64
    NetworkPolicy NetworkPolicy
}
```

要求：

- 独立进程组；
- stdout/stderr 并行且持续消费，任何预览或 Artifact 配额都不得导致停止排空 pipe；
- stdout/stderr 分别使用固定内存 head/tail 预览，并分别流式写入 staging Artifact；
- Artifact 采用 SHA-256 内容寻址、私有临时文件、`fsync` 和无覆盖原子提交，相同内容自动去重；
- 分别记录每条流的观察字节数、预览截断、Artifact 截断和 Artifact 引用；达到 Artifact 上限后继续读取但丢弃后续持久化内容；
- 超时先温和终止，宽限后杀进程组；命令结束时即使原执行 Context 已取消，也使用独立短时 Context 提交已捕获 Artifact；
- 记录退出码、信号、耗时、截断和资源使用；
- 非交互命令关闭 stdin；交互命令走独立 PTY 工具并审批；
- 最小环境变量，剔除模型和云凭证；日志秘密脱敏。

沙箱逐步增强：逻辑路径/命令/网络策略 → 进程组与资源限制 → macOS Seatbelt 或 Linux namespace/seccomp/cgroup → 容器/远程沙箱。没有 OS 沙箱时必须如实标记隔离级别。

## 12. 权限与安全

### 12.1 Capability 和风险

能力包括：`fs.read`、`fs.write`、`fs.delete`、`process.exec`、`process.background`、`process.interactive`、`network.connect`、`git.read`、`git.write`、`git.remote_write`、`secret.use`、`workspace.outside`、`agent.delegate`，并带路径、主机、命令、时间和次数作用域。

风险：

- R0：纯计算；
- R1：工作区普通源码和 Git 只读；
- R2：工作区写入、普通构建测试、依赖下载；
- R3：删除、工作区外、网络写、交互进程；
- R4：远程 Git 写、部署、凭证、提权、不可逆操作。

默认 R0/R1 自动允许；R2 首次提示并可会话授权；R3 精确审批；R4 默认拒绝，需专用能力显式开启。

### 12.2 Policy

决策为 `allow`、`deny`、`ask`。输入包括工具来源、能力、作用域、风险、工作区信任、用户策略、临时授权和参数摘要。审批可允许一次、会话内精确动作、指定作用域，或拒绝。授权绑定调用参数哈希，参数变化立即失效。模型不能创建持久化授权。

### 12.3 威胁模型

重点防御：恶意仓库 Prompt Injection、模型幻觉破坏命令、路径/符号链接逃逸、Shell 注入、恶意 MCP、日志泄密、恢复重复非幂等操作、依赖安装脚本、后台进程逃逸和用户改动覆盖。

MCP Server 使用最小环境；首次连接展示来源和工具清单；工具变化重新确认；所有 MCP Tool 转换为 Loom Capability 并经过相同 Policy；远程连接需 TLS、主机白名单和凭证引用。

## 13. 事件、持久化与恢复

### 13.1 事件

关键事件：

```text
session.created / run.created / run.state_changed
user.message_added
model.request_started / model.response_completed / model.request_failed
tool.call_prepared
permission.requested / permission.resolved
tool.execution_started / tool.execution_completed
file.changed / plan.revised / context.compacted
checkpoint.created / budget.updated
run.completed / run.failed / run.cancelled
```

高频文本 Delta 可聚合，不必永久逐条保存；审批、调用边界、文件变化和终态必须持久化。

### 13.2 Store

```go
type SessionStore interface {
    CreateSession(context.Context, Session) error
    AppendEvents(ctx context.Context, expectedVersion int64, events []Event) error
    AppendEventsAndCheckpoint(ctx context.Context, expectedVersion int64, events []Event, checkpoint Checkpoint) error
    LoadEvents(ctx context.Context, id SessionID, after int64) ([]Event, error)
    SaveCheckpoint(context.Context, Checkpoint) error
    LoadLatestCheckpoint(context.Context, SessionID) (Checkpoint, error)
}
```

推荐 SQLite WAL 保存 Session、Run、Event、Approval、Usage 和 Artifact 元数据；大输出以 SHA-256 内容寻址存文件。当前 Artifact Store 已实现私有目录、流式 staging、单 Artifact 配额、同步、无覆盖原子提交、内容去重、读取时摘要校验和失败清理；`run_cmd` 为 stdout/stderr 分别生成 Artifact。SQLite schema v2 增加按 Session 聚合的 Artifact 引用索引，Checkpoint 与新增引用在同一事务内提交；v1 数据库打开时会扫描已有 Checkpoint 回填索引。显式 `loom gc` 读取引用快照，仅删除超过 24 小时宽限期的无引用 Blob 与崩溃残留 staging 文件。推进 Run 时 Event batch 与覆盖该版本的 Checkpoint 必须在同一事务内提交，避免事件已提交但投影快照缺失。数据库仍需备份和损坏诊断；Artifact 仍需 Session 级/全局配额和可配置保留策略。

### 13.3 Checkpoint 与恢复

在用户消息后、完整模型响应后、工具批次前后、文件修改后、压缩后和终态前创建检查点。包含聚合版本、状态、消息/计划引用、预算、工作区身份、相关哈希、待审批/待执行调用和后台进程引用。

恢复规则：

- 未形成完整模型响应则标记中断并重新请求；
- 只读幂等工具可重试；
- 文件写通过操作 ID 和前后哈希确认是否完成；当前 `replace_text`/`apply_patch` 已实现前后哈希三态核对；
- 普通 Shell 默认非幂等，不自动重放；当前 `run_cmd` 结果未知时阻断恢复并要求人工核实；
- 后台进程只有在 PID、启动时间、可执行身份和 owner token 均可确认时才能接管；当前尚未实现后台进程 registry，因此不宣称可恢复；
- 旧审批在恢复后失效；只读调用可明确标记为可重试，但当前不会静默自动执行；
- 恢复决定必须形成审计结果；跨进程 lease/fencing 完成前，不提供 exactly-once 副作用保证。

## 14. 验证闭环

验证按成本递增：格式/语法 → Diagnostics → 相关单测 → 相关目标构建 → 模块测试 → 全仓验证。确定性发现 `go.mod`、Bazel 文件、`package.json`、`pyproject.toml`、`Cargo.toml`、Maven/Gradle、Makefile、CI 和项目规则，优先使用项目明确命令。

最终结果必须区分：已运行且通过、已运行但失败、未运行及原因、环境阻塞和剩余风险。

自修复约束：不通过删除正确测试、关闭 Lint/ASan 或引入大范围无关改动来获得绿色结果；连续失败达到阈值后停止并总结；怀疑测试错误时提供证据并请求决定。

## 15. 扩展与前端协议

子 Agent 作为 `delegate_task` 工具：独立上下文和预算、默认只读、最大递归深度 1、只返回结构化结论和证据，主 Agent 负责验证及修改。适用于大型仓库并行探索、资料研究和独立 Review。

MCP 是主要工具扩展协议，先支持 stdio，后续支持标准远程传输、健康检查、超时和重连。

未来 `loom serve` 暴露版本化 JSON-RPC 或本地 gRPC：创建/恢复/取消 Run、订阅事件、提交审批、查询计划/Artifact/diff、提供编辑器上下文和协议版本协商。客户端不能直接访问 SQLite 表。

## 16. 配置

配置优先级：编译默认值 < 用户配置 < 工作区配置 < 环境变量 < CLI 参数 < 会话临时设置。秘密只保存引用。

Session 创建时保存脱敏配置快照：模型、Limits、Policy 版本、工具和 MCP、上下文策略、沙箱等级。恢复时使用原快照保证可解释性；安全策略只能保持或收紧，放宽需重新审批。

项目规则从用户全局到工作区根、当前目录逐层发现；更深目录规则只作用于子树。兼容 `LOOM.md`、`AGENTS.md`、`CLAUDE.md`。规则可影响行为但不能提升权限。

## 17. 可观测性、隐私与错误

使用 `log/slog`，字段包括 session/run/turn/tool_call ID、组件、耗时、状态码、Provider/模型、Token/费用、I/O 大小和重试次数。默认不记录完整 Prompt、代码、工具输出、凭证或私有推理。

Metrics：Run 成功/未验证/失败率、首字和总延迟、工具成功/超时率、Token/费用、审批率、压缩比、编辑冲突、测试修复轮次、停滞和恢复成功率。

Trace 层级：`run → context.build/model.request/tool.batch/compact/verification`，内容只记录哈希、大小和 Artifact ID。安全审计独立记录高风险命令、审批、越界访问、秘密使用、MCP 变化、远程写和策略变化，模型工具不能删除。

错误分类：`invalid_input`、`permission`、`conflict`、`unavailable`、`rate_limited`、`timeout`、`cancelled`、`budget`、`security`、`internal`。跨模块错误包含稳定码、安全消息和可重试性。

幂等性：事件 ID 去重；文件编辑使用 `expected_hash + operation_id`；Approval 绑定参数哈希；普通 Shell 默认非幂等。内部 Event channel 有界，高频 Delta 合并，慢 UI 不能阻塞进程输出排空。

## 18. 性能与容量目标

初始目标：

- CLI 启动到可输入 P95 < 300ms（不含首次迁移）；
- 收到模型 Delta 后 UI 展示 P95 < 50ms；
- 内置只读工具调度开销 P95 < 10ms（不含 I/O）；
- 空闲 Runtime 目标 < 100MiB；
- 工具预览默认 <= 64KiB，完整输出转 Artifact；
- 默认最多 4 个并发工具；
- 有 Checkpoint 的会话恢复目标 < 2s；
- 所有配额可配置且存在硬上限。

## 19. 测试与评测

### 19.1 自动化测试

单元测试覆盖状态迁移、Provider 转换、流式参数聚合、Schema、路径安全、Policy、预算、裁剪/脱敏、文件冲突、循环检测、事件重放。

Provider 契约测试使用脱敏夹具覆盖文本、多工具、参数分片、Usage、429/5xx、断流、非法 JSON 和上下文超限。Tool 契约要求 `Prepare` 无副作用、取消及时、输出有界、审计完整、不越权。

集成测试在临时 Git 仓库验证编辑闭环、用户并发修改、Ctrl+C、进程树清理、崩溃点恢复、SQLite 迁移、恶意 MCP、规则作用域和 Artifact GC。

安全测试覆盖路径穿越、符号链接竞态、Shell 注入、Prompt Injection、秘密泄漏、超长输出、后台进程逃逸和授权重放。

### 19.2 Agent Eval

版本化任务包含仓库快照、用户请求、工具/预算、隐藏测试、必须/禁止变更、安全断言和评分器。覆盖单文件修复、跨文件功能、构建修复、测试补充、大仓定位、错误需求识别、注入防御、用户修改保护、工具故障和预算任务。

指标：Pass@1、多次运行成功率、隐藏测试、无关 diff、Token/费用/耗时、Tool Call 数、安全违规和用户介入。随机任务必须重复运行，报告均值、分位数和置信区间，而非最佳结果。

## 20. CLI 体验

```text
loom                         交互模式
loom run <prompt>            单次运行
loom resume [session-id]     恢复
loom sessions                会话列表
loom inspect <session-id>    计划、事件、费用、变更
loom diff [session-id]       Loom 归因修改
loom config                  非敏感配置
loom auth                    凭证引用
loom mcp                     MCP 管理
loom doctor                  环境与安全诊断
loom eval                    运行评测
```

流式 UI 展示模型可见文本、工具名称和安全参数摘要、执行状态、审批、计划、Token/费用和最终验证；不展示私有推理。非 TTY 环境输出稳定 JSON Lines，便于自动化。退出码区分成功、未验证、需用户、失败、取消和配置错误。

## 21. 分阶段实施路线

### 21.1 实现进展快照（2026-07-23）

当前代码已完成 Phase 0、Phase 1、Phase 2 的可运行安全基线，并进入 Phase 3 的持久化恢复主链路。这里的“完成”指对应主链路已经落地并通过单元测试，不代表已经满足第 22 节的全部 1.0 发布门槛。

| 阶段 | 状态 | 当前实现 |
|---|---|---|
| Phase 0 | 已完成 | 领域状态机、强类型 ID、Canonical Message/Tool/Event 协议、Limits、Fake Model/Tool/Store/Approver、Tool Registry、基础 Policy、工作区路径边界和 Go/Bazel 构建骨架 |
| Phase 1 | 已完成 | OpenAI-compatible Provider、Chat Completions/Responses 流协议、部分流与断流处理、`read_file`、`list_dir`、`search_text`、Context Manifest、Transcript 投影和 `loom run` 闭环 |
| Phase 2 | 安全基线已完成 | 哈希保护编辑、原子写、严格补丁、PreparedCall 绑定与执行前复验、审批和副作用意图事件、隔离命令执行、Git status/diff、网络默认拒绝、环境秘密剔除和 CLI 工具装配 |
| Phase 3 | 主链路进行中 | SQLite WAL Event Store、schema v2 与 v1→v2 回填迁移、Canonical Transcript、原子 Event+Checkpoint+Artifact 引用索引提交、`sessions`/`inspect`/`resume`/`gc`、模型请求审计、安全 Reconciler、内容寻址 Artifact Store、流式 staging、命令 stdout/stderr Artifact 和 grace-period 孤儿 GC 已实现；备份、lease/fencing、完整故障矩阵、更复杂升级迁移和配额/保留策略仍待实现 |
| Phase 4～7 | 未开始 | 按后续路线实施 |

当前包与能力：

- `internal/domain`：状态机、消息、事件、工具、计划、预算、Context Manifest、`ArtifactStore` 和 `StagedArtifact` 等核心领域模型。
- `internal/agent`：模型—工具循环、预算检查、审批路由、PreparedCall 复验、模型请求生命周期审计和恢复 Reconciler。Event 与对应 Checkpoint 原子提交；副作用执行前必须先持久化 intent；提交失败时禁止 dispatch；旧审批在恢复时失效；同一 Tool Call 已有结果时不会在当前 Loop 内自动重放。
- `internal/model/openai`：支持 OpenAI-compatible Chat Completions 与 Responses API，包含 SSE 聚合、Usage、工具调用、生命周期事件和兼容网关直接 EOF 的处理。
- `internal/tool/builtin`：工作区内的有界只读文件、目录和文本搜索。
- `internal/tool/edit`：`replace_text` 和单文件 strict unified diff `apply_patch`。两者要求 `expected_hash`，冲突返回结构化错误，禁止覆盖并发修改；Prepare 在可确定时记录写入前后哈希，供崩溃恢复执行三态核对。
- `internal/workspace`：路径规范化、符号链接与敏感路径拒绝、文件 Snapshot、同目录临时文件、`fsync`、原子 rename、权限和扩展元数据保留。无法安全保留元数据时 fail closed。
- `internal/artifact`：SHA-256 内容寻址 Blob Store；支持私有目录、流式 staging、单对象配额、观察/保存字节统计、截断后继续消费、`fsync`、无覆盖原子提交、内容去重、读取校验、幂等 Abort，以及引用白名单 + 宽限期驱动的孤儿 Blob/staging GC。
- `internal/process`：`Program + Args` 非 Shell 执行、最小环境、凭证变量剔除、stdout/stderr 并行排空、独立 head/tail 预览、外部流式 Writer、逐流字节/截断统计、独立进程组、超时/取消回收、可执行文件哈希复验和沙箱抽象。
- `internal/tool/command`：R2 `run_cmd`，审批摘要展示程序、参数、工作目录、环境变量名、超时与网络策略，不展示环境变量值；命令执行前创建 stdout/stderr staging Artifact，完成后先提交 Blob 再形成 Tool Result 引用，模型只接收严格有界的 head/tail 预览。
- `internal/tool/gittools`：只读 `git_status` 和 `git_diff`，使用固定 Git 子命令、literal pathspec、最小环境、超时和输出上限。
- `internal/session`：SQLite WAL Event Store、乐观版本控制、纳秒级稳定排序、schema v2 版本检查和 v1 Artifact 引用回填迁移、Checkpoint/Artifact 引用原子提交、全局引用快照、只读一致性检查和 Session Inspection。
- `cmd/loom`：注册 Phase 1/2 工具；R2/R3 在 TTY 中精确提示审批，非 TTY 默认拒绝；公开支持 `run`、`sessions`、`inspect`、`resume`、`gc` 和 `version`。`gc` 使用 24 小时安全宽限期并输出结构化清理统计。

Phase 2 当前安全保证：

1. 文件编辑使用 SHA-256 乐观锁；`expected_hash` 不匹配时不写入。
2. 写入采用同目录临时文件、同步、执行前二次哈希检查和原子替换；符号链接、特殊文件、敏感目录与工作区逃逸默认拒绝。
3. 审批绑定规范化参数、风险、能力和读写路径；执行前重新核对模型 Tool Call、Registry Definition 和 PreparedCall。
4. 命令不经过 Shell，默认不继承模型、云服务和常见 Token/Secret/Credential 环境变量。
5. 命令超时或取消时先终止进程组，再强制回收；输出 pipe 有硬关闭边界，避免脱离进程组的后代无限持有输出管道。
6. 网络默认拒绝。macOS 在 `sandbox-exec` 可用时使用 Seatbelt；Linux 当前没有满足约定的 namespace/seccomp/cgroup 实现，因此 `run_cmd` 在 Linux 上 fail closed，而不是降级为无沙箱执行。Seatbelt profile 的读策略为宽读 + 显式凭证拒绝：现代运行时（Go/Rust）在路径级读限制下无法启动（dyld 需要 `file-read*` 宽集），因此读取全面放开，但对 `~/.ssh`、`~/.gnupg`、`~/.aws`、`~/.kube`、`~/.docker`、`~/.config/gcloud`、`~/Library/Keychains`、`~/.netrc`、`~/.git-credentials`、`~/.env`、`credentials.json`、`service-account.json` 等凭证路径逐条 deny；写入仍限定工作区与临时目录，网络默认拒绝。
7. Git 工具只允许固定只读操作，路径按 literal pathspec 传递，避免 pathspec magic 扩大读取范围。

已知限制与后续工作：

- SQLite Event Store 与进程重启恢复已落地，但跨进程 lease/fencing 尚未实现；乐观版本能阻止两个恢复者同时提交同一版本，不能阻止旧 owner 在外部副作用系统继续运行。
- `replace_text`/`apply_patch` 可按当前文件 hash 判定“已应用、未应用、外部冲突”；普通命令、后台进程和不支持幂等键/状态查询的外部 API 仍必须进入人工核实，绝不自动重放。
- 模型请求已记录 started/failed 及 Context Manifest hash，但 provider 不支持幂等查询时，崩溃后的重复计费和已生成但未落盘响应无法自动确认。
- 当前已实现环境变量秘密剔除和审计 payload 最小化，但跨 Model/UI/Artifact/Trace/MCP 的完整秘密分类、脱敏和不可导出 handle 管线仍属于 Phase 5。
- 大型命令输出已流式转存独立 stdout/stderr 内容寻址 Artifact；单 Artifact 默认仍受 `MaxArtifactBytes` 限制，超限后继续排空进程输出并明确标记 Artifact 截断。引用索引和显式 24 小时 grace-period 孤儿 GC 已实现，但自动调度、Session 级/全局配额、可配置保留策略和 Session 删除后的引用回收流程尚未实现。
- Linux 生产沙箱尚未实现；在此之前，Linux 命令执行保持 fail closed。
- CLI 尚未实现 `diff`、`config`、`auth`、`mcp`、`doctor` 和 `eval`；`resume` 当前是“恢复/继续同一 Session 并追加新 Prompt”，后续需拆分透明重试、Session continuation 和人工 resolution 命令。
- 最终变更归因目前依赖 `git_status`/`git_diff` 和 `file.changed` 事件，尚未形成跨恢复的完整归因报告。

当前验证基线：

- `bazel test //go/pl/loom/...`：14 个测试目标全部通过。
- `bazel build //go/pl/loom/... --platforms=@rules_go//go/toolchain:linux_amd64`：通过。
- macOS 本机构建、CLI `version` 冒烟、lint 和 `git diff --check`：通过。
- 安全回归覆盖路径穿越、符号链接、敏感路径、哈希冲突、PreparedCall 篡改、Git pathspec、环境秘密剔除、无沙箱 fail closed、head/tail 输出边界、staging commit/abort/去重、Artifact 配额截断后持续排空、stdout/stderr 独立完整保存、Provider 上下文不重复展开 Artifact 引用、schema v1 引用回填、跨 Checkpoint 引用保留、GC 宽限期/引用保护/取消/残留 staging 清理、CLI GC、命令超时/取消和进程后代持有 pipe 等场景。

### Phase 0：规范、架构骨架与安全基座

范围：规范化状态机、Canonical Model/Transcript/Tool 协议、领域事件、Fake Model/Tool/Store、CLI 空壳、Go/Bazel 构建，以及在任何真实文件或进程工具前必须具备的最小安全基座：工作区路径边界、Tool Registry 不可绕过、环境变量 allowlist、R0/R1/R2 基线 Policy、日志脱敏、超时与进程组清理。平台沙箱不可用时，运行时必须 fail closed 或仅开放只读能力。

验收：状态迁移表和不变量全覆盖；在线流与事件重放生成相同 Transcript；可用 Fake 组件完成无副作用 Run；安全基座回归通过；`go test` 和 Bazel 测试通过。

### Phase 1：只读 Agent 闭环

范围：一个 Provider、Canonical Streaming、`read_file`/`list_dir`/`search_text`、工具注册、轮次、预算、Context Manifest 和只读 Session 投影。

验收：Agent 可自主搜索并回答仓库问题；Tool Call ID 正确关联；部分流、断流和重试符合协议；错误能返回模型；取消和最大轮次有效；完整可见对话可以持久化、分页恢复和确定性渲染。

### Phase 2：安全编辑、隔离执行与验证

范围：`replace_text`/`apply_patch`、哈希冲突、原子写、PreparedCall 复验、副作用操作日志、`run_cmd`、Git status/diff、权限审批、秘密出口策略、网络默认拒绝和平台可用的 OS 沙箱。

验收：能完成“定位—修改—测试—失败再修复”；不覆盖用户并发修改；审批对象与实际执行完全一致；命令超时清理进程组；不可确认的副作用进入 `outcome_unknown` 而不自动重放；最终列出 diff 和验证。无法建立约定沙箱时不得自动执行不可信仓库命令。

### Phase 3：持久化、完整会话与流式 UX

范围：SQLite Event Store、Canonical Transcript Projection、Artifact、Checkpoint、恢复、完整流式事件、会话导出/删除、结构化日志、备份/迁移/GC 和故障矩阵。

验收：在线运行、纯事件重放和 Checkpoint 恢复得到相同 Transcript；在模型响应、Artifact 提交、文件写入、Shell 执行和 SQLite commit 的规定故障点注入后，不重复危险副作用；Artifact 无悬空引用且大型输出不导致内存失控；Session 删除满足数据生命周期约定。

### Phase 4：上下文工程

范围：规则层级、Token 预算、Repository Map、输出裁剪、结构化压缩、停滞检测。

验收：长任务多次压缩后保留约束和修改状态；上下文不超模型上限；重复循环可检测并停止。

### Phase 5：生产安全强化

范围：版本化 Threat Model、跨工具敏感数据流策略、高级秘密代理、平台沙箱强化、防篡改审计、安全测试与隐私模式。核心 Capability、敏感路径、网络默认拒绝和基础沙箱已经在 Phase 0～2 前置。

验收：高优先级威胁均映射到 prevention/detection/recovery/test；无审批不能越界、删除或远程写；敏感读取不能未经精确审批流向模型、网络或 MCP；日志、Artifact、Trace 和备份扫描无已知秘密。

### Phase 6：扩展生态与本地 Daemon

范围：MCP stdio、后台进程、LSP 只读能力、只读子 Agent、版本化 RPC、Runtime lease/fencing、本地认证、事件游标和断线续传。

验收：第三方工具使用统一权限和审计；异常 Server 可隔离回收；同一 Run 不会被两个 owner 推进；客户端断开不隐式取消 Run；IDE 客户端无需链接内部包即可安全驱动和恢复 Run。

### Phase 7：生产发布

范围：Eval 基线、性能优化、兼容矩阵、数据库迁移策略、崩溃报告、发行和升级。

验收：核心 Eval 达到预设成功率且安全违规为零；macOS/Linux CI 稳定；升级和回滚经过演练；关键 SLO 有监控。

## 22. 1.0 发布门槛

必须同时满足：

1. 至少两个模型 Provider 或一个 Provider 加 OpenAI-compatible 适配通过契约测试。
2. 文件、搜索、编辑、Shell、Git 工具闭环稳定。
3. 工作区边界、哈希冲突、原子写、进程组取消和权限审批默认开启。
4. SQLite/Artifact/Checkpoint 可从故障注入恢复。
5. 上下文预算和压缩可支撑长任务。
6. 最终结果包含变更归因、验证证据和剩余风险。
7. 安全回归无高危失败，秘密不进入默认日志。
8. Eval 报告包含成功率、费用、耗时、稳定性和安全指标。
9. macOS、Linux 构建测试通过，无已知高危漏洞。
10. 任何尚未实现的隔离或保证在产品中明确披露。
11. 在线运行、事件重放和 Checkpoint 恢复产生相同的 Canonical Transcript。
12. 不可确定是否完成的外部副作用进入 `outcome_unknown`，绝不自动重放或误报成功。
13. 无可用 OS 沙箱时不得自动执行不可信仓库命令；网络默认拒绝。
14. Session、Artifact、日志、Trace 和备份通过秘密泄漏与数据删除回归测试。
15. OTel/Langfuse 为可选、异步、默认关闭内容采集的派生观测后端，其失败不影响 Run。
16. 发布产物具备签名、SBOM、构建来源证明、依赖漏洞扫描和经过演练的原子升级/回滚。
17. Runtime lease/fencing 能防止多进程重复推进同一 Run 和重复产生副作用。

## 23. 关键决策记录

| 决策 | 选择 | 原因 |
|---|---|---|
| 核心语言 | Go | 进程、并发、取消、跨平台单二进制和长期维护适合 Agent Harness |
| 主模式 | 单主 Agent ReAct | 上下文连续、冲突少、成本和调试复杂度低 |
| 状态来源 | 事件日志 + Checkpoint | 支持审计、恢复和前端订阅 |
| 持久化 | SQLite + 内容寻址 Artifact | 本地可靠、部署简单、大输出不挤入数据库 |
| 编辑并发 | 哈希乐观锁 + 原子写 | 防止覆盖用户修改，行为可验证 |
| 扩展 | MCP/子进程/RPC | 避免 Go Plugin ABI，隔离边界清晰 |
| 权限 | Capability + scope + risk | 比按工具名授权更精确、可组合 |
| 子 Agent | 按需、默认只读 | 降低 Token、文件冲突和不可解释性 |
| 检索 | 精确搜索/LSP 优先 | 对代码更可靠，先避免向量库复杂度 |
| 推理记录 | 不持久化私有思维 | 保护隐私，审计聚焦动作、证据与结果 |

## 24. 开放问题

以下问题在对应阶段用 ADR 固化：

1. 首批 Provider 选择及统一 Tool Schema 的最小公分母。
2. SQLite 驱动选择：纯 Go 与 CGO 的发布、性能权衡。
3. macOS/Linux 沙箱的统一能力模型和降级行为。
4. 精确 Tokenizer 的依赖和离线可用性。
5. `apply_patch` 的语法、模糊匹配容忍度及冲突展示。
6. RPC 采用 JSON-RPC、ConnectRPC 还是 gRPC。
7. Artifact 默认保留期限、加密和安全擦除策略。
8. Windows 进程树、路径和沙箱支持范围。
9. Eval 任务许可、脱敏和可重复运行方式。

所有开放问题都不得阻塞早期闭环，但不得以临时实现突破既定安全边界。

## 25. 规范化执行模型

本章及之后的“必须”“不得”属于规范性要求，优先于前文的示意性伪代码。

### 25.1 生命周期、阶段与结果分离

不得用单个 `status` 同时表示生命周期、执行阶段和最终结果：

```go
type Lifecycle string // active, suspended, terminal
type Phase string     // preparing, calling_model, awaiting_approval, executing_tools, compacting
type Outcome string   // succeeded, completed_unverified, needs_user, budget_exhausted, failed, cancelled
type SuspensionReason string // approval, clarification, credential, external_condition, budget, outcome_unknown
```

不变量：

- `terminal` 必须有 Outcome，非终态不得有最终 Outcome；
- `suspended` 必须有 SuspensionReason，且不持有执行资源；
- `awaiting_approval` 只能有已持久化且未过期的 PreparedCall；
- `executing_tools` 必须持有有效 Runtime Lease 和 fencing token；
- 一个 Session 可有多个 Run，但同一 Run 只能有一个 active owner；
- 一个 Turn 可有多个 Model Attempt，但最多一个 Attempt 被接受为 Canonical Response。

主要迁移：

| 当前阶段 | 命令/事实 | 下一阶段/结果 |
|---|---|---|
| preparing | context ready | calling_model |
| calling_model | complete response with tools | awaiting_approval 或 executing_tools |
| calling_model | complete response without tools | terminal 或 preparing（继续收敛） |
| calling_model | partial/failed attempt | retry、suspended 或 terminal |
| awaiting_approval | all required approved | executing_tools |
| awaiting_approval | denied | preparing，将拒绝作为工具结果返回模型 |
| awaiting_approval | timeout/client unavailable | suspended(approval) |
| executing_tools | batch completed | compacting 或 preparing |
| executing_tools | indeterminate effect | suspended(outcome_unknown) |
| compacting | summary committed | preparing |
| 任意活动阶段 | budget exhausted | suspended(budget) 或 terminal(budget_exhausted) |
| 任意非终态 | cancel | terminal(cancelled) |

每次迁移由 `Command → validated Events → State Projection` 完成。同一事务内校验 aggregate version、去重 Command/Event ID、分配连续 sequence、写事件并更新必要 Projection；提交后才发布给 UI/RPC。

### 25.2 Tool Batch

Tool Batch 保存调用顺序、依赖 DAG、PreparedCall、审批状态和 Execution 状态。独立调用可并行，但结果在 Canonical Transcript 中按原始 Tool Call Index 关联；UI Timeline 可同时显示实际开始和完成顺序。部分失败不会隐式取消已运行调用；未启动调用是否继续由批次策略决定并形成事件。

### 25.3 用户交互

审批和普通澄清必须分离：

- `permission.requested/resolved`：授权副作用；
- `interaction.requested/resolved`：问题、补充信息和 steering；
- `run.steering_requested`：用户要求改变方向，在安全点取消未启动动作并创建新 Turn；
- 用户回答必须绑定 Request ID，重复提交幂等。

## 26. Canonical Model Protocol

### 26.1 请求、尝试与流事件

每次逻辑模型调用有 `ModelRequestID`，每次网络尝试有 `AttemptID`。重试不得覆盖失败 Attempt；只有完整通过校验的 Attempt 才能提交 Canonical Assistant Message。

`ModelRequest` 必须包含模型固定版本/别名解析结果、Canonical Messages、Tool Definitions 及版本、输出上限、能力要求、Context Manifest ID 和脱敏 Metadata。

`ModelEvent` 是 tagged union，至少包含：

```text
response_start
text_start / text_delta / text_end
tool_call_start / tool_arguments_delta / tool_call_end
usage
response_end
provider_warning
stream_error
```

合法序列由 Provider 契约测试验证。未知事件可以保留为受限 Provider Metadata，但不得破坏既有序列。

### 26.2 Stop Reason

统一为 `end_turn`、`tool_use`、`max_output`、`content_filter`、`cancelled`、`provider_error`、`unknown`。`max_output` 或断流时若 Tool Call 参数未完整验证，禁止执行该调用。部分文本可作为 `interrupted` Revision 展示，但不得冒充完整回复。

Tool Call 参数按 Provider index/ID 聚合，结束后执行 JSON 语法、Schema 和大小校验。重复 ID、非法顺序和无法映射的 Tool Result 属于协议错误，而不是普通工具错误。

### 26.3 Provider Metadata 和重试

Provider 要求后续回传的 response ID、cache key 等以明确 allowlist 持久化；原始响应仅在用户选择后作为受限 Artifact 保存。部分响应后的重试创建新 Attempt；失败 Attempt 可出现在 Audit/UI Timeline，但不进入下一轮 Model Context，除非 Context Engine 明确选择其安全摘要。

## 27. Canonical Transcript 与会话渲染

必须区分四个视图：

1. **Canonical Transcript**：用户和 Assistant 的逻辑消息、工具调用及结果，是完整会话恢复的稳定语义记录；
2. **Model Context View**：从 Transcript、摘要和证据中按预算选择的模型输入；
3. **UI Timeline**：在 Transcript 基础上增加审批、重试、进度、验证和系统通知；
4. **Audit Timeline**：包含安全、所有权、恢复和不可见内部动作。

### 27.1 顺序和 Revision

`Message` 必须包含 `Sequence`、`Status` 和 Revision；`MessagePart` 必须包含稳定 `PartIndex`，并用真正的 tagged union 表达 Text、ToolCall、ToolResult、ArtifactRef。不得依靠数据库返回顺序或完成时间推断展示顺序。

Assistant Streaming 在内存聚合，并按时间或字节阈值保存 Draft Revision。正常结束后原子提交 Final Revision；崩溃后最后 Draft 以 `interrupted` 展示。重试不会覆盖旧 Revision，被接受的 Attempt 产生新的 Final Revision。

并行 Tool Result 通过 `ToolCallID` 关联；Canonical Transcript 按 Tool Call Index 排列，UI 可显示实际完成时间。审批、澄清和失败 Attempt 默认显示在 UI Timeline，不伪装成模型对话。

### 27.2 大内容与缺失 Artifact

SQLite 永久保存 Tool Result 的安全摘要、退出码、截断信息和 Artifact ID。Artifact 采用懒加载和 range 读取。Artifact 因保留策略被清理或损坏时，历史仍显示摘要及明确占位，不影响 Transcript 结构。

恢复渲染分页读取 Message 和 Part；UI 可展示全量历史，但 Context Engine 不得因此把全量历史重新发送给模型。必须提供契约测试：同一 Session 的在线流、纯事件重放和 Checkpoint + 后续事件重放生成字节级等价的规范化 Transcript JSON。

### 27.3 导出

定义版本化 JSONL 导出格式，至少包含 Session Metadata、Messages/Parts、Tool 摘要、审批和 Artifact Manifest。默认导出不包含秘密和已清理正文；包含 Artifact 的导出需要显式授权。

## 28. SQLite 逻辑模型与事务协议

### 28.1 核心表

至少包含：

| 表 | 性质 | 关键约束 |
|---|---|---|
| `sessions` | 聚合根 Projection | stable ID、version、workspace ID |
| `runs` | Projection | session FK、lifecycle/phase/outcome |
| `events` | 事实日志 | `(session_id, sequence)` unique、EventID unique、schema version |
| `messages` / `message_parts` | Transcript Projection | sequence/part index unique、revision 状态 |
| `model_attempts` | 执行事实 | Request/Attempt ID unique、stop reason |
| `tool_calls` / `tool_executions` | 执行 Projection | ToolCall/Operation ID unique、effect state |
| `approvals` / `interactions` | 安全与交互事实 | request ID、prepared hash、actor、expiry |
| `checkpoints` | 重放优化 | 覆盖的 event sequence 和 schema version |
| `artifacts` / `artifact_refs` | 元数据 | content hash、引用 owner/purpose |
| `workspace_changes` | 归因日志 | operation、old/new hash、路径 |
| `runtime_leases` | 所有权 | run ID unique、owner、fencing token、expiry |
| `usage_records` | 计量 | Provider/Attempt/Token 分类 |

Event 是状态事实来源；Transcript 等 Projection 必须可由 Event 重建。Checkpoint 只优化重放，不是第二事实来源。

### 28.2 Append 事务

一次追加必须：

1. 开启短写事务；
2. 校验 Session aggregate version 和有效 fencing token；
3. 按 Command/Event ID 去重；
4. 分配连续 sequence；
5. 插入 Event；
6. 同事务更新必要 Projection、Artifact 引用和 aggregate version；
7. commit 成功后发布事件。

SQLite 使用本机磁盘、WAL、`foreign_keys=ON`、`busy_timeout` 和受控连接池。禁止把数据库置于 NFS/SMB/云同步目录。写入采用单写者倾向，不逐 Token/逐输出行提交。

### 28.3 Migration、备份和损坏

Migration 版本化、可重复、启动前检查磁盘空间，并在破坏性迁移前使用 SQLite Online Backup API 创建校验过的备份。规定二进制与 Schema 的兼容窗口；迁移失败进入只读诊断模式，不继续执行 Agent。

备份 Manifest 同时记录数据库版本与 Artifact 哈希集合；定期执行 restore test。启动与 `loom doctor` 可运行 integrity check、悬空引用和哈希检查。磁盘满、I/O 错误或数据库损坏时停止新副作用并保留安全诊断路径。

## 29. Artifact Store 协议与数据生命周期

Artifact 写入顺序：创建权限受控临时文件 → 流式脱敏/限制 → 写入并 `fsync` → 校验哈希 → 原子 rename → `fsync` 目录 → SQLite 事务建立引用。数据库提交失败产生可回收孤儿；启动和 GC 扫描孤儿。不得先提交指向不存在文件的引用。

GC 使用 mark-and-sweep、宽限期和 pin/lease，根包括 Message、Event、Checkpoint、审计、活跃 Run、备份。正在写或读的 Artifact 不得删除。读取时验证哈希，损坏对象隔离并在 UI 显示占位。

默认目录权限为 `0700`、文件 `0600`，拒绝不安全 owner 和符号链接。跨 Session 去重仅限同一用户安全域。压缩格式必须限制展开大小，防止压缩炸弹。

### 29.1 数据分类和秘密出口

“完整输出”默认指**经过持久化策略脱敏后的完整输出**。所有数据在进入模型、UI、Artifact、日志、Trace、MCP、崩溃报告前分别经过分类和出口策略。原始敏感数据只在明确授权时进入独立加密、短保留区。

Secret Pipeline 包括结构化字段、已知 Token 前缀、私钥/连接串规则、熵检测和用户规则。`secret.use` 优先传递不可导出的 handle；子进程默认不能访问 Keychain、SSH/GPG Agent、credential helper 或 Loom 数据目录。

### 29.2 保留与删除

每类 Session、Event、Checkpoint、Artifact、审计、备份、日志和遥测定义默认保留期。用户可执行 inspect/export/archive/delete/purge。删除 Session 先写 tombstone，再删除可删除 Projection 和引用；宽限期后 GC 无引用 Artifact；备份按承诺到期。审计仅保留最小脱敏记录。文档和 UI 必须说明 SSD/备份环境下安全擦除的实际边界。

## 30. 副作用提交与崩溃恢复

### 30.1 副作用分类

每个 Tool Definition 声明：

- `read_idempotent`：可安全重放；
- `write_reconcilable`：可通过状态/哈希和 Operation ID 对账；
- `write_compensatable`：有显式、受版本保护的补偿操作；
- `effect_non_idempotent`：无法可靠对账，禁止自动重放；
- `long_running_process`。

执行状态为 `prepared → authorized → intent_committed → dispatched → effect_observed → completed`。崩溃后无法确认结果时进入 `outcome_unknown`，挂起等待专用 Reconciler 或用户，不得标记成功或自动重试。

执行任何副作用前，Runtime 必须持有 lease/fencing token，并先持久化 Intent。文件写通过 old/new hash、Operation ID 和原子写收据对账。Shell/MCP 等非幂等动作保存命令、安全参数、启动身份和可用外部请求 ID；无法对账时明确展示可能已发生的效果。

### 30.2 PreparedCall

PreparedCall 是不可变安全对象，至少绑定：规范化参数和哈希、Tool Definition/实现版本、Capability/Scope/Risk、精确/前缀/未知读写集、幂等分类、可执行文件绝对路径及哈希、cwd、允许环境变量名、stdin 模式、网络和沙箱策略、超时、输出上限、秘密引用、审批文案和过期时间。

审批文案由宿主生成。审批后、获取锁后、Execute 前重新验证路径、符号链接、文件哈希、可执行文件、工作区版本、工具版本、策略版本和 lease；变化则 PreparedCall 失效并重新审批。锁按规范化资源键全序获取，避免死锁。

### 30.3 故障矩阵

实现前建立可执行矩阵，覆盖模型发送/首 Delta/完整响应、Prepare/审批/Intent/Dispatch、文件 write/fsync/rename、Shell 启动/退出、Artifact rename/引用 commit、Event commit、Checkpoint、WAL checkpoint、ENOSPC/EIO/SIGKILL。每个点定义证据、自动动作、可重试性、是否 `outcome_unknown` 和是否需要用户介入。

## 31. Runtime Ownership 与 Daemon

同一 Run 只能有一个 active owner。Owner 获取带单调 fencing token 的租约并心跳续期；租约过期后新 owner 获取更大 token。事件提交、PreparedCall 授权和副作用 dispatch 都必须校验 token，旧 owner 即使仍存活也不得继续执行。

数据库 fencing 只能阻止旧 owner 提交状态，不能原子阻止其在校验后执行文件、进程或远程副作用。因此生产模式采用**单实例 Daemon + 内核排他锁 + 单一副作用执行器**：只有持有数据目录 OS 排他锁的 Daemon 可以 dispatch，CLI/IDE 只能通过 RPC 提交命令。新 Daemon 必须在内核确认旧锁释放且旧执行器进程退出后才能接管；lease 续期或锁所有权失败的 Runtime 必须在下一次外部调用前停止。支持幂等键/fencing 的外部系统继续传递 token；无法被下游 fence 且在 owner 交接窗口中状态不明的动作一律进入 `outcome_unknown`，新 owner 不得自动重放。Direct Mode 仅用于没有 Daemon 的单进程模式，并持有同一数据目录排他锁。

Daemon 使用权限受控的 Unix Domain Socket（Windows 后续使用等价本地机制），进行本地客户端认证和协议版本协商。Direct Mode 与 Daemon Mode 不得同时拥有同一数据目录；通过 OS 排他锁、实例身份和进程身份阻止混用。

RPC Command 带幂等键；事件订阅使用持久化 cursor，断线后先补拉再订阅。慢客户端可以丢弃可重建 Delta 或断开，但不能阻塞 Runtime。客户端断开不等于取消 Run。审批记录 actor/client identity。Daemon 升级前停止接收新 Run、挂起或转交活动任务、Flush Store，并清理无法托管的进程。

## 32. Context Manifest 与可复现性

每次 Model Request 保存轻量 Context Manifest：规则来源及哈希、Message/Part 范围、代码路径哈希/文件哈希/行区间、Artifact 摘要版本、压缩摘要及 lineage、Tokenizer 名称版本、预算桶、裁剪项与原因、最终 Prompt Hash。默认不保存完整 Prompt。

不可信数据携带不可由模型修改的 provenance、trust label 和敏感等级；摘要必须继承来源，不得将外部内容洗白为系统事实。宿主执行跨工具数据流策略：敏感读取流向模型、网络、MCP 或工作区外写入时默认阻断或重新精确审批。

恢复时 Provider、模型、规则、工具或工作区发生漂移，原 PreparedCall 全部失效。允许用户 fork Session 并创建新配置 Revision；不得静默替换原模型后声称可复现。

## 33. 可选 OTel/Langfuse 观测后端

本地 Event Store 是事实来源；Langfuse 不是控制面、恢复源或强依赖。领域事件经 Telemetry Adapter 映射到 OpenTelemetry，Langfuse 通过 OTLP 或可选 Exporter 接入。

映射：Session → Session；Run → Trace；Turn/Context/Tool Batch/Verification → Span；模型调用 → Generation；反馈和 Eval → Score/Experiment。Trace 与 Event 不一一对应，高频 Text Delta 和 Output Chunk 聚合后导出。

远程遥测默认关闭。开启后默认只发送 ID 哈希、模型、Token/费用、延迟、状态、大小和错误码；Prompt、Completion、源码、路径、Patch 和工具正文默认不发送。内容采集需要显式二次选择、脱敏和仓库级策略。

Exporter 异步使用有界队列、批处理、超时和有限重试；队列满时丢弃观测并计数，不阻塞或改变 Run Outcome。失败 Run、安全拒绝、恢复和 Eval 可 100% 采样，普通成功 Run 按比例采样。Telemetry 配置、Schema 和内容策略进入 Session 脱敏快照。

## 34. 威胁模型、沙箱与供应链发布

版本化 Threat Model 必须列出资产、攻击者、信任边界、假设、prevention、detection、recovery、回归测试和残余风险。至少覆盖恶意仓库/依赖、受污染模型、恶意 MCP、本地其他进程、Provider/Collector/更新源和社会工程审批。

Tool Policy 不能约束已经启动的任意子进程，因此执行不可信仓库命令前必须建立 OS 隔离：默认网络拒绝；工作区受控挂载；敏感用户目录、Loom 数据目录、Agent Socket 和秘密不可见；限制 CPU、内存、进程、文件、磁盘和时长；清理继承 FD。Linux 和 macOS 分别维护能力矩阵和不可保证项；初始化失败必须 fail closed。

构建发布要求锁定依赖与校验和、漏洞/许可证扫描、SBOM、构建 provenance、签名和 macOS 公证。更新 Manifest 签名并防回滚，二进制原子替换；数据库 Migration 先备份并符合兼容窗口。MCP 配置记录来源、版本和二进制哈希，变化后重新审批。Stable/Canary 渠道支持快速撤回和高风险能力 kill switch。

## 35. CLI 行为契约与生产 SLO

CLI 的 TTY、非 TTY 和 JSONL 行为必须版本化：stdout 仅承载结果/事件，stderr 承载诊断；`ask` 在没有可信交互通道时转为 suspended 或拒绝，不自动允许；退出码稳定映射 Outcome。除前文命令外，1.0 需提供 transcript/show、cancel、archive/delete/purge、export、artifact inspect 和 daemon status/stop 对应能力。

本地产品仍定义可在 CI、Canary 和自愿遥测中度量的 SLI：Crash-free Run、Session 恢复率、恢复 RTO、已确认事件/Artifact RPO、进程树清理率、Store 一致性、Provider 错误优雅失败率。以下是零容忍安全不变量：

- 危险副作用重复执行率为 0；
- 用户并发修改覆盖率为 0；
- 未授权工作区越界和网络外传率为 0；
- 默认遥测敏感正文泄漏率为 0；
- 不确定副作用误报成功率为 0。

每个 Phase 的 Acceptance Suite 必须绑定 Requirement ID、故障注入点、测试夹具版本、资源上限和不支持能力披露；“达到预设成功率”等不可量化表述必须在发布前由版本化 Eval Policy 给出具体阈值。
