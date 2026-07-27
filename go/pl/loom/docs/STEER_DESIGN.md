# Loom Steer 设计（运行中追问）

| 项目 | 内容 |
|------|------|
| 状态 | Draft v1.1（待 review；v1.1 自审第二轮：prepare 执行时机精确化、补齐 Peek、FIFO 对齐论证、接力后面板语义） |
| 日期 | 2026-07-26 |
| 关联文档 | `DESIGN.md`（§25.3 用户交互、§26 模型协议）、`TUI_DESIGN.md`、`CONFIG_DESIGN.md` |
| 目标读者 | loom 运行时与前端贡献者 |

---

## 1. 背景与目标

loom 当前是严格单 turn 串行：模型生成或工具执行期间，用户的新输入被 `Controller` 拒绝（`cannot submit prompt in state %q`），TUI 只能把草稿弹回输入框。用户必须盯着进度等 turn 结束才能追问——这是日常使用中最主要的体验断点。

`DESIGN.md` §25.3 早已把这类交互写进蓝图（`interaction.requested/resolved`：问题、补充信息和 steering；`run.steering_requested`：在安全点取消未启动动作并创建新 Turn），但从未实现。本设计将其落地，目标：

1. **运行中可追问（steer）**：turn 进行中提交的消息不等待、不打断，在**下一次模型调用前**注入对话历史，模型自然看到并调整后续行为；
2. **打断即接力**：Ctrl+C 取消当前 turn 时，未消费的追问自动作为新 turn 的 prompt 接力，一条都不丢；
3. **统一队列语义**：注入与接力不是两套机制，而是**同一队列的两个消费时机**——loop 在迭代点消费（注入），turn 结束时消费（接力）；
4. **不改变既有单 turn 模型**：任何时刻仍然只有一个 active turn；steer 是 turn 内的消息注入，不是并发 turn。

### 1.1 与非目标

**非目标（本期不做）**：

- **并发 turn / 多任务并行**：单 active turn 模型不变；
- **真 steering（`run.steering_requested` 的完整语义）**：即"在安全点取消**未启动**动作并改方向"。本设计的 Ctrl+C 接力覆盖其主要场景（取消整个 turn + 新方向接力），"只取消未启动批次、保留已完成部分"的精细语义留待后续；
- **排队消息编辑**（codex 的 Alt+Up 编辑 queued message）：P2；
- **headless（`loom run`/`resume`）的 steer**：headless 是单 prompt 进程，无交互式追问场景，行为不变；
- **审批中追问的特殊通道**：awaiting_approval 时 steer 正常入队，审批解决后随 turn 继续被注入（§5 边界分析），不做"审批内嵌回复"协议。

---

## 2. 现状评估

### 2.1 硬约束

| 位置 | 现状 | 影响 |
|------|------|------|
| `app/controller.go` `handleSubmitPrompt` | 非 `idle` 状态直接拒绝 | busy 提交被弹回 |
| `ui/update.go` `submitUserInput` | 被 controller 拒绝后恢复草稿 | 用户等待 |
| `agent.Loop.Execute` | 阶段机循环（`prepare → callModel → (awaitApproval/executeTools/compact) → flushEvents`），每次模型调用前必经 `prepare()` | 天然的注入点 |

### 2.2 参照：Codex CLI 的 steer 机制

对 `/Users/liubang/workspace/github/codex`（codex-rs）的调研结论：

| 层 | codex 做法 | loom 对齐判断 |
|----|-----------|---------------|
| Steer 注入 | busy 提交注入当前 turn 的 `pending_input`，turn 主循环在**每次采样请求前** drain 进历史（`session/mod.rs:3955 steer_input`、`session/turn.rs:256`） | 完全对齐：loom 的 `prepare()` 是语义等价位置 |
| Rejected 排队 | Review/Compact 等独立 turn 类型不可 steer，排队等接力 | **不需要**：loom 的 compaction 是 turn 内阶段，所有 turn 均可 steer |
| 区分两种打断 | Esc 时有 pending steer → 取消 + 立即作为新 turn 提交；无 pending → 回 composer 编辑 | 对齐：loom 用"残留接力"实现前者；后者即现有取消语义 |
| `PendingInputPreview` | composer 上方分区显示 pending/queued | 对齐：复用 loom plan panel 的 pinned 渲染模式 |

### 2.3 已有地基

| 能力 | 位置 | 说明 |
|------|------|------|
| 异步通道先例 | `agent.GoalCell` / `PlanCell` | "外部写、loop 每轮读"的 mailbox 模式，steer 通道直接复刻 |
| 注入点先例 | `Loop.prepare()` | 每轮模型调用前执行；`maybeInjectBudgetNotice` 已在此注入系统消息，注释明确论证了该位置 **transcript pairing-safe**（不会在 assistant tool-call 与其 results 之间插入消息） |
| 消息 durable 链 | `Run.AddUserMessage` → `EventUserMessageAdded` → `publishingStore` | turn 内新增 user message 走既有 durable 事件路径，checkpoint/compaction 语义自动正确 |
| 取消全链路 | `Controller.CancelTurn` → turnCtx → loop terminate → terminal checkpoint | 接力机制的触发点现成（`onTurnFinished`） |
| pinned 面板先例 | `ui.renderPlanPanel` | composer 上方常驻面板的渲染模式，steer 面板直接复刻 |
| 运行时事件协议 | `runtimeevent` broker（durable/ephemeral 分离） | 新增 Kind 成本清晰 |

### 2.4 差距清单

| # | 差距 | 根因 |
|---|------|------|
| G1 | busy 提交被拒绝 | `handleSubmitPrompt` 的状态检查无排队分支 |
| G2 | loop 无法感知外部新消息 | `agent.Loop` 只有 GoalCell/PlanCell 两条工具→loop 通道，无用户→loop 通道 |
| G3 | 追问的持久化时点未定义 | user message 目前只在 turn 开始前写入 |
| G4 | UI 无 pending 呈现层 | 提交只有"乐观 echo → 确认"两态 |

---

## 3. 总体设计

### 3.1 核心语义：一个队列，两个消费时机

```text
用户提交（任意时刻）
   │
   ▼
Controller.SubmitPrompt(text)
   │
   ├─ idle ────────────────► 直接开启新 turn（现状不变）
   │
   └─ busy（running / awaiting_approval / cancelling）
        │
        ▼
   SteerCell.Put(text)          ← 进程级 mailbox（agent 包，仿 GoalCell）
        │
        ├─ 消费时机 ①：loop 在每次模型调用前的 prepare() 时 Take()
        │     → 逐条 Run.AddUserMessage（durable，与 turn 内其他事件同序列）
        │     → 本次模型调用的 messages 自然携带 → 模型"看到"追问
        │
        └─ 消费时机 ②：turn 结束（含完成/取消/失败）时 Take() 残留
              → 合并为一条 prompt（\n\n 连接）
              → 自动开启接力 turn（等价于用户排队等待）
```

关键性质：

- **任意时序下消息不丢**：注入发生在模型调用前；错过最后一个注入点的消息进入接力；接力 turn 的第一次 `prepare()` 前又有新追问，继续被注入——链式正确；
- **无并发 turn**：注入是 turn 内事件，接力是串行 turn，单 active turn 不变式全程成立；
- **Ctrl+C 语义自然外溢**：取消当前 turn → 消费时机 ② 触发 → 追问接力——这就是 codex 的"带消息打断"，不需要额外机制。

### 3.2 消息形态

| 时机 | 形态 | 理由 |
|------|------|------|
| 注入（时机①） | **逐条** `AddUserMessage` | 用户分次说的话保持独立消息，与 codex drain 行为一致 |
| 接力（时机②） | **合并**为一条 prompt（`\n\n` 连接） | 接力 turn 只能有一个 prompt；合并保留全部内容且语义为"一段补充说明" |

### 3.3 持久化与崩溃语义

- SteerCell 是**易失内存结构**，不入事件溯源：驻留其中的消息是"未交付"状态，生命周期以秒计；
- 消息一旦注入（时机①）即为 durable 的 `EventUserMessageAdded`，与 turn 内其他事件同事务刷盘，checkpoint/恢复语义自动正确；
- **崩溃窗口**：进程崩溃时 cell 中未交付消息丢失——与 codex `pending_input` 语义一致，可接受（用户可在 transcript 中看到自己最后一条追问是否已注入）。

---

## 4. 详细设计

### 4.1 `agent.SteerCell`（新）

```go
// SteerCell is the mailbox between the controller (receiving user input
// while a turn is busy) and the loop (which owns the Run). Unlike
// GoalCell it is a bounded FIFO: every queued message is preserved in
// submission order. Lives on the Bootstrap so leftovers survive a turn
// boundary and become the next turn's prompt (§3.1).
type SteerCell struct {
    mu    sync.Mutex
    queue []string
}

const steerCellCapacity = 8 // soft cap against runaway scripts (§5)

func NewSteerCell() *SteerCell
func (c *SteerCell) Put(text string) error // error when full
func (c *SteerCell) Take() []string        // drains and returns all, in order
func (c *SteerCell) Peek() []string        // ordered copy, non-destructive (Snapshot/PendingSteers)
func (c *SteerCell) Len() int              // for UI/snapshot visibility
```

放置位置：`Bootstrap.SteerCell`（与 GoalCell/PlanCell 同级），装配时注入每个 turn 的 `agent.Loop`。

### 4.2 Loop drain 点：`prepare()`

```go
func (l *Loop) prepare(ctx context.Context) error {
    // ... TransitionTo(PhaseCallingModel), IncrementTurn ...
    l.drainSteer()          // NEW: 注入追问（在 budget notice 之前）
    l.maybeInjectBudgetNotice()
    return nil
}

// drainSteer 把追问注入为正式 user message。该位置与 budget notice 一样
// 是 pairing-safe 的：只在"上一次工具结果已记录、下一次模型调用未发出"
// 的间隙执行，永远不会切开 assistant tool-call 与其 results 的配对。
func (l *Loop) drainSteer() {
    if l.SteerCell == nil {
        return
    }
    for _, text := range l.SteerCell.Take() {
        l.Run.AddUserMessage(domain.Message{
            ID: domain.NewMessageID(), Role: domain.RoleUser,
            Parts:     []domain.ContentPart{{Kind: domain.PartText, Text: text}},
            CreatedAt: l.Run.Clock.Now(),
        })
    }
}
```

注入后立即走既有 `flushEvents` → `publishingStore` → durable 事件链（§4.4）。注意 `prepare()` 在阶段机循环中并非每轮迭代都执行，而是**每次模型调用前恰好一次**（`prepare → callModel → (awaitApproval/executeTools/compact) → prepare → …`）——这正是注入语义想要的位置。

### 4.3 Controller：`SubmitPrompt` 语义扩展（不新增 API）

```go
// SubmitResult reports how a submitted prompt was accepted.
type SubmitResult struct {
    // Steered is true when the prompt was queued into the active turn's
    // SteerCell instead of starting a new turn immediately.
    Steered bool
    // QueueLen is the resulting pending-steer count (0 when started).
    QueueLen int
}
```

| 状态 | 行为 | 返回 |
|------|------|------|
| `idle` | 现状：开启新 turn | `{Steered: false}` |
| `running` / `awaiting_approval` / `cancelling` | `SteerCell.Put(text)`（满则报错，草稿恢复） | `{Steered: true, QueueLen: n}` |
| 其他（booting/fatal/closed） | 现状：拒绝 | error |

**接力**：`onTurnFinished`（turn 终结的唯一收口）在状态回落 `idle` 后检查 `SteerCell.Len() > 0`：

```go
if leftovers := c.steerCell().Take(); len(leftovers) > 0 {
    prompt := strings.Join(leftovers, "\n\n")
    // 经 cmdCh 自提交，保持命令串行化（handleSubmitPrompt 状态已是 idle）
    c.cmdCh <- controllerCommand{Kind: cmdSubmitPrompt, Prompt: prompt, ResultCh: ...}
}
```

接力对**所有**终结原因生效（完成/取消/失败/预算耗尽）：失败时追问接力等价于"自动重试并补充信息"，符合用户直觉；budget 耗尽时接力 turn 立即再次被 runaway 检查拦下，无失控风险。

### 4.4 运行时事件（runtimeevent 新增两个 Kind）

| 事件 | 通道 | 触发点 | UI 消费 |
|------|------|--------|---------|
| `steer.queued`（ephemeral） | broker | controller 在 `Put` 成功后发布，payload `{Text, QueueLen}` | pending 面板新增一条（§4.5） |
| `steer.injected`（durable） | publishingStore 消费 `domain.EventUserMessageAdded` 新增 case | loop drain 后 user message 落盘 | pending 面板移除对应条目，transcript 追加正式 user block |

事件序列一致性的论证：turn 开始时的 prompt 由 `controller.runTurn` 经 `KindTurnStarted` 发布（其 `EventUserMessageAdded` 走裸 store 刷盘，不经 `publishingStore`，无重复）；turn 内的 steer 注入只可能来自 drain——`publishingStore` 的新 case 恰好只命中 steer，语义无歧义。同时 cell 严格 FIFO：`steer.injected` 到达顺序与 `steer.queued` 一致，UI 按"移除头部第一条"即可精确对齐，无需按文本匹配（重复文本不误删）。

`Snapshot` 增加 `PendingSteers []string`：事件流重连/快照刷新时 pending 面板可重建。

### 4.5 UI

**提交路径**（`submitUserInput` 改造）：

| SubmitResult | 行为 |
|--------------|------|
| `Steered=false` | 现状：乐观 echo user block → 等待 turn.started 确认 |
| `Steered=true` | **不**进 transcript block 流；经 `steer.queued` 事件在 pending 面板新增条目；状态栏提示 `Queued — will inject before next model call` |

**Pending 面板**（复刻 plan panel 的 pinned 渲染，位于其上方）：

```text
  Steering (Ctrl+C to send now):
  ↳ 把这个函数改成并发安全的
  ↳ 另外把测试也补上
```

- 数据源：`steer.queued` 事件累积 + `Snapshot.PendingSteers` 兜底；`steer.injected` 按序移除；
- 注入后对应消息转为正式 user block（`steer.injected` 事件驱动，与既有 block 流一致）；
- 面板仅在非空时渲染，不占常驻高度；接力（时机②）取空 cell 后面板自然清空，接力 turn 运行中的新追问重新开启一轮 queued→injected 循环。

**打断语义**：Ctrl+C 维持现有双击确认与取消行为不变；取消后 `onTurnFinished` 的接力自动把 pending 面板消息转为新 turn——用户感知为"打断并立即发送"，与 codex 一致。pending 为空时 Ctrl+C 即纯取消（现状）。

### 4.6 headless 与 API 兼容

`runAgent`（headless）每个进程只提交一次 prompt，永远在 idle 状态提交——`SubmitResult.Steered` 恒为 false，行为不变。`SteerCell` 在 headless 装配中同样注入但永远为空，无成本。

---

## 5. 并发与边界分析

| # | 场景 | 分析 |
|---|------|------|
| B1 | 模型流式输出中提交 | cell 暂存；当前模型调用完成后，`executeTools`/`end_turn` 前无注入点——若模型返回 `end_turn`，turn 结束，走接力。若返回 tool_use，`executeTools` 后的下一轮 `prepare()` 注入 ✓ |
| B2 | 工具执行中提交 | 同 B1 后半：工具批次完成 → `prepare()` → 注入 ✓（即 codex 的 "submitted after next tool call"） |
| B3 | 审批挂起中提交 | cell 暂存；审批解决 → 工具执行 → `prepare()` → 注入；审批拒绝 → turn 继续或结束，语义一致 ✓ |
| B4 | 取消中（cancelling）提交 | cell 暂存；turn 终结 → 接力 ✓ |
| B5 | drain 与 Put 并发 | cell 内部 mutex；`Take()` 全量取走——drain 期间新 Put 的消息留在下一轮，顺序正确 ✓ |
| B6 | 接力与 SubmitPrompt 并发 | 接力经 `cmdCh` 入队，与外部提交串行；先到先开 turn，后到入 cell ✓ |
| B7 | 进程崩溃 | cell 未交付消息丢失（§3.3，与 codex 一致，可接受） |
| B8 | steer 消息触发 budget/runaway | 注入是 `AddUserMessage`，usage 语义与普通 prompt 一致；接力 turn 独立预算窗口 ✓ |
| B9 | compaction 与注入的相对顺序 | compaction 决策在 `prepare()` 前的阶段机顶部（`shouldCompact`），注入在 `prepare()`——压缩后注入的消息作为最新 user message 永远不被当轮压缩 ✓ |
| B10 | cell 满（8 条） | `Put` 报错 → UI 恢复草稿并提示——软上限防脚本失控，人手速不可达 |

---

## 6. 对现有代码的改动面

| 位置 | 改动 |
|------|------|
| `internal/agent/steer.go`（**新增**） | `SteerCell`（~60 行）+ 单测 |
| `internal/agent/run.go` | `Loop.SteerCell` 字段；`prepare()` 中 `drainSteer()` |
| `internal/app/controller.go` | `SubmitPrompt` 语义扩展 + `SubmitResult`；`handleSubmitPrompt` busy 分支；`onTurnFinished` 接力；`Snapshot.PendingSteers`；`steer.queued` 发布 |
| `internal/app/bootstrap.go` | `Bootstrap.SteerCell` 装配并注入每个 turn 的 Loop |
| `internal/app/controller.go`（publishingStore） | `EventUserMessageAdded` → `steer.injected` case |
| `internal/runtimeevent/event.go` | 新增两个 Kind + payload 类型 |
| `internal/agent/BUILD`、`internal/app/BUILD` | steer.go / steer_test.go 入 srcs（bazel） |
| `internal/ui/model.go` | `Model.pendingSteers []string` 状态字段 |
| `internal/ui/update.go` | `submitUserInput` 按 `SubmitResult` 分流；pending 面板状态与事件消费；Ctrl+C 文案 |
| `internal/ui/view.go` | `renderSteerPanel()`（复刻 plan panel 模式） |
| `internal/ui/ui_test.go` 等 | 见 §7 |

## 7. 测试策略

| 层 | 内容 |
|----|------|
| `agent` | SteerCell FIFO/上限/Take 清空；`prepare()` drain 后 `run.Messages` 含注入消息且事件 durable（fakes 模式） |
| `app` | busy 提交 → cell；turn 结束自动接力（FakeModel 验证第二 turn 的 messages 含合并 prompt）；注入的 user message 产生 `steer.injected` 事件；取消后接力；cell 满报错；Snapshot.PendingSteers |
| `ui` | steered 提交进 pending 面板不进 block 流；`steer.injected` 后转正式 block；面板随 Snapshot 重建 |
| 端到端 | `fakes.FakeModel` 两阶段脚本：turn1 工具循环中 steer → turn1 后续模型调用 messages 含追问（对齐 codex steer 语义的核心验收） |

## 8. 分阶段实施

| 阶段 | 内容 | 验收 |
|------|------|------|
| **P1（本期）** | §4 全部：SteerCell、drain、controller 扩展、接力、事件、UI 面板 | §7 全绿；`loom chat` 中 busy 提交可见注入与接力 |
| **P2（后续）** | queued 消息编辑（Alt+Up）；真 steering（取消未启动批次） | 按需求驱动 |

## 9. 开放问题

1. **注入消息是否需要模型可见的标记**（如 `[user steer]` 前缀）让模型区分"原计划"与"中途补充"？倾向：不加——用户消息天然就是最高优先级信号，前缀反而教模型可以区别对待。若实际使用中发现模型忽视追问，再以 system note 形式补充。
2. **cell 容量 8 的依据**：纯经验值（codex 无上限；loom 软上限防失控）。若实际触达，放宽为 16 或改为"满时最老一条自动接力"。
3. **接力合并的分隔符**：`\n\n` 拼接多条残留。备选：逐条开多个接力 turn（turn 数膨胀，拒绝）；或 JSON 数组（污染模型上下文，拒绝）。
