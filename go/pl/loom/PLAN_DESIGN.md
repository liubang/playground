# Plan 功能设计（update_plan）

> 状态：草案 → 实现中
> 参考：OpenAI Codex CLI 的 `update_plan` 工具
> 关联文档：DESIGN.md（事件溯源/预算语义）、TUI_DESIGN.md

## 1. 背景与目标

Loom 目前缺少任务计划能力：模型在执行多步任务时无法维护一份结构化、持久化的步骤清单。这带来三个实际问题：

- 长程任务中用户看不到"进行到哪一步、还剩几步"，可观测性只能靠滚动消息；
- 上下文压缩（尤其是 Level 3 摘要替换）会丢弃消息历史，模型对自己制定的计划失忆；
- `domain.Plan`/`EventPlanRevised`/checkpoint 的 `plan` 字段早已存在，但从未有生产端写入，是纯死代码。

目标：落地 `update_plan` 工具，让模型像 Codex 一样维护任务计划；计划随 checkpoint 持久化、抗压缩、可恢复，并在 TUI 中可见。

非目标（v1 不做）：

- 不改变 run 的终止/续跑语义：plan 是 **advisory**（模型自管理 + 用户可观测），不像 Goal 那样驱动自动续跑；
- 不做计划审批、不做子计划/层级、不做多 plan 并存；
- 不做 plan 完成后的自动 wrap-up 注入。

## 2. 参考实现：Codex update_plan 要点

Codex（`gpt_5_codex_prompt.md`，原文照录）：

> ## Plan tool
> When using the planning tool:
> - Skip using the planning tool for straightforward tasks (roughly the easiest 25%).
> - Do not make single-step plans.
> - When you made a plan, update it after having performed one of the sub-tasks that you shared on the plan.

工具语义（codex-rs 实现）：

- 入参为**全量计划快照**：`{explanation?, plan: [{step, status: pending|in_progress|completed}]}`，每次调用整体替换而非增量 diff——简单、幂等、崩溃安全；
- 服务端校验：**至多一个 `in_progress`**，违反则工具报错；
- TUI 常驻渲染 checklist（☐/◐/☑），随工具调用实时更新；
- plan 是 session 级状态，与消息流正交。

## 3. 现状分析

已有（零改动可复用）：

| 组件 | 位置 | 状态 |
|---|---|---|
| `Plan`/`PlanItem` 类型 + `Validate`（已含 at-most-one-in_progress） | `internal/domain/plan.go` | 完整 |
| `EventPlanRevised`（`plan.revised`） | `internal/domain/event.go` | 完整 |
| checkpoint `Plan` 字段随持久化往返 | `internal/domain/interfaces.go` | 完整 |
| `RecoverRun` 回放 `plan.revised` 恢复 plan | `internal/agent/run.go` | 完整 |
| transcript projector 忽略该事件（不影响消息投影） | `internal/session/transcript.go` | 完整 |

缺口（本次实现）：

1. 没有 `update_plan` 工具，事件从未被写入；
2. Loop 不消费 plan（drain、上下文回注）；
3. 系统 prompt 无 plan 指引；
4. TUI 无渲染（runtimeevent 无对应 kind，UI 无 plan 状态）。

关键设计约束（来自既有架构）：

- 工具不能直接触碰 `Run`：走 **mailbox 模式**（参照 `goal.go` 的 `GoalCell`/`drainGoalUpdates`），loop 在工具批后统一 drain 并追加审计事件；
- `plan.revised` 事件的 payload 必须是 `domain.Plan` 本体——`RecoverRun` 现有回放逻辑直接 `json.Unmarshal(event.Payload, &plan)`，且历史上从未写入过该事件，无旧数据兼容负担；
- loom 的 `PlanItem{Index, Goal, Status, Evidence}` 与 Codex 的 `{step, status}` 对应关系：`Goal↔step`、`todo↔pending`、`done↔completed`；`Evidence` 是 loom 增强（完成证据，呼应 workflow 的"验证闭环"文化）。

## 4. 设计

### 4.1 领域模型

复用 `domain.Plan` 不动结构。工具入参是快照的**轻量形**（模型不传 `Index`，由工具按数组顺序重排为 0..n-1）：

```json
{
  "plan": [
    {"goal": "阅读现有实现", "status": "done", "evidence": ["读完 run.go/goal.go"]},
    {"goal": "实现 update_plan 工具", "status": "in_progress"},
    {"goal": "补测试并回归", "status": "todo"}
  ]
}
```

校验规则（工具层 `Prepare` 强制执行，错误作为工具错误结果返回给模型，可重试修正）：

1. `plan` 必填且 `len(items) >= 2`（Codex 规则：不做单步计划；全量快照语义下，"收尾"提交的是全 done 的多步计划，不受影响）；
2. 每项 `goal` 非空（trim 后）、`status ∈ {todo, in_progress, done}`；
3. 至多一个 `in_progress`（复用 `Plan.Validate`）；
4. `evidence` 可选，仅对 `done` 项有意义；不强制（避免卡模型），prompt 中鼓励。

### 4.2 update_plan 工具

新文件 `internal/agent/plan.go`，镜像 `UpdateGoalTool` 的接线：

- `PlanCell`：mailbox（与 `GoalCell` 同构，`Put`/`Take`，一批内多次调用取最后一次）；
- `UpdatePlanTool`：`Name: "update_plan"`，`Risk: domain.R1`（纯簿记，无需审批，与 update_goal 一致）；
- `Execute` 只做"接收并入箱"，回执 `{applied, items, note}`；真正生效在 loop drain 时（与 goal 的"tool batch 后生效"语义一致，保证工具结果可重放）；
- 工具描述内联核心规则（skip 简单任务 / 禁止单步 / 至多一个 in_progress / 完成一步更新一次 / 不要在消息里复述计划）。

### 4.3 Loop 集成（`internal/agent/run.go`）

1. `Loop` 新增字段 `PlanCell *PlanCell`（nil 时 drain 为 no-op，测试/最小装配不炸）；
2. `executeTools` 末尾、`drainGoalUpdates()` 旁调用 `drainPlanUpdates()`：
   - 取箱内快照 → `Plan.Validate()` 双保险（失败：logger warn + 丢弃本次更新，绝不让坏 plan 杀死 run）→ `Run.Plan = plan` → `appendEvent(EventPlanRevised, plan)`；
3. **plan 状态回注**（loom 相对 Codex 的增强，解决"压缩后计划失忆"）：`effectiveMessages` 构建请求时，若 `Run.Plan` 非空且未完成，在 system prompt 之后追加一条**临时** system 消息渲染当前 checklist（含各项 status 与 evidence）。
   - 每次请求按最新状态重建，不落 transcript、不进事件——天然抗压缩（Level 3 摘要丢弃消息也丢不了它），恢复后同样自动生效；
   - 渲染格式（纯文本、紧凑）：
     ```
     [task plan] 2/4 done; current: 实现 update_plan 工具
     1. [done] 阅读现有实现 — 证据: 读完 run.go/goal.go
     2. [in_progress] 实现 update_plan 工具
     3. [todo] 补测试并回归
     ```

### 4.4 持久化与恢复

零改动：

- `flushEvents`/`saveTerminalCheckpoint` 的 checkpoint 已带 `Plan` 字段；
- `RecoverRun` 已回放 `plan.revised`（恢复最新 plan）+ 终态 checkpoint 直接带 plan 进 `ContinueRun`；
- `loom inspect` 输出自带 checkpoint plan，可直接用于验收。

### 4.5 系统 prompt

`internal/prompt/prompt.go` 的 `builtinSections()` 新增一节 `loom://builtin/plan`（位于 workflow 节之后），照 Codex 三条核心规则并按 loom 语境扩展：

- 简单直接的任务（约最简单 25%）不要用 update_plan；
- 不做单步计划；
- 制定计划后，每完成一个子任务就更新：先把当前步标 `done`（尽量附一句 `evidence`），再把下一步标 `in_progress`；任意时刻至多一个 `in_progress`；
- 计划持久保存，压缩与中断恢复后仍然有效，其最新状态会在每次请求前自动出现在你的上下文里——不要在回复消息中复述计划。

### 4.6 TUI 渲染

- `internal/runtimeevent/event.go`：新增 `KindPlanUpdated`（`plan.updated`），payload 为 `domain.Plan`；
- `internal/app/controller.go`：域事件桥新增 `case domain.EventPlanRevised` → `publishDurable(KindPlanUpdated, plan)`（镜像 `EventContextCompacted` 的现有桥法）；恢复续跑时若 checkpoint 带非空 plan，session 打开后补发一次；
- `internal/ui`：
  - model 存 latest plan；
  - transcript 维护一个**就地更新**的 plan 块（checklist：`[x]` done /`[>]` in_progress /`[ ]` todo，首行 `plan · N/M done`），新 revision 更新同一块而非追加（参考流式文本块的就地更新模式）；
  - 状态栏新增 `plan:N/M` 段（有 plan 时显示，紧凑优先）。

### 4.7 与 Goal 的分工

| | update_goal | update_plan |
|---|---|---|
| 语义 | 跨 turn 的目标 + token 预算 | 步骤清单 + 进度 |
| 驱动行为 | 驱动自动续跑/wrap-up | 纯 advisory，不改变控制流 |
| 完成判定 | 需证据审计后 close | 全部 done 即完成，无额外动作 |

两者可独立使用：短任务都不用；长任务可只用 plan；需要跨 turn 自动续跑时用 goal（goal 的续跑 prompt 不感知 plan，plan 状态靠 4.3 的回注抵达模型）。

### 4.8 与 compaction 的交互

- masking/archival/Level 3 均不触碰 checkpoint 的 Plan 字段；
- Level 3 摘要替换会丢消息历史，但 4.3 的回注使模型在下一次请求即重新看到最新 plan——这是 plan 相对"消息里写 TODO"的结构性优势；
- `EventPlanRevised` 被 transcript projector 忽略，不参与消息投影，不影响 sequence 不变式（与此前 sequence 修复正交）。

## 5. 边界与失败模式

| 场景 | 行为 |
|---|---|
| 模型提交非法快照（单步/多空 goal/两个 in_progress） | 工具返回 error 结果（invalid_input + 原因），run 不受影响，模型可修正重试 |
| drain 时校验失败（防御性） | warn + 丢弃，保留旧 plan |
| 一批内多次调用 update_plan | 取最后一次（与 goal cell 一致） |
| run 失败/中断时 plan 未完成 | checkpoint 保留现状；resume 后 plan 与回注恢复 |
| 模型从不调用 update_plan | 可接受（prompt 引导非强制），无 plan 段/块，行为与现状一致 |
| PlanCell 为 nil（最小装配） | drain no-op；工具仍可注册（Execute 入箱，无人取——注册侧保证非 nil，防御兜底） |

## 6. 测试计划

单元/集成（新增 `internal/agent/plan_test.go` 及既有文件补充）：

1. 工具：`Prepare`/`Execute` 参数校验矩阵（合法快照、单步拒绝、未知字段拒绝、两个 in_progress 拒绝、evidence 透传、index 重排）；
2. loop：executeTools 后 drain 生效 + `plan.revised` 事件追加；非法快照不杀 run；一批多次调用取最后；
3. 回注：`effectiveMessages` 在 plan 非空未完成时含 plan 消息、完成后不含、消息不落 transcript；
4. 恢复：`RecoverRun` 回放 plan.revised（已有用例补断言）、终态 checkpoint → `ContinueRun` plan 保留；
5. prompt：builder 含 plan 节；
6. ui：runtimeevent kind 注册/校验、plan 块渲染与就地更新、状态栏段；
7. 回归：`bazel test //go/pl/loom/...` 全绿。

E2E（真实模型 + 真实 DB）：

1. `loom run` 跑多步任务 → sqlite 验证 `plan.revised` 事件序列及状态迁移（todo→in_progress→done）；
2. 触发压缩后任务继续 → 验证压缩后模型仍按计划推进（回注生效）；
3. `loom resume` 追问 → plan 恢复；
4. checkpoint plan 与 `loom inspect` 输出核对。

## 7. 验收标准

- 多步任务中模型自发创建并推进计划（至少一次 create + 两次状态迁移）；
- `plan.revised` 事件持久化，payload 通过 `Plan.Validate`；
- 压缩与 resume 后 plan 完整，回注消息可见于请求日志/trace；
- TUI 状态栏显示 `plan:N/M`，transcript plan 块随 revision 更新；
- 全量测试绿；设计文档、代码、测试结果一致。

## 8. 实施步骤（文件级）

1. `internal/agent/plan.go`（新）：PlanCell、UpdatePlanTool、drainPlanUpdates；
2. `internal/agent/run.go`：Loop.PlanCell 字段、executeTools drain、effectiveMessages 回注；
3. `internal/agent/BUILD`：登记 plan.go / plan_test.go（srcs 为显式枚举）；
4. `cmd/loom/main.go`、`internal/app/bootstrap.go`：注册 update_plan；
5. `internal/prompt/prompt.go`：plan 指引节；
6. `internal/runtimeevent/event.go`：KindPlanUpdated；
7. `internal/app/controller.go`：plan.revised 桥接 + 恢复补发；
8. `internal/ui/{model,update,view,blocks}.go`：plan 状态、状态栏段、checklist 块；
9. 测试与 E2E（见第 6、7 节）。
