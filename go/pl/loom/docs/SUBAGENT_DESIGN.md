# Loom Subagent 设计（委托执行）

| 项目 | 内容 |
|------|------|
| 状态 | **已实现**（V1 同步版与 V2 异步版均已落地：`delegate_task` 同步/异步、`wait_subagent`、`resume_subagent`、`subagent.*` 配置；原 Draft v1 中"V2 异步版为演进方向"已兑现） |
| 日期 | 2026-07-31 |
| 关联文档 | `DESIGN.md`（§15 扩展与前端协议、§22 关键取舍）、`CONTEXT_DESIGN.md`、`PERMISSION_DESIGN.md`、`STEER_DESIGN.md` |
| 目标读者 | loom 运行时与前端贡献者 |

---

## 1. 背景与目标

`DESIGN.md` §15 早已把子 Agent 写进蓝图：

> 子 Agent 作为 `delegate_task` 工具：独立上下文和预算、默认只读、最大递归深度 1、只返回结构化结论和证据，主 Agent 负责验证及修改。适用于大型仓库并行探索、资料研究和独立 Review。

领域层也已预埋了两个枚举值（`domain.CapAgentDelegate`、`domain.ToolSourceSubAgent`），但一直未实现。本设计将其落地，目标：

1. **上下文隔离的探索能力**：主 Agent 可以把"在大型仓库里找 X""研究 Y 的资料"这类高 token、低产出的探索任务委托给子 Agent，子 Agent 在**独立上下文**里跑完整的 agent loop，只把结构化结论带回主 transcript——主上下文不被几十次 search/read 的中间产物污染（与 `CONTEXT_DESIGN.md` 的压缩策略互补：压缩是事后补救，委托是事前隔离）；
2. **预算可控**：子 Agent 有独立且收紧的 runaway cap，其 token 消耗折算回父 run 预算，委托不是预算漏洞；
3. **默认只读、零新交互**：子 Agent 只持有 R0/R1 只读工具，永不触发审批、永不提问（`AutonomousQuestioner`），父 turn 阻塞等待期间 Controller 的单 turn 不变量不受任何冲击；
4. **可审计、可恢复**：子 Agent 是独立的持久化 session（事件溯源 + checkpoint），与父 session 通过委托边关联，崩溃恢复语义明确；
5. **不改变既有单 turn 模型**：任何时刻仍然只有一个 active turn；委托是 turn 内的一次工具调用，不是并发 turn。

### 1.1 非目标（本期不做）

- **异步 spawn/wait 模型**（codex V2 路线：spawn 立即返回、邮箱通信、父子并发）：需要重写 Controller 并发模型与审批桥，见 §8 演进路径——**已于 2026-08 实现**（`delegate_task async=true`、`wait_subagent`、`resume_subagent`）；
- **可写子 Agent**：写操作、审批桥接留待后续；子 Agent 的写需求由主 Agent 消化结论后自行执行（主 Agent 负责验证及修改）；
- **递归委托**：子 Agent 的工具集不含 `delegate_task`，深度恒为 1；
- **fork 父上下文**：`fork_turns` 语义预留（§8），V1 子 Agent 一律全新上下文；
- **TUI 嵌套流式渲染**（codex 式子活动内联折叠块）：V1 的可观测性走「状态行 + 只读 overlay」的拉取模型，见 §10。

---

## 2. 参照：Codex CLI 的 multi-agent 机制

对 `/Users/liubang/workspace/github/codex`（codex-rs）的调研结论。codex 存在 V1/V2 两代并存实现，核心是 **Thread 即 Agent**：每个子 agent 是独立的 `CodexThread`（独立 thread_id、session、rollout、token 计量、状态机），与父共享 `ThreadManagerState`。

### 2.1 值得照抄的语义决策（与架构无关）

| codex 做法 | loom 采纳情况 |
|-----------|---------------|
| spawn/wait 分离的工具语义（spawn 返回 agent 引用而非最终文本） | **语义预留**：`delegate_task` 结果 metadata 携带 `child_session_id` 引用，V2 演进不改工具语义 |
| fork 父历史时过滤（保留 system/user/FinalAnswer，丢弃工具中间产物；`fork_turns="all"\|N`） | V2 候选，V1 不 fork |
| 角色 = 配置叠加层（`apply_role_to_config`，不改代码路径） | V2 候选；V1 只有内置只读研究者一种形态 |
| 并发槽位 + RAII 预留（`SpawnReservation` reserve-then-commit，Drop 自动释放） | 并行 delegate 时照抄 |
| `Interrupted` 不是终态；`NotFound`/`Died` 视为已终止 | V2 候选（异步才有中断语义） |
| 拓扑持久化：一条 `(parent, child, status)` 边支撑整棵树恢复 | **采纳**：子 session 创建事件记录 `parent_session_id + parent_call_id`，等价于 codex 的 spawn edge |
| token/时长按 `parent_id` 归因的遥测维度 | **采纳**：trace RunMeta 携带父 session 引用 |

### 2.2 明确不抄的架构形态

| codex 做法 | 不抄的原因 |
|-----------|-----------|
| 异步多线程运行时（spawn 立即返回、邮箱、`wait_agent`） | loom Controller 的核心不变量是"一个 session 同一时刻只有一个 active turn"，审批是单通道状态机；异步子 agent 在父 turn 阻塞期间触发审批会死锁。支持它等于重写 Controller 并发模型——为不存在的需求付复杂度税 |
| 子 agent 继承父全部工具集（含 spawn 自身） | codex 靠按 thread 继承审批/sandbox 兜底；loom 的 capability/risk 权限体系更细，`DESIGN.md` 明确选择默认只读 + 深度 1 |
| V2 取消递归深度限制（`AgentPath` 寻址） | 放大预算失控面，对 loom 无收益 |
| 角色可覆盖审批策略 | 会击穿 loom permission 层的信任假设 |

### 2.3 codex 自身的不确定性

codex 的 multi-agent 仍在快速演进：V1/V2 并存、`multiAgentMode` 已标记 deprecated（被 `effort: ultra` 取代）、hooks 持续新增。照抄移动靶风险高；本设计只取其**已被两代实现共同验证**的语义（上下文隔离、独立持久化、拓扑边、用量归因）。

---

## 3. 总体设计

### 3.1 一句话

**子 Agent 就是一个 `domain.Tool`（`delegate_task`），它的 `Execute` 内部构造并同步运行一个受限的 `agent.Loop`**；父 turn 在工具执行点阻塞等待子 run 到达终态，取最终结论文本作为工具结果。

### 3.2 架构图

```
父 turn (Controller 唯一 active turn)
  └─ agent.Loop.Execute
       └─ executeTools
            └─ delegate_task.Execute(ctx, prepared)          ← 父 turn 在此阻塞
                 ├─ CreateSession(childSessionID)             ← 独立 session（事件溯源）
                 ├─ NewRun(childSessionID, childLimits)       ← 收紧的 runaway cap
                 ├─ agent.Loop{Registry: 只读子集,            ← 无 delegate_task → 深度恒 1
                 │             Approver:  deny-all,           ← 安全网（只读集本就不会 ask）
                 │             Store:     裸 store,           ← 持久化但不发 runtime 事件
                 │             Recorder:  共享 recorder}      ← trace 嵌套为子节点
                 │    .Execute(ctx)                           ← ctx 派生自 turnCtx，取消天然传播
                 └─ ToolResult{Content: 子 run 结论文本,
                               Metadata: {child_session_id, usage, outcome}}
```

### 3.3 关键决策与理由

| # | 决策 | 理由 |
|---|------|------|
| D1 | 同步阻塞，不引入异步 spawn/wait | 完美契合单 turn Controller：审批、取消、预算、持久化全部走现有路径，零并发模型改动。codex 调研结论是其异步模型耦合于 ThreadManager 多前端运行时，非 subagent 问题的内在要求 |
| D2 | 子 run 用**独立 SessionID** 持久化 | 事件流/checkpoint/崩溃恢复全部复用现有机制；同一 session 写会导致 event log 序列冲突。关联通过创建事件的 `parent_session_id + parent_call_id`（等价 codex 的 spawn edge） |
| D3 | 子 registry 预构建为**只读工具子集**，不含 `delegate_task` | 一举三得：默认只读落地；递归深度恒 1（无需 depth 计数器）；R0/R1 在 DefaultPolicy 下永不 ask，子 Agent 完全自主，无需审批桥接 |
| D4 | 子 limits 从配置派生并收紧；子 usage 折算回父 run | 委托不能成为预算漏洞（§5） |
| D5 | 工具不声明 `CapAgentDelegate`，以 `Source: ToolSourceSubAgent` 为审计标记；Prepare 上报 `Risk: R1` | 最初设计是声明 capability（R4）但上报 R1，实现时被 loop 的执行期漂移校验否决——`validatePreparedExecution` 规定 prepared risk 只能高于、不能低于定义的静态层级（"降层会削弱审批时所依据的策略"）。R1 的语义依据：子 Agent 只读，副作用仅为新建 session 记录 + token 消耗（可计量、有上限）。收益：崩溃恢复走"未启动/只读调用直接关闭"路径（`RecoverRun` 对 R≤1 不做 reconcile），规避 R4 的 "uncertain non-idempotent outcome" 阻塞；空 capability 默认 R0 → R1 属于合法升层（同 run_cmd 的 R2→R3） |
| D6 | 子 Loop 用裸 store（不经 publishingStore） | publishingStore 绑定父 runID 且向 Controller 投影状态；子 run 事件只持久化不发布。父 UI 上 delegate 就是一次普通工具调用 |
| D7 | 模型解析用 atomic 注入当前 turn 的 provider/model | 工具实例 bootstrap 期注册、跨 turn 共享；模型选择是 per-turn 状态。复刻 `AtomicSessionEnv` 的"外部写、执行读"模式 |

---

## 4. 工具定义与执行语义

### 4.1 `delegate_task`

```json
{
  "name": "delegate_task",
  "description": "Delegate a self-contained read-only research task to a sub-agent that explores the workspace in its own isolated context and returns a structured conclusion with evidence. Use for large-codebase exploration, fact gathering, or independent review. The sub-agent cannot modify files, ask questions, or delegate further.",
  "input_schema": {
    "type": "object",
    "properties": {
      "task":  { "type": "string", "description": "Complete, self-contained task description. The sub-agent sees no conversation history; include all necessary context." },
      "focus": { "type": "array", "items": { "type": "string" }, "description": "Optional paths or symbols to prioritize." }
    },
    "required": ["task"]
  }
}
```

- `Source: ToolSourceSubAgent`（审计标记，不声明 capability，见 D5），Prepare 上报 `Risk: R1`；
- 结果 content：子 run 的最终结论文本（超长走既有 preview 截断；子 run 内部的大输出已由 ArtifactStore 外部化）；
- 结果 metadata：`child_session_id`、`child_outcome`、`child_input_tokens`、`child_output_tokens`、`child_turns`；
- 子 run 失败/预算耗尽：工具返回 `ToolStatusError`（`Retryable: false`），error message 携带子 outcome 与 `child_session_id` 供人工审计，父 Agent 自行决定降级策略（自己查或换个问法）。

### 4.2 子 Agent 工具集（只读子集）

`read_file`、`list_dir`、`grep`、`glob`、`view_image`、`present_image`、`git_status`、`git_diff`、`git_log`、`git_merge_base`、`git_blame`、`web_fetch`、`web_search`。
明确排除：`edit`/`write`（写）、`run_cmd`（进程执行）、`lint`（进程执行）、`update_goal`/`update_plan`（父 run 状态）、`ask_user`（无交互对象）、`delegate_task`（递归）。

### 4.3 子 Agent 系统提示

复用 `prompt.NewBuilder`（同一 workspace 上下文与规则发现），追加子 Agent 专属指令：只读研究者身份、输出契约（结论 + 证据路径 + 置信度 + 未尽事项）、禁止尝试修改、预算意识。不加载 skills 目录、不加载 managed prompt。

### 4.4 取消传播

工具的 `Execute(ctx)` 拿到的 ctx 派生自 turnCtx：父 turn 取消 → 子 loop `Execute` 退出 → 子 run 落 cancelled 终态 + terminal checkpoint。无需新机制。

---

## 5. 预算与用量

### 5.1 子 run 预算（收紧的 runaway cap）

子 limits 以父 limits 为底，仅 `max_tokens` 维度允许配置收紧（§7）；其余维度（cost、tool output、artifact）与父一致。注意 loom 的预算哲学（`CONTEXT_DESIGN.md` §4.4.3）只有稀缺资源（session 累计 token、成本）是预算维度，**没有 turn 数上限**——失控行为由 runaway 检测（重复调用、连续失败、停滞）按行为捕获，子 run 原样复用同一套 `RunawayConfig`，budget 检查、wrap-up soft landing 零改动。子 Agent 的消耗同时折算回父 run（§5.2），双重约束下委托不是预算漏洞。

### 5.2 用量折算回父 run

子 run 结束后，其 `Usage`（input/output tokens、tool calls、cost）通过 `ToolResult.Metadata` 带回；父 loop 在记录工具结果时检测该 metadata 并累加进父 `Run.Usage`（新增 `Run.AddExternalUsage`），随后走既有 `EventBudgetUpdated` 持久化与通知路径。语义：子 Agent 花的 token 就是父 prompt 花的 token——委托在预算维度上透明。

### 5.3 上下文窗口

子 run 的 `WindowModel` 由 factory 按同一模型元数据与 `context.*` 配置重建（与 `runTurn` 同一推导），子 run 内部自动获得压缩能力。

---

## 6. 持久化与崩溃恢复

### 6.1 委托边

子 session 创建后，子 run 的首个事件（`EventRunCreated` payload）携带 `parent_session_id` 与 `parent_call_id`。等价于 codex `agent-graph-store` 的 `(parent, child, status)` 边，但落在既有事件流里，无需新存储。

### 6.2 恢复语义

- **父 run 崩溃恢复**：`delegate_task` 上报 R1（D5），`RecoverRun` 对"已 started 未 completed"的 R≤1 调用直接关闭为"未执行、未重放"工具错误——恢复不被阻塞。父 Agent 恢复后可重新委托；
- **子 session 自身**：是完整的事件溯源 session，`loom resume <child_session_id>` 可独立恢复审计（主要用于人工排查，不自动续跑）；
- **不自动 reconcile 子 run 结论**：V1 不做"父恢复后找回已完成子 run 的结论"（codex 也未做等价物——其恢复是重建整棵树重跑）。留作开放问题（§9）。

---

## 7. 配置

新增 `subagent.*` 配置节（`CONFIG_DESIGN.md` 优先级链不变）：

```yaml
subagent:
  enabled: true            # 总开关；false 则不注册 delegate_task
  max_tokens: 0            # 子 run 的累计 token 上限；0 = 继承 limits.max_tokens
  max_output_tokens: 8192  # 子 Agent 单次响应的输出上限；0 = 继承 limits.max_output_tokens
  model: ""                # 可选 "provider/model"；空 = 跟随当前 turn 模型
```

`enabled=false` 是硬开关：工具不进 registry，模型无从调用。`model` 允许把子 Agent 钉在更便宜的模型上（探索任务的经典用法）；钉住时子 Agent 用该模型自己配置的 reasoning，主 Agent 的会话级 `/reasoning` 覆盖不外溢。`max_output_tokens` 默认 8192 是效果与延迟的权衡（含 reasoning 余量）：子 Agent 的结论应当精炼，而截断只能在流完整个输出预算后才暴露——上限同时是耗时上限（§12）。

---

## 8. 演进路径（V2 异步版，不在本期）

触发条件：出现"主 Agent 等待子 Agent 时还需继续本地工作"或"并行探索"的强需求。

1. **Controller 多 turn 化**（前置大工程）：审批桥多路复用、turn 间状态隔离；
2. **spawn/wait 分离**：`delegate_task` 拆为 `spawn_agent`（返回引用，立即返回）+ `wait_agent`；语义已在 V1 的 metadata 引用中预留；
3. **并发槽位**：`AgentRegistry` + RAII reservation（照抄 codex）；
4. **fork_turns**：fork 过滤规则照抄 codex（保留 system/user/FinalAnswer，丢弃工具中间产物）；
5. **角色系统**：`apply_role_to_config` 式配置叠加层（explorer/worker/reviewer）；
6. **TUI 嵌套渲染**：子 session 事件经 broker 发布，前端聚合为可折叠块（codex 的"折叠成一行 + picker 切换"策略）；
7. **可写子 Agent**：审批桥接 + 角色化权限叠加。

---

## 9. 开放问题

1. 父 run 崩溃恢复后，是否应通过委托边找回已完成子 run 的结论（避免重复委托烧钱）？需要 `RecoverySpec` 扩展 `subagent_run` kind + 子 session 终态查询；
2. 子 Agent 的 trace 与父 trace 在 Langfuse 侧的关联展示（当前靠 RunMeta 的 parent 引用，是否需要一等字段）；
3. 并行 delegate 时只读工具的并发安全审计（`web_fetch` artifact staging、`grep` runner 复用）。

---

## 10. TUI 交互（V1.1 已实现）

子 loop 仍然只写裸 store（D6 不反转）；可观测性由「observer 桥 + 拉取模型」在 loop 外补足，三层结构：

### 10.1 生命周期桥（推）

`subagent.Factory.Observer` 在子 run 启动/终态时同步回调（钩子在 delegate 工具 Execute 内触发，要求 cheap + non-blocking）。`app.WireSubagentObserver` 把它接到 broker，以**父 session 信封**发三个 ephemeral 事件（前端现有订阅过滤器零改动）：

| 事件 | 时机 | 内容 |
|------|------|------|
| `subagent.started` | 子 session 创建后 | `call_id`（绑定 delegate 工具块）、`child_session_id`（drill-in 目标）、`task` |
| `subagent.progress` | 每 1s（桥自己的 ticker goroutine） | 从子 checkpoint **拉取**的计数器：tool calls、in/out tokens、elapsed |
| `subagent.finished` | 子 run 终态 | outcome + 最终计数器 |

进度是拉取而非子 loop 推送：子 checkpoint 每个工具批次 flush 一次，准实时足够，子 loop 与事件流零耦合。

### 10.2 delegate 工具块状态行

`subagent.*` 事件经 `ApplyRuntimeEvent` 投影到 delegate 工具块的 `SubagentBlockState`：进行中显示 `↳ exploring… 5 calls · 12k tok · Ctrl+G to watch`，终态显示 `↳ 7 calls · 31k tok · succeeded · Ctrl+G to view`。

### 10.3 只读 drill-in overlay（拉）

`Ctrl+G` 打开全屏只读 overlay（`ModeSubagent`）：目标选择「进行中的委托优先，否则最近一次」。内容 = `Controller.SubagentView(childID)` 直读子 checkpoint（messages + usage + outcome），用主 transcript 同一套 block 渲染器渲染；进行中每秒自动刷新并跟随 tail；`↑↓/PgUp/PgDn` 滚动，`Esc/q` 关闭，`Ctrl+C` 保持全局取消语义。overlay 无输入路径，只读天然成立；历史委托随时可回看（子 session 永久持久化）。

### 10.4 与 codex 式的差异（有意为之）

不做子活动内联流式渲染：V1 父 turn 阻塞期间主 transcript 没有需要避让的新内容，全屏 overlay 信息密度更高，且不需要反转 D6。V2 异步化后 overlay 主入口保留，再加 agent picker 做并发切换（演进非返工）。

---

## 11. 并行工具执行（V1.2 已实现）

模型一个回合发出多个 `delegate_task`（或多个只读调用）时，`executeTools` 不再一律串行，而是**按安全性分段并行**：

### 11.1 机制

1. **opt-in 接口**：`domain.ConcurrentSafely`（`ConcurrentSafe() bool`）是工具实现的显式声明——共享状态必须只有 mutex 保护的基础设施（file-state book、artifact store、response cache），副作用必须限于本调用。已接入：全部只读工具（read_file/list_dir/grep/glob/view_image/present_image/git_*/web_fetch/web_search/read_skill）与 `delegate_task`（每次委托都是全新隔离 session）。未接入（默认串行）：edit/write/run_cmd/lint/update_goal/update_plan/ask_user/browser；
2. **分段**：一个批次的调用按原始顺序切成「连续安全调用」的最大段。段内并行，段间严格串行——写操作永远不会和读操作重叠，顺序语义不受影响；
3. **持久化不变量不变**：并行段的**全部** `tool.execution_started` 事件在任何副作用发生前一次性 flush——崩溃恢复对每个 started-but-uncompleted 调用的 reconcile 证据与串行路径完全一致；
4. **投影保持单线程**：只有 `tool.Execute` 并发；结果收集后**按调用顺序**串行记录（RecordToolResult / trace / 用量折算 / runaway 计数），transcript、事件序列、预算记账全部确定；
5. **限流**：段内并发上限 4（`maxConcurrentToolExecs`）——子 Agent 是完整的模型循环，无限扇出会放大 provider 压力。

### 11.2 对子 Agent 的意义

多个委托真正并发跑：每个子 session 独立持久化、各有 observer ticker 与进度行（桥按 callID 分轨），overlay 点击各自钻取。上下文隔离在并行下不变（e2e 线上字节级验证）。

### 11.3 并发安全审计记录

| 共享设施 | 结论 |
|----------|------|
| `workspace.FileStateBook` | mutex 保护 |
| `artifact.Store` | 每次调用私有 staging 文件 |
| `webfetch.responseCache` | mutex 保护 |
| `session.SQLiteStore` | 单连接串行写 + 每 session 乐观版本 |
| `fakes.FakeStore` / `FakeModel` | mutex 保护 |
| `process.Runner` | 每次调用独立进程；输出缓冲 mutex 保护 |
| trace Recorder（otel） | 线程安全；`recordTool` 只在串行段调用 |

---

## 12. 输出截断的软着陆与失败指引（V1.3 已实现）

源自一次线上复盘：三个并行子 Agent 全部死于 `response incomplete: max_output_tokens`——responses API 把输出截断上报为流错误，整轮探索成果（几十次工具调用 + 3.5 分钟生成）归零，父 Agent 只拿到一句 provider 报错。三层修复：

### 12.1 Provider：输出截断不是流错误

openai responses API 的 `response.incomplete`（reason=max_output_tokens 等）不再产生 stream error：缓冲文本保留、usage 照常入账、流以 `StopMaxOutput` 结束，交给 loop 决策。其他 incomplete 原因（content_filter 等）维持错误路径。chat-completions 的 `length` 与 Anthropic 的 `max_tokens` 原本就映射 `StopMaxOutput`，行为对齐。

### 12.2 Loop：连续截断触发补救 turn（max_output wrap-up）

`StopMaxOutput` 第一次放行继续（部分文本留在 transcript，下一轮接着写）；**连续第二次**进入新的 `max_output` wrap-up 维度（复用 budget soft-landing 状态机）：注入「立即基于已有信息简要收尾、禁止工具」的补救指令，tools-denied 一轮后以 `OutcomeCompletedUnverified` 着陆。补救 turn 自身再撞线则直接以现有文本收尾，不会无限循环。对 delegate_task 而言 `completed_unverified` 走**成功**路径——父 Agent 拿到的是抢救出的结论，不是报错。审计：`budget.wrapup_started{dimension:max_output}` 事件入流，崩溃恢复从事件重新武装。

### 12.3 失败结果的行动指引

所有委托失败结果末尾追加 next-action 指引（codex `ERROR_NEXT_ACTION` 模式）：`If you still need the answer, delegate again with a narrower or more specific task, or investigate directly yourself.`——带指引的错误让主 Agent 的降级行为（缩范围重委托/自查）明显可预期。
