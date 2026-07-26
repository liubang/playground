# Loom TUI 设计

> 状态：Draft  
> 日期：2026-07-23  
> 目标版本：`0.3.0-dev`

## 1. 背景

Loom 已具备真实的模型—工具循环、OpenAI-compatible Provider、权限审批、安全编辑、隔离命令执行、SQLite Event Store、Checkpoint、Session 恢复和 Artifact Store，但当前入口仍是一次性命令：

```text
loom run <prompt>
loom resume <session-id> <prompt>
```

Provider 虽然通过 `ModelStream` 接收增量事件，`agent.Loop` 却在内部聚合完整响应后才把最终消息暴露给 CLI；工具状态只存在于领域事件和日志中；`consoleApprover` 直接读取 stdin；CLI 在任务结束后一次性打印最终答案。因此当前实现无法提供 Claude Code 风格的常驻对话、实时文本、工具折叠、审批面板、可恢复中断和状态栏。

TUI 不只是给 `fmt.Printf` 加颜色。它要求 Runtime 提供稳定、版本化、可背压的实时事件协议，并将输入、审批、取消与渲染从 Agent 状态机中解耦。

## 2. 目标与非目标

### 2.1 目标

第一阶段交付一个可日常使用的终端交互客户端：

1. `loom` 或 `loom chat` 进入常驻会话；
2. 用户可在同一 Session 中连续提交多轮 Prompt；
3. 模型可见文本按 Delta 实时展示，首字渲染 P95 小于 50ms；
4. 工具准备、审批、执行、成功、失败和耗时实时展示；
5. 审批与实际 `PreparedCall`、参数哈希、风险和读写范围严格绑定；
6. 第一次 `Ctrl+C` 只取消当前活动并保留 Session；空闲时 `Ctrl+D` 或 `/exit` 退出；
7. Session 可恢复，已持久化 Transcript 可重建视图；
8. 窄终端、CJK、Emoji、粘贴、多行输入和窗口缩放可用；
9. 非 TTY 环境维持确定性、无 ANSI 的机器可读输出；
10. 慢 UI、关闭 UI 或渲染异常不得阻塞模型流、进程 pipe 排空或持久化。

### 2.2 非目标

第一版不实现：IDE/浏览器远程客户端、多 Session 标签页、鼠标必需交互、私有 Chain-of-Thought、后台 PTY、任意二进制 Artifact 内联预览、主题市场，以及 TUI 直接修改 Runtime 内存或 SQLite 表。

## 3. 设计原则

1. **Runtime 是唯一事实来源**：UI 只发送命令、审批和取消，只消费事件与快照。
2. **持久事件与临时事件分层**：Event Store 保存可恢复事实；文本 Delta、spinner 和 resize 不污染事件日志。
3. **先持久化，再展示不可逆状态**：审批请求和工具执行 intent 落盘后，UI 才显示为可操作或已执行。
4. **慢消费者隔离**：UI 事件通道有界；高频 Delta 合并；关键状态不可静默丢失。
5. **TTY 与非 TTY 同源**：TUI、线性终端和 JSONL Renderer 消费同一 Runtime Event。
6. **安全信息最小化**：审批展示规范化参数、路径、环境变量名和哈希，不显示 Secret 值。
7. **终端只是客户端**：未来 `loom serve` 可复用 Command/Event 协议。
8. **可测试优先**：Reducer、布局和 Renderer 必须能确定性测试。

## 4. 当前实现与差距

可复用能力：`ModelStream` 已提供增量事件；`domain.Event` 已覆盖核心事实；Loop 已在副作用前落盘 intent；`Approver` 可替换；`Run.Suspend/Resume` 已有领域语义；SQLite 可恢复 Transcript；命令输出已使用 Artifact；仓库已有 `x/term`、`go-runewidth` 和 `uniseg`。

主要差距：

1. `aggregateStream` 吞掉模型 Delta；
2. `Loop.Execute` 是封闭同步调用，没有 Observer；
3. `Run.appendEvent` 外部不可订阅；
4. `consoleApprover` 与 TUI 争用 stdin；
5. 顶层单一 Context 将首次中断变成整个 Run 终止；
6. `resume` 不是常驻 Session Controller；
7. Runtime 组装集中在 `cmd/loom/main.go`；
8. 没有版本化前端 Command/Event Envelope。

## 5. 总体架构

```text
┌──────────────────────── Terminal ────────────────────────┐
│ Bubble Tea Model                                         │
│ transcript / composer / approval / status / overlays     │
└───────────────┬──────────────────────▲────────────────────┘
                │ UICommand             │ RuntimeEvent
                ▼                       │
┌───────────────────────────────────────────────────────────┐
│ SessionController                                         │
│ command serialization / turn cancellation / snapshots     │
└───────┬──────────────┬──────────────────────┬──────────────┘
        ▼              ▼                      ▼
  agent.Loop      ChannelApprover        EventBroker
        │              ▲                      ▲
        ▼              │                      │
 Model / Tools / SessionStore / ArtifactStore ┘
```

建议包结构：

```text
internal/app/           Bootstrap、SessionController、UICommand
internal/runtimeevent/  Event Envelope、Broker、Observer
internal/ui/            Bubble Tea Model、Reducer、View、组件
internal/render/        linear 与 jsonl Renderer
```

`app` 不依赖 Bubble Tea；`agent` 不依赖前端包；UI 不直接引用 SQLite 实现。

## 6. Runtime Event 协议

`domain.Event` 是可恢复事实；TUI 还需要文本 Delta、首字延迟和临时进度。因此定义独立的 `RuntimeEvent`：持久事件提交后映射为 Runtime Event；Delta 和进度是 ephemeral Event。

```go
type RuntimeEvent struct {
    Version   int              `json:"version"`
    Sequence  uint64           `json:"sequence"`
    SessionID domain.SessionID `json:"session_id"`
    RunID     domain.RunID     `json:"run_id,omitempty"`
    Turn      int64            `json:"turn,omitempty"`
    Kind      RuntimeEventKind `json:"kind"`
    Time      time.Time        `json:"time"`
    Durable   bool             `json:"durable"`
    Payload   json.RawMessage  `json:"payload,omitempty"`
}
```

`Version` 首版固定为 1；`Sequence` 是 Controller 进程内递增序号，不冒充 Event Store sequence；Payload 使用稳定、脱敏 DTO。

事件集合：

| Kind | Durable | 用途 |
|---|---:|---|
| `session.opened` | 是 | Session、模型、工作区、恢复状态 |
| `turn.started` | 是 | 用户消息已落盘 |
| `run.phase_changed` | 是 | 当前阶段 |
| `model.request_started` | 是 | 模型请求开始 |
| `model.text_delta` | 否 | 可见文本增量 |
| `model.tool_call_delta` | 否 | 工具调用接收进度 |
| `model.response_completed` | 是 | Canonical Message、Usage、StopReason |
| `model.request_failed` | 是 | 稳定错误码 |
| `approval.requested/resolved` | 是 | 审批生命周期 |
| `tool.prepared/started/completed` | 是 | 工具生命周期 |
| `tool.progress` | 否 | 有界进度 |
| `budget.updated` | 是 | Token、费用、预算 |
| `run.cancel_requested` | 否 | soft cancel 请求 |
| `run.cancelled/completed` | 是 | 最终状态 |
| `runtime.warning/fatal` | 否/是 | Runtime 状态 |

Observer：

```go
type Observer interface {
    ObserveEphemeral(RuntimeEvent)
    ObserveDurable(RuntimeEvent) error
}
```

Broker 规则：durable 队列默认 256，写满时取消当前 Turn；文本 Delta 用单槽 coalescer，每 16ms 或 16KiB flush；spinner/progress 可覆盖旧值；stdout/stderr 不进入 Broker；订阅者退出后不得继续等待审批。

提交顺序：

```text
产生领域事实
→ AppendEventsAndCheckpoint 成功
→ 发布 Durable RuntimeEvent
→ UI 更新为“已确认”
```

文本 Delta 可先作为草稿展示；最终持久化 Message 用于校正草稿。流中断时草稿标记 `interrupted`。

## 7. SessionController

Controller 是每个前端会话唯一 Runtime owner，负责创建/恢复 Session、串行处理命令、为每个 Turn 创建独立 Context、运行 Loop、桥接审批、生成快照、取消和关闭。同一 Session 同时最多一个活动 Turn。

首版命令：

| Command | 前置状态 | 行为 |
|---|---|---|
| `submit_prompt` | idle | 持久化用户消息并开始 Turn |
| `cancel_turn` | running/approval | 取消当前 Turn，保持 Session |
| `resolve_approval` | approval | 校验 Approval ID + ArgsHash |
| `new_session` | idle | 创建新 Session |
| `resume_session` | idle | 恢复 Session |
| `request_snapshot` | 任意 | 返回只读投影 |
| `shutdown` | idle 或确认后 | 持久化并退出 |

命令返回 Ack；重复 Command ID 幂等返回原结果。

Controller 状态机：

```text
booting → idle | fatal
idle → running | booting | closed
running → awaiting_approval | cancelling | idle | fatal
awaiting_approval → running | cancelling | fatal
cancelling → idle | fatal
```

Session 与 Turn 必须分离。当前 `Loop.Execute` 将 Context 取消映射为终态 `OutcomeCancelled`；实现 TUI 前需引入 `ExecuteTurn` 或 interrupted/suspended 语义，不能把一次用户取消变成整个 Session 永久终结。

将 `runAgent` 中 Validator、Registry、Sandbox、Artifact Store、Provider、Session Store、Policy 和 Limits 的组装移到 `internal/app.Bootstrap`，供 CLI、TUI、测试和未来 daemon 共享。

## 8. TUI 状态、布局与输入

根 Model 保存 mode、session、transcript、composer、approval、status、overlay、viewport、theme、尺寸和 follow-tail。`Update` 是单线程 Reducer；Runtime goroutine只通过 `Program.Send` 投递消息。

Transcript Block：User、Assistant（streaming/final/interrupted）、Tool（prepared/running/success/error/cancelled）、Notice、Summary。每个 Block 使用稳定 ID 更新。工具默认折叠：

```text
● Read go/pl/loom/internal/domain/interfaces.go
  166 lines · 12ms
```

布局：

```text
┌ Header: Loom · model · session · workspace ───────────────┐
│ Transcript viewport                                       │
├ Optional approval / command overlay ──────────────────────┤
│ Composer (1..8 lines)                                     │
├ Status: phase · tokens · elapsed · sandbox · shortcuts ────┤
```

宽度小于 60 时隐藏次要字段；高度小于 12 时压缩 Header；宽度小于 30 时进入 minimal renderer。所有裁剪按 grapheme/display width。

输入：Enter 提交，Alt+Enter 或 Ctrl+J 换行；bracketed paste 默认限制 1MiB；Prompt 收到 Ack 后清空，失败恢复草稿。默认 follow-tail，用户上滚后暂停并显示新事件数，End/G 回到底部。

首版 Slash Command：`/help`、`/new`、`/resume <id>`、`/sessions`、`/inspect`、`/model`、`/clear`、`/compact`（未实现时明确提示）、`/exit`。模型文本不能触发本地命令。

## 9. 流式渲染

将 `aggregateStream` 拆成可观察的协议聚合器：

```go
type StreamAggregator struct { /* protocol state */ }
func (a *StreamAggregator) Apply(ModelEvent) ([]RuntimeEvent, error)
func (a *StreamAggregator) Finalize() (streamResponse, error)
```

每个合法文本 Delta 同时更新 Canonical 聚合器并发布 ephemeral Event。不能为 UI 绕过协议校验；非法 Delta 仍终止请求，已显示文本标记 interrupted。

Markdown 策略：streaming 时只做轻量样式；final 后完整渲染；代码块保留文本并横向裁剪；剥离控制字符和未经允许的 ANSI；链接只显示不自动打开。首版不强制引入 Glamour。

## 10. 审批交互与安全

`ChannelApprover` 将请求交给 Controller 并等待 Reply：

1. Loop 产生并持久化 `permission.requested`；
2. Controller 发布 `approval.requested`；
3. TUI 显示 Overlay；
4. UI 返回 `approval_id + call_id + args_hash + decision`；
5. Controller 重新校验绑定关系；
6. 回复 Approver；
7. Loop 持久化 resolution 后发布结果。

Overlay 至少展示风险、能力、工具、描述、Program/Args 或目标路径、读写范围、网络策略、环境变量名、ArgsHash 短前缀和恢复属性。禁止显示 Secret 值。

按键：`y` 仅允许本次；`n`/Esc 拒绝；Ctrl+C 拒绝并取消 Turn。UI 崩溃、失焦超时或 Context 取消默认拒绝。首版不提供模糊的“始终允许所有命令”。

## 11. 中断、退出与恢复

Context 分层：

```text
processCtx    应用生命周期，SIGTERM/强制退出取消
sessionCtx    当前 TUI Session，/new 或 /exit 取消
turnCtx       单次 Prompt，第一次 Ctrl+C 取消
operationCtx  Model/Tool 自身超时
```

Turn cancel 不能取消最后一次持久化；终止提交使用 `context.WithoutCancel` 加 5 秒硬超时。

Ctrl+C 语义：

| 状态 | 第一次 Ctrl+C | 2 秒内第二次 |
|---|---|---|
| idle 且输入非空 | 清空输入 | 退出确认 |
| idle 且输入为空 | 显示再次退出提示 | 退出 |
| model/tool running | cancel Turn | 强制退出进程，保留已提交状态 |
| awaiting approval | deny + cancel Turn | 强制退出 |
| cancelling | 提示正在回收 | 强制退出 |

`SIGTERM` 不弹交互确认，取消 Turn、拒绝审批、尝试 5 秒持久化后退出。`Ctrl+Z` 交给 Bubble Tea 正确恢复终端模式。

恢复 TUI 时：先读一致性 Snapshot，渲染持久 Transcript，再订阅新事件；草稿 Delta 不恢复；未完成模型请求显示 interrupted；已记录 intent 但结果未知的副作用显示“需要人工核实”，绝不自动重放；旧审批失效。

## 12. 非 TTY 与降级

模式选择：

```text
显式 --json       → JSONL Renderer
显式 --no-tui     → Linear Renderer
stdin/stdout 为 TTY → TUI
否则              → JSONL（run）或报错（交互 chat）
TERM=dumb/NO_COLOR → 无颜色 Linear/Minimal
```

JSONL 每行一个 `RuntimeEvent v1`，stdout 只写协议，日志写 stderr；无 ANSI、无 spinner、无交互审批。风险操作在无可用审批输入时默认拒绝。TUI panic 时必须恢复终端 raw mode，再输出安全错误。

## 13. 依赖策略

推荐使用稳定版 Bubble Tea v1 + Bubbles + Lip Gloss：

- Bubble Tea：事件循环、终端生命周期和 resize；
- Bubbles：textarea、viewport、spinner；
- Lip Gloss：样式和布局；
- 首版暂不引入 Glamour，减少 Markdown 流式重排与依赖成本。

仓库已有 `x/term`、`runewidth`、`uniseg`。新增 Charm 依赖通过 `go/go.mod`、Gazelle/go_deps 和 `MODULE.bazel use_repo` 管理。Bubble Tea 仅存在于 `internal/ui`，Runtime 和 Renderer 不依赖它。

如果 Bazel 引入阻塞，可先用现有 `x/term` 实现 Linear REPL 验证 Runtime Event 协议，但最终全屏 TUI 不自行重造终端 raw mode、resize 和输入法处理。

## 14. 并发、所有权与资源清理

- Bubble Tea goroutine：唯一 UI Model owner；
- Controller goroutine：唯一 Session/Loop owner；
- 每个 Turn 最多一个 Loop goroutine；
- Approver 通过按 Approval ID 索引的单次 Reply channel；
- Broker 是唯一事件序号分配者；
- Tool 内部并发不直接写 UI；
- 所有 channel 都有明确 owner 和关闭方；
- `Program.Send` 在 UI 退出后必须停止；
- Store、Artifact Store、ModelStream、Broker、Controller 按逆序关闭；
- 工具取消沿用现有进程组回收边界。

关闭顺序：停止接收 Prompt → 取消 Turn → 拒绝 Pending Approval → 等待工具回收 → 持久化 → 关闭 Broker → 关闭 Store → 恢复终端。

## 15. 错误与可观测性

错误分三层：

1. **用户可恢复**：工具失败、审批拒绝、模型限流；显示 Block，Session 继续；
2. **Turn 终止**：模型协议错误、预算耗尽、用户取消；回到 idle；
3. **Runtime fatal**：持久化一致性失败、Broker 丢 Durable Event、Store 损坏；锁定输入并要求退出/诊断。

日志不能写到 TUI stdout。使用独立 `slog` 文件或 stderr（Linear/JSONL 下），字段包含 session、turn、event sequence、component、duration 和 error code，不记录 Prompt、代码、Secret 或完整工具输出。

建议指标：启动到可输入、首字延迟、Delta 到渲染延迟、Tool 卡片延迟、审批等待、丢弃/合并 Delta 数、Resize 重绘、内存、取消完成耗时和终端恢复失败数。

## 16. 测试设计

### 16.1 单元测试

- RuntimeEvent JSON 兼容、未知 Kind、脱敏；
- Broker sequence、Delta 合并、durable overflow、关闭竞态；
- StreamAggregator 文本/工具参数分片、断流和 final 校正；
- Controller 命令幂等、状态转换、并发 Prompt 拒绝；
- ChannelApprover 绑定校验、超时、UI 退出默认拒绝；
- Reducer 对所有 Event 的确定性状态；
- CJK/Emoji/组合字符宽度、窄屏和 resize；
- Composer 粘贴限制、历史、提交失败恢复；
- ANSI/control character 清理；
- Ctrl+C 状态表。

### 16.2 集成测试

使用 Fake Model/Tool/Store 和无真实终端的 Bubble Tea Program：

1. Prompt → 文本 Delta → final；
2. Prompt → Tool Call → approval allow → Tool Result → final；
3. approval deny 后 Tool Error 回模型；
4. 模型断流，草稿 interrupted，Session 可继续；
5. 命令执行中 cancel，进程组回收后回 idle；
6. UI 慢消费不阻塞工具输出；
7. SQLite commit 失败不展示“已执行”；
8. TUI 退出后恢复同一 Session；
9. JSONL 输出与 TUI 消费同一事件序列；
10. panic/强退后终端属性恢复。

### 16.3 PTY/E2E

在伪终端运行真实二进制，覆盖 resize、bracketed paste、Ctrl+C/Ctrl+D、审批按键、无颜色、`TERM=dumb`、stdout 重定向和真实 Provider 冒烟。快照测试固定终端尺寸、主题和 Clock，避免把 spinner 时间写入 golden。

### 16.4 安全测试

- 模型输出 ANSI/OSC 8/OSC 52/终端标题注入；
- 审批描述包含控制字符；
- Secret 不进入 UI Event/JSONL/日志；
- 篡改 Approval ID、Call ID、ArgsHash；
- 非 TTY 风险操作默认拒绝；
- UI disconnect 时 Pending Approval 不悬挂；
- 超长 Delta、超长单词和 1MiB+ paste 不导致无界内存。

## 17. 分阶段实施

### M0：Runtime Event 基座

- 定义 `RuntimeEvent v1`、Observer 和 Broker；
- 提交后发布 Durable Event；
- StreamAggregator 暴露文本 Delta；
- 增加 Linear/JSONL Renderer；
- 保持现有 `loom run` 行为兼容。

验收：真实 Provider 首字可实时输出；慢 Renderer 不阻塞；全量测试通过。

### M1：SessionController 与常驻 REPL

- 抽取 Bootstrap；
- 引入 Session/Turn Context 分层；
- 实现 `submit_prompt`、`cancel_turn`、`shutdown`；
- 实现 ChannelApprover；
- 先用 Linear REPL 验证多轮和取消。

验收：同一 Session 连续三轮；取消单轮后可继续；审批不争用 stdin。

### M2：最小 Bubble Tea TUI

- 引入 Bubble Tea、Bubbles、Lip Gloss；
- Transcript、Composer、Status、Tool Block；
- resize、follow-tail、minimal layout；
- `/help`、`/new`、`/exit`。

验收：真实只读和编辑任务可完整运行；CJK/窄屏可用；退出恢复终端。

### M3：审批、恢复和 Session UX

- Approval Overlay；
- Session Picker、`/resume`、Snapshot 恢复；
- interrupted/unknown side-effect 展示；
- `/inspect`、预算和沙箱状态。

验收：审批绑定安全测试、崩溃恢复和未知副作用测试通过。

### M4：体验与性能收口

- final Markdown 渲染；
- Tool Block 展开、Diff 预览、搜索和历史；
- 虚拟化 Transcript；
- 主题、NO_COLOR、可访问性；
- 性能 Profile 和 E2E 稳定性。

验收：10k Block Session 内存受控；Delta P95 小于 50ms；无终端污染。

## 18. 兼容与迁移

- 保留 `loom run <prompt>` 和 `loom resume <id> <prompt>`；
- `loom` 无参数且 TTY 时进入 TUI；非 TTY 时打印 usage 或要求显式 `--json`；
- `loom chat [--resume id]` 是显式交互入口；
- Event Store schema 不因 ephemeral Event 改动；若新增 Turn 状态字段，使用下一版 SQLite migration；
- RuntimeEvent v1 与远程 RPC 解耦，未来可封装进 JSON-RPC notification；
- 旧 Session 没有 UI 元数据也必须可恢复。

## 19. 验收标准

功能：常驻多轮、实时 Delta、工具状态、审批、单 Turn 取消、恢复、Slash Command、非 TTY JSONL 全部可用。

安全：UI 不绕过 Policy；审批绑定哈希；无 TTY 默认拒绝；控制字符不执行；Secret 不进入显示或日志；不可确认副作用不自动重放。

可靠性：慢 UI 不阻塞 Runtime；持久状态只在 commit 后确认；Ctrl+C 后进程组回收；所有退出路径恢复终端；UI panic 不损坏 Session。

性能：启动到可输入 P95 `<300ms`（不含首次迁移），Delta 到显示 P95 `<50ms`，空闲 Runtime `<100MiB`，10k Transcript Block 滚动无明显卡顿。

## 20. 关键决策

1. 使用 Bubble Tea v1，而不是自研终端事件循环；
2. Runtime Event 与持久 Domain Event 分离；
3. 先完成 M0/M1 的事件和 Controller，再写全屏 UI；
4. Session 与 Turn Context 分离，Ctrl+C 默认取消 Turn；
5. Durable Event 不允许无声丢弃，高频 Delta 允许合并；
6. 审批由 ChannelApprover 桥接，TUI 不接触 Tool Execute；
7. 首版不强制引入 Glamour；
8. TUI、Linear、JSONL 共用协议与 Controller；
9. 无 TTY、UI disconnect 和校验失败全部 fail closed；
10. TUI 完成后再继续 Session export/delete 等 Phase 3 功能。

## 21. 开放问题

实现前需要在代码评审中最终确认：

- 一次 Turn 取消使用 `LifecycleSuspended`，还是新增显式 `turn.interrupted` 领域事件；
- RuntimeEvent DTO 放在 `domain` 还是独立 `runtimeevent` 包；本设计推荐独立包；
- Bubble Tea 采用 Alternate Screen 还是 inline mode；建议默认 inline-compatible，长 Transcript 不因退出消失，并提供 `--alt-screen`；
- Session Picker 是否首版分页；建议复用 `ListSessions`，默认最近 100 条；
- Markdown final Renderer 使用轻量自研 AST 还是后续引入 Glamour；
- `tool.progress` 是否在首版只支持阶段进度，不承载命令原始输出；建议是；
- 强制退出的第二次 Ctrl+C 窗口采用 2 秒还是可配置值。
