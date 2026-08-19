# Loom Context 与预算管理重设计

> 状态：v2 已实施（2026-07-29；一轮独立评审修正 2 blocker / 6 major / 14 minor 后落地，bazel 31/31 测试通过）；v3 预算维度重构（删除 wall_time 预算、会话级 token 预算、stall 看门狗）**已实施**（2026-07-29，动机：会话 `sess_a49f833ae4b228beb17353b49ae016c0` 实测复盘；bazel 31/31 测试通过）
> 作者：liubang
> 日期：2026-07-29
> 参考：OpenAI codex（`codex-rs/core`）的 context 管理体系；会话 `sess_b0d9afeafa12de91d5cdf01bdee52868` 的事故分析

## 1. 背景

### 1.1 事故复盘

会话 `sess_b0d9afeafa12de91d5cdf01bdee52868`（2026-07-29，glm-5.2，200k 窗口）执行一个代码核验任务，最终 `budget_exhausted` 终止，**用户未收到任何结论**。事件链：

1. `Condenser.TargetTokens` 写死 32k，而模型窗口 200k —— transcript 估算到 32k（窗口的 16%）就触发压缩，11 分钟内压缩 3 次；
2. 第一次压缩把 REVIEW.md 的 39KB 阅读结果 mask 成 artifact → 模型后续 4 次用 `run_cmd grep` 重新提取（每次触发一次人工审批，共等待 147s），并重复读取 `policy.go`×2、`rules.go`×5、`stream.go`×2；
3. 重读与审批推高回合数，50 回合耗尽；
4. `Execute` 主循环只调用 `CheckRunaway()`（纯硬检查），`CheckBudget()` 的软告警（80%）从未接入生产路径；`budgetNoticeLevel` 只覆盖 context occupancy 一个维度；
5. 撞满 `MaxTurns=50` 后立即 `terminate`，当时模型还在核验中途（最后一条助手消息是一个工具调用）——没有收尾回合，没有最终答复。

### 1.2 根因归纳

| 根因 | 现状 | 后果 |
|------|------|------|
| 压缩阈值与模型窗口脱钩 | `defaultTargetTokens = 32_000` 写死 | 200k 窗口下 16% 占用即压缩，误压缩 |
| 压缩触发依赖纯估算 | `shouldCompact` 第一判据是 `est > TargetTokens`（bytes/4），不看 provider 实测 | 估算偏差直接转化为误触发 |
| 预算无梯度提醒 | 软告警未接入主循环；notice 只有 occupancy 单维度两档 | 从"正常"直接跳到"硬杀" |
| 硬上限无软着陆 | `CheckRunaway` 命中即 `terminate`；只有 goal token budget 有 wrap-up 回合 | 任务中途暴毙，零结论输出 |
| 失控检测缺位 | `MaxRepeatedActions`/`MaxParallelTools` **无消费方（死配置，REVIEW R9）**，唯一防线是数回合 | 重复非法调用拦不住，正常长任务反被误杀 |
| 工具输出上限偏小且割裂 | `MaxToolOutputBytes = 16KB` 仅 run_cmd 消费；read_file/grep/list_dir 各有独立上限 | 大量输出外部化，压缩后被迫重读/重取 |

### 1.3 codex 的对照做法

- 压缩阈值 = `context_window × 90%`（配置只能调低），按 provider 实测 token 计量；
- 没有 MaxTurns / MaxToolCalls，context 压力全部由压缩吸收（mid-turn 压缩后回合继续）；
- 会话级 token 预算：加权计量（output×weight + 非缓存 input×weight）+ **梯度提醒数组** + 耗尽前注入一次"自救提示"；
- 工具输出**入库即截断**（per-model 默认 10k tokens，保留头尾、告知原始大小）；
- 切换小窗口模型时主动压缩（`ModelDownshift`）；压缩后给用户可见的准确性提醒；
- **无 wall-clock 预算维度**（v3 核实 codex 源码：`thread_goal.time_used_seconds` 仅计量展示、注入提示文案，不参与任何阈值判定，`status_after_budget_limit` 只按 token 判定）；失控由行为信号 + opt-in token 预算 + 用户中断共同兜底。

## 2. 设计原则

1. **窗口是唯一尺度**。一切阈值（压缩触发、压缩目标、提醒档位）都从模型的有效上下文窗口按比例推导，禁止与窗口脱钩的绝对值常量。
2. **实测优先，估算兜底**。occupancy 以 provider 实测 input tokens 为准；bytes/4 估算只用于尚无实测数据的冷启动。
3. **context 压力全部由压缩吸收**。压缩可以在一个 prompt 内发生任意多次；回合不因压缩而终止。
4. **预算只计量稀缺资源本身，不用代理指标**。稀缺资源是 tokens 与成本（会话级累计）；**禁止用"回合数"这类代理指标惩罚工作量，累计墙钟同属代理指标——时间本身不是稀缺资源**。失控（死循环、连续失败、停滞）用行为检测识别，不用预算识别；时间维度仅以"距上次进展的活跃时长"参与停滞检测（v3 修正，原"时间用 wall time 直接计量"废弃）。
5. **任何终止都有结论**。预算维度在硬杀之前必须有梯度提醒和一次软着陆收尾回合；软着陆状态必须可崩溃恢复，正常任务永远不该"暴毙"。
6. **用户意图是锚**。真实用户消息在任何压缩级别下都优先逐字保留；被压缩的应是工具输出和中间推理过程。
7. **状态外置优于总结**。plan / goal 等结构化状态存于 Cell（不进 transcript），天然跨压缩存活，减少对模型总结的依赖。

## 3. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│ WindowModel（单一事实源）                                     │
│   effective_window = context_window × utilization           │
│   推导：compact_trigger / compact_target / notice 档位        │
└──────────────┬──────────────────────────────┬───────────────┘
               │                              │
┌──────────────▼──────────────┐  ┌────────────▼───────────────┐
│ Pressure（context 压力）      │  │ Budget（预算与软着陆）        │
│   Occupancy（实测优先）       │  │   梯度提醒（全维度）           │
│   Compactor（三级管线）       │  │   软着陆 wrap-up 状态机       │
│   trigger/target 全窗口化     │  │   失控行为检测（非预算）       │
└─────────────────────────────┘  └────────────────────────────┘
```

- **WindowModel**：新建的小型纯值对象，输入模型元数据（context_window）和配置，输出全部派生阈值。`Loop` 与 `Condenser` 共享它，消除"压缩目标"与"占用告警"两套尺度并存的现状。
- **Pressure**：负责 occupancy 计量与压缩决策。替代现有 `shouldCompact` / `contextOccupancy` / `Condenser.TargetTokens` 的拼接逻辑。
- **Budget**：负责所有预算维度的梯度提醒与软着陆，以及失控行为检测。统一并取代现有 `budgetNoticeLevel`（仅 occupancy）与 goal 专用 wrap-up 两套机制。

## 4. 详细设计

### 4.1 WindowModel：窗口模型与阈值推导

```go
// WindowModel 是从模型元数据推导的全部 context 阈值集合（纯值对象）。
type WindowModel struct {
    // Nominal 是模型声明的 context window（tokens）。
    Nominal int64
    // Effective 是可安全使用的窗口：Nominal × Utilization。
    Effective int64
    // CompactTrigger 是自动压缩触发线（occupancy 达到即压缩）。
    CompactTrigger int64
    // CompactTarget 是单次压缩的目标 occupancy（压到该值以下）。
    CompactTarget int64
    // NoticeLevels 是梯度提醒触发线（升序，全部低于 CompactTrigger，见 4.4）。
    NoticeLevels []int64
}
```

默认比例（均可配置，见 §6）：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `utilization` | 0.95 | 有效窗口占比，为 system prompt、工具定义、输出预留余量 |
| `compact_trigger_ratio` | 0.80 | 相对 Effective；与 codex 的 90%（相对 nominal）量级一致 |
| `compact_target_ratio` | 0.50 | 相对 Effective；压缩后预留一半窗口给后续工作 |
| `notice_levels` | [0.60, 0.75] | 相对 Effective，**全部低于 trigger**：收敛提醒 / 压缩临近预告 |

以 200k 窗口为例：Effective=190k，触发线 152k，压缩目标 95k，提醒线 114k / 142k。对比现状（32k 触发）：**压缩频率下降约 5 倍**，且随模型窗口自动伸缩。

档位语义与数值自洽性（评审 B1 修正）：提醒档位必须全部低于压缩触发线——高于 trigger 的档位永远发不出（occupancy 到 trigger 即压缩并降回 target），且会违反 §6 的启动校验。档位 2 的语义因此是"压缩临近预告"而非"自交接要求"：模型收到后应在下一次可见回复浓缩关键状态（文件路径、已确认结论、剩余步骤），使信息在即将到来的压缩中幸存。

约束：
- `target_ratio < trigger_ratio`、`notice_levels` 严格升序且全部 < `trigger_ratio`、`0 < utilization ≤ 1` —— 启动期硬校验（fail-fast），**默认配置必须通过自身校验**（回归测试覆盖）；
- 各比例允许用户向任意方向调整（更激进或更保守均合法），只有上述顺序约束是硬性的；
- 模型未声明 context_window 时，回退 `Limits.MaxInputTokens`（沿用现状语义），所有比例照常生效；
- 配置可针对单个模型覆盖 `utilization`（如某些网关对窗口虚标）。

### 4.2 Occupancy：实测优先的占用计量

```
occupancy = 最近一次实测窗口占用（lastCallContext，cache-inclusive）
          + 该次请求组装之后新增 transcript 的 bytes/4 估算
```

- **实测口径即 provider 的窗口占用**：Anthropic 为 `input_tokens + cache_read_input_tokens + cache_creation_input_tokens`（后两者被 Anthropic 排除在 `input_tokens` 之外，但同样占据窗口），OpenAI 为 `prompt_tokens`（本身含 cached）；provider 通过 `ModelEvent.ContextTokens` 上报，不区分时回退为计费 input。
- **增量口径按请求组装点切分**（`lastCallBase`）：该次响应自身、随后追加的工具结果与 steer 全部计入——不能按「最后一条 assistant 消息」切分（loom 的工具结果消息同为 assistant 角色，那样切增量恒为 0）。
- 该公式**成为唯一计量函数**：压缩触发、梯度提醒、前端占用环（`context.usage` 事件与 snapshot occupancy）全部使用它，UI 显示与触发器永远同源；
- **废除** `shouldCompact` 中 `est > TargetTokens` 这一独立估算判据 —— 它是本次事故误压缩的直接根源；
- **冷启动语义**（评审 m9 补充）：`Loop` 每个 prompt 由 controller 整体重建，`lastCallContext` 不跨 prompt 存活，因此**每个 prompt 的首个模型调用前 occupancy 为完整请求估算**（system prompt 各部 + plan note + transcript + tool schemas，与 `effectiveMessages` 共用同一前缀构建）；turn 内第二次调用起即为实测校准值。该估算偏差只影响"首个请求前是否压缩"的判断，方向保守（宁可多压一次，不会漏压导致 provider 拒绝——后者有 overflow 强制压缩兜底）；
- `/model` 切换同理：新模型的 Loop 从完整估算起步，不存在"上一个模型的实测值污染新模型 occupancy"的问题（评审 m9 确认）；
- 压缩成功后 `lastCallContext` 清零，occupancy 回退为压缩后 transcript 的完整请求估算（含固定开销），并立即向 UI 补发；
- `estTokens` 降级为压缩管线内部的前后对比指标与事件上报字段，不再参与触发决策。

### 4.3 Compactor：窗口化的三级压缩管线

保留现有三级管线（这是 loom 相对 codex 单级总结的优势），仅做以下改造：

```
Level 1  mask：     窗口外的大工具输出 → artifact + 指针占位
Level 2  archive：  最老消息段 → 全保真 JSON artifact + marker
Level 3  summarize：模型生成 handoff 总结，围绕总结重建 transcript
```

1. **目标窗口化**：三级统一以 `WindowModel.CompactTarget` 为目标，删除 `Condenser.TargetTokens` 字段与 `defaultTargetTokens` 常量；`Condenser` 改持 `WindowModel`。
2. **L3 自身超限的降级**（借鉴 codex `remove_first_item`）：summarize 请求若被 provider 以 context-overflow 拒绝，从最老消息开始逐个丢弃（pairing-safe 边界）后重试，最多 N=3 次；仍失败则保留 Level 1/2 的 masked 历史（现状的兜底语义不变）。
3. **用户消息锚点**：L3 重建时真实用户消息逐字保留（现状 `buildSummaryReplacement` 已有），预算从 `summaryUserMessageMaxBytes`(80KB) 改为 `CompactTarget` 的 20%（按 bytes/4 折算）。注意行为变化（评审 m13）：小窗口模型下该预算同比收缩（如 65k 窗口约 25KB），最老用户消息被逐字保留的数量减少——方向正确（一切随窗口缩放），但需要在压缩事件中记录被截断的用户消息数以便观察。
4. **触发时机**（与 codex 对齐）：
   - **pre-turn**：每次模型调用前检查（现状已有，唯一决策点）；
   - **mid-turn**：压缩后 `TransitionTo(PhasePreparing)` 回合继续（现状已有，语义保留）；
   - **overflow 强制**：provider 拒绝 → `ForceCompact` 重试，连续 2 次失败才终止（现状已有）；
   - **ModelDownshift（简化，评审 m10）**：`/model` 在 turn 边界生效、Loop 随之重建，`ContextWindow` 取新模型元数据后，窗口化触发器在下一 turn **自然生效**——无需专项机制，只在因新窗口压缩时将事件 `trigger` 标记为 `downshift`（当前 occupancy ≥ 新 trigger 且本 run 尚未用该 WindowModel 压缩过）；
   - **手动**：`/compact`（现状 `ForceCompact` 已有）。
5. **压缩后用户提醒（新增）**：每次自动压缩后向 UI 发布一条提示（不进 transcript）："已压缩上下文（occupancy 152k→71k）。长会话 + 多次压缩可能降低模型准确性，适当时建议开新会话。"UI 以非阻塞横幅呈现。
6. **压缩事件扩充**：`context.compacted` payload 增加 `trigger`（auto/manual/overflow/downshift）、`phase`（pre_turn/mid_turn）、`occupancy_before/after`（实测口径，有实测时）、`truncated_user_messages`（L3 丢弃的逐字用户消息数）。

### 4.4 Budget：梯度提醒与软着陆

#### 4.4.1 全维度梯度提醒

将现有 occupancy 单维度的 `maybeInjectBudgetNotice` 泛化为全预算维度：

| 维度 | 档位 1（advisory） | 档位 2 |
|------|--------------------|--------|
| context occupancy | 60% Effective（收敛提醒） | 75% Effective（压缩临近预告） |
| tokens（会话级累计） | 80% | 95% |
| cost_usd（会话级累计） | 80% | 95% |

预算维度只保留**稀缺资源本身**（tokens、成本）与 context 压力；回合数/工具调用数/累计墙钟均不再作为预算维度（见 4.4.3）。tokens 与 cost 为**会话级累计口径**：跨 prompt 不重置（v3 变更，原 per-prompt 窗口废弃，理由见 4.4.3）。

- occupancy 档位 1 文案："上下文占用 ~X/Y。继续工作，但请收敛范围、避免重读大输出。"
- occupancy 档位 2 文案："上下文占用 ~X/Y，自动压缩临近。请在**下一次可见回复**中浓缩关键状态（已确认的结论、文件路径、剩余步骤），使其在压缩后幸存。"
- tokens/cost 档位 2 文案："预算即将耗尽（~X/Y，维度 Z）。请立即收敛，做好收尾准备。"
- 注入点沿用现状 `prepare()`（transcript pairing-safe 的唯一安全点）；tokens/cost 档位判定直接使用会话累计 usage（v3：`touchWallTime` 的预算职责随 wall_time 维度一并删除）；
- 每个档位每个维度**每 prompt 只发一次**（Loop 每 prompt 重建，"每 run"语义即为"每 prompt"）；压缩成功后 occupancy 维度的档位重新武装，其余维度单调不重置。已知行为（v3 接受）：tokens/cost 的 notice 去重状态随 Loop 每 prompt 重建而丢失，usage 持续高于档位时新 prompt 会重发一次同档提醒——提醒在新 prompt 重现符合直觉；checkpoint 持久化 notice 状态列为可选优化；
- **持久化约定**（评审 m11）：注入的消息作为普通 transcript 消息落事件（`user.message_added` 或专用 system note 事件），同时落 `budget.notice` 审计事件（dimension/level/usage/limit）。现状 `AddSystemNote` 刻意不落事件、不动 version，仅用于临时性提示；梯度提醒属于影响模型行为的预算决策，必须可回放，故走事件双写，不复用 `AddSystemNote`。

#### 4.4.2 软着陆 wrap-up 状态机

预算维度（tokens/cost）命中硬上限、或停滞看门狗（`stall_timeout`）判定卡死时**不再直接 terminate**，进入软着陆。完整状态机规定如下（评审 M1/M2/M3 修正；v3 扩展触发源）：

**进入条件**：硬命中只在 `PhasePreparing`（下一次模型调用前）被观察——这是 transcript pairing-safe 点。其他 phase（执行工具批次中途、等待审批返回后）先完成当前 phase 迁移，到达 `PhasePreparing` 再统一判定。循环顶部的无条件硬检查改为"若 `wrapUpPending` 未置位才检查"。

**执行**：

1. 置 `wrapUpPending = true`，落 `budget.wrapup_started` 事件（dimension/usage/limit），注入 wrap-up 用户消息："运行预算（维度 X）已用尽。这是最后一个回合：请立即总结已完成的工作、已验证的结论、剩余事项与建议的下一步。**不要再调用工具**。"；
2. 模型产出**无工具调用**的可见回复后终止：预算维度用 `Terminate(OutcomeBudgetExhausted)`；停滞触发用 `Terminate(OutcomeFailed)` 并附 stall 原因（资源耗尽是正常收尾，停滞是异常终态，两者在事件与 UI 上须可区分）；
3. **防滥用**（评审 M2）：wrap-up 回合中模型仍发起工具调用时，**一律自动 deny，不进入审批流程**——对每个 call 直接记录 `permission_denied`（"run is in budget wrap-up; tool calls are disabled"）的工具结果，保证 tool_call ↔ tool_result 配对完整，随后 terminate。由此消除"软着陆退化为软挂起（等待人工审批）"的漏洞；
4. "恰好一次"：`wrapUpPending` 置位期间硬检查被抑制；terminate 或强制终止后状态随 run 终态消亡，不存在第二次注入。

**崩溃恢复**（评审 M3）：`wrapUpPending` 是 Loop 内存字段，崩溃即丢失。恢复路径两条（双保险）：

- 主路径：`budget.wrapup_started` 已落事件，`RecoverRun` 重放时发现该事件且无后续终态事件 → 重放后重新置位 `wrapUpPending`，继续软着陆；
- 兜底：transcript 尾部存在未应答的 wrap-up 用户消息（无对应助手回复）→ 等同置位（与 `unresolvedToolCalls` 恢复模式一致）。

**与 goal 的统一**：goal 的 `goalWrapUpPending` 并入本状态机——goal token budget 耗尽只是"维度=goal_tokens"的一种软着陆，共享同一进入条件、防滥用与恢复逻辑，提示词骨架复用。

由此保证：**任何预算维度的硬终止之前，用户一定能拿到一份结论性总结，且该保证在崩溃恢复后依然成立**。

#### 4.4.3 删除回合配额，建立失控行为检测

**删除 `MaxTurns` 与 `MaxToolCalls` 两个预算维度**（配置项 `limits.max_turns`/`max_tool_calls`；对应 env 读取点已在 CONFIG_DESIGN P1 中移除，此处仅删配置 schema 与 domain 字段）。理由：

- 回合数是成本/时间/无进展的**代理指标**，粗糙且错配：一个合法的大型重构可能需要数百回合，一个三回合的死循环才是失控。惩罚工作量而非失控行为，是本次事故"50 回合暴毙"的制度根源；
- codex 无此维度，生产验证可行；loom 现状更糟——`MaxRepeatedActions`/`MaxParallelTools` 本就无消费方（死配置），数回合是唯一防线。删它的前提是**把真正的失控检测建立起来**：

| 检测 | 规则 | 动作 |
|------|------|------|
| 重复调用 | 同一 `(tool, args_hash)` 连续出现 ≥3 次（**含 prepare 失败**，口径见下） | 第 2 次注入警告（"相同调用已失败/执行过，请换思路"），第 3 次终止 run（`OutcomeFailed`，指明死循环原因） |
| 连续失败 | **执行期**失败（不含 prepare 失败）连续 ≥5 次且无一次成功 | 终止 run（`OutcomeFailed`） |
| 无进展 | 连续 N=10 个回合无任何进展信号（见下） | 注入收敛提醒（**不终止**——漫游用提醒纠正；可配置关闭） |

口径规定（评审 M4/M5 修正）：

- **重复调用的 hash 口径**：prepare 成功用 `PreparedCall.ArgsHash`；prepare 失败时该值不存在，用规范化后的原始 `Arguments` 的 hash（与 ArgsHash 同一规范化函数）。已知漏报：`__malformed_arguments` 占位对超长 raw 截断 2KB，两次"近似相同"的超长 malformed 调用 hash 不同——接受该漏报（malformed 场景已有明确错误提示驱动模型自我纠正）；
- **prepare 失败只进"重复调用"检测，不进"连续失败"终止**——挣扎修复 schema 的模型需要重试空间；连续失败只统计真正被执行后失败的调用；
- **无进展的"进展信号"包含信息获取**（评审 M4）：新读取的文件路径、新工具输出字节数（去重后）、新 artifact、文件写入、plan 推进、可见文本输出，任一即为进展。只读调研型任务（本次事故类型）天然持续产生进展信号，不会误报；纯粹的"原地打转"（重复读同一文件）同时被"重复调用"检测覆盖，两个检测正交。

**v3：删除 wall_time 预算，建立会话级 token 预算**（动机：会话 `sess_a49f833ae4b228beb17353b49ae016c0` 实测复盘，取代评审 M6 的取舍）：

- **删除 `MaxWallTime`**。实测证明 per-prompt 累计墙钟是错误的护栏：(a) 口径污染——`awaiting_approval` 与用户输入等待全部计入，复盘中审批等待占会话时长 35%，单次挂起 24 分钟直接耗爆 30 分钟预算；(b) 阈值失效——挂起期间无 loop 迭代评估档位，恢复时越过全部提醒档位直接软着陆（35m10s/30m0s 才发出第一条 notice）；(c) 误杀长程任务——goal 机制承诺跨 turn、跨压缩自动续跑，wall_time 在 per-prompt 维度 30 分钟砍断它，与原则 4「禁止代理指标惩罚工作量」自相矛盾（累计墙钟与回合数同为代理指标）；(d) codex 无此维度（§1.3），生产验证可行。
- **新增会话级 token 预算 `MaxTokens`**：口径为会话内全部模型调用的实测 `input + output` token 累计（复用 goal `TokensUsed` 的跨 prompt 累计先例——原 M6/§9.1 的二期项提前落地）；**默认 0 = 不限**（opt-in，与 codex `token_budget` 哲学一致：长程任务默认不受资源维度打扰）；命中走 4.4.2 软着陆（dimension=`tokens`）。网关返回缓存命中数后切换加权计量（§9.3）。
- **usage 口径拆分**：`Usage.InputTokens/OutputTokens/CostUSD` 改为**会话级累计**（checkpoint 持久，`ContinueRun` 继承，崩溃恢复语义天然一致）；`Usage.Turns/ToolCalls/WallTime` 维持 per-prompt 观测计数（`ResetUsageForNewTurn` 仅重置这三者；`WallTime` 保留为纯展示字段，不再是预算维度）。
- **`MaxEstimatedCostUSD` 同步改为会话级累计**（默认值 5.0 保留；未配定价时 `CostUSD` 恒 0、维度失明的现状不变——tokens 维度成为成本失明时的主护栏）。

**停滞看门狗 `stall_timeout`**（时间维度卸下预算职责后的唯一角色，属失控检测而非预算）：

| 检测 | 规则 | 动作 |
|------|------|------|
| 停滞（v3 新增） | 距上次进展信号的**活跃时长** ≥ `stall_timeout`（默认 15m，0=禁用）；进展信号与「无进展」检测同口径 | 走 4.4.2 软着陆（dimension=`stall`），终态 `OutcomeFailed` |

口径规定：

- **计时只累计 agent 活跃时长**：`awaiting_approval` 挂起与用户输入等待期间暂停计时（恢复执行时将挂起时长补偿进基准）——v2 复盘的核心教训就是「等待不该消耗任何预算」；
- 判定位置为 `prepare`（与其他检查一致，pairing-safe）；长命令执行、慢模型响应属于活跃时间，正常计入（15m 默认值对此留有充足余量）；
- 与 `stall_warn_turns` 正交互补：turns 维度管「漫游」（有产出但无进展，提醒纠正），timeout 维度管「卡死」（长时间零进展，软着陆终止）；
- 防不住的形态（诚实声明）：持续产生「假进展」的跑偏（不断读新文件但方向错误）不在本检测范围，由 stall_warn 提醒 + tokens 预算 + 用户中断兜底。

`MaxParallelTools` 同样无消费方，一并删除字段（当前 `executeTools` 为串行执行，不存在并发调度；未来引入并发时以内部常量管理，不属于用户预算语义）。

### 4.5 工具输出：入库即截断

对齐 codex 的 `record_items` 截断哲学，把压力挡在 transcript 之外：

- `MaxToolOutputBytes` 默认值 16KB → **48KB**（≈12k tokens，与 codex 的 per-model 10k tokens 对齐）；
- **统一收口**（评审附录 A 确认现状割裂）：`MaxToolOutputBytes` 目前仅 run_cmd 消费，read_file/grep/list_dir 各有独立上限。截断统一上收到工具结果入库处（`routeToolCalls`/结果修剪处），各工具的独立上限保留为领域性约束（如 read_file 的分页语义），但**入库前的最终截断只有一处**；
- 超限输出的截断方式统一为**头尾保留**（各半），头部注入一行警告：
  `Warning: output truncated (original 96.2KB / ~24k tokens, showing first+last 24KB). Full output: <artifact path 或 "unavailable">`；
- run_cmd 等大输出工具沿用现有 artifact 全量落盘（不变），截断文本中**必须附带 artifact 绝对路径**（评审确认现状：模型可见 payload 只有 `ArtifactRef{id,size}` 无路径，截断标记无指针——必须补齐，否则"外部化→无法回读→重新执行"循环照旧）；
- **与压缩管线的分工**（评审 m8）：入库截断（48KB）管"单条输出的体量"，Level 1 mask（`MaskMinBytes`）管"窗口外历史输出的总量"，两者正交，mask 保留。但 `MaskMinBytes` 从 4KB 上调到 16KB：4KB 级输出存量小、外部化收益低，且事故的病理正是"中等输出被 mask → 重读"。16KB 以下输出信任 4.3 的窗口化触发线在总体层面兜底。

### 4.6 事件流一致性修复（配套）

本次事故暴露的事件流缺陷随本设计一并修复：

1. **prepare_failed 补齐前置事件**：工具参数解码失败（prepare_failed）时，先落 `tool.call_prepared` + `tool.execution_started`，再落 `result_added` + `execution_completed`，消除"有完成无开始"的孤儿事件。payload 降级形态（评审 m14）：prepare 失败时 `ArgsHash`/`ReadPaths`/`ApprovalDesc` 等审计字段不存在，`tool.call_prepared` 以 `{"call_id","tool","prepare_failed":true,"args_raw_hash","args_summary"}` 的降级结构落事件（`args_raw_hash` 同时供 4.4.3 的重复调用检测使用；`args_summary` 为白名单非敏感字符串参数（path/pattern/working_dir/repo_root 等）的截断摘要，便于直接从事件日志定位失败入参，完整原始参数可能含文件内容或密钥，永不落盘）；
2. **`__malformed_arguments` 错误消息**：prepare 阶段检测到该占位结构时，提取内嵌 `error` 提示字段作为错误消息返回给模型，不再把内部字段名 `json: unknown field "__malformed_arguments"` 透传给模型。

## 5. 对现有代码的改造点

| 文件 | 改动 |
|------|------|
| `internal/agent/window.go`（新增） | `WindowModel` 及阈值推导、配置解析、启动期校验（含"默认配置必须通过自身校验"回归） |
| `internal/agent/compact.go` | `Condenser` 持 `WindowModel`；删 `TargetTokens`；`MaskMinBytes` 4KB→16KB；L3 超限重试；用户消息预算窗口化 |
| `internal/agent/run.go` | `shouldCompact` 废除估算判据；`maybeInjectBudgetNotice` → 全维度梯度提醒（含 `touchWallTime`）；循环顶部硬检查改为 `PhasePreparing` 判定 + `wrapUpPending` 抑制；软着陆状态机（含自动 deny、崩溃恢复）；压缩后 UI warning 钩子；预算耗尽 stale 文案（"raise via LOOM_MAX_*"，指向已删除机制）更新；**v3**：wall_time 维度移除、`touchWallTime` 预算职责删除；`ResetUsageForNewTurn` 仅重置 per-prompt 观测计数，tokens/cost 会话累计从 checkpoint 继承 |
| `internal/agent/runaway.go`（新增） | 失控行为检测：重复调用 / 连续失败（执行期）/ 无进展计数；**v3**：新增 `stall_timeout` 停滞看门狗（活跃时长口径，挂起暂停计时），命中走软着陆（dimension=`stall`，`OutcomeFailed`） |
| `internal/agent/budget.go` | **v3**：`dimensionWallTime` 删除，`dimensionTokens`/`dimensionStall` 新增；tokens/cost notice 文案改会话累计口径 |
| `internal/agent/goal.go` | goal wrap-up 并入统一软着陆状态机 |
| `internal/domain/limits.go` | 删除 `MaxTurns`/`MaxToolCalls`/`MaxParallelTools`/`MaxRepeatedActions`（死配置）字段；`Check()` 同步裁剪为 wall_time/cost/occupancy 维度（`CheckBudget` 软告警正式接入 4.4.1）；`CheckRunaway` 只保留 wall_time/cost；`DefaultLimits`：MaxToolOutputBytes=48KB；**v3**：删除 `MaxWallTime`，新增 `MaxTokens`（默认 0=不限）；`Check()` 预算维度改为 tokens/cost |
| `internal/domain/events.go` | `context.compacted` payload 扩充；新增 `budget.notice` / `budget.wrapup_started` 事件；`tool.call_prepared` 降级 payload |
| `internal/agent/stream_hooks.go` | `__malformed_arguments` 错误消息提取 |
| `internal/app/bootstrap.go` 等 | 工具结果统一截断收口；artifact 路径附带 |
| `internal/config` | 新增 `context:` / `runaway:` 配置节（见 §6）；删除 `limits.max_turns` 等 schema 字段；**v3**：`limits.max_wall_time` → `limits.max_tokens`，`runaway.stall_timeout` 新增（KnownFields 硬校验，旧字段出现即启动报错） |
| `internal/ui` | 压缩提示横幅；`budget.notice` 渲染；状态栏用量告警色从"累计 input/MaxInputTokens"（长会话常红，违背原则 4）改为 occupancy/Effective 驱动（`Usage.Turns/ToolCalls` 字段保留作展示，不删） |

迁移说明（评审 m5/m6）：checkpoint 不含 `Limits`（仅 Usage），sqlite 存量数据无反序列化问题；配置加载 `KnownFields(true)` 使"已删除字段出现即报错"天然成立，与 CONFIG_DESIGN"开发期无历史用户，breaking 一次性做净"的政策自洽——影响面仅限作者本机 `~/.loom/config.yaml`。

## 6. 配置

```yaml
context:
  utilization: 0.95            # 有效窗口 = context_window × utilization
  compact_trigger_ratio: 0.80  # 自动压缩触发线（相对有效窗口）
  compact_target_ratio: 0.50   # 压缩目标（相对有效窗口）
  notice_levels: [0.60, 0.75]  # 梯度提醒档位（相对有效窗口，必须全部 < trigger）

limits:
  max_tokens: 0                # 会话级累计 token 预算（0 = 不限，opt-in；v3 替代 max_wall_time）
  max_cost_usd: 5.0            # 会话级累计成本上限（需配置定价后生效）
  max_tool_output_bytes: 49152
  # max_turns / max_tool_calls / max_parallel_tools / max_repeated_actions / max_wall_time 已删除；
  # 配置中出现即启动报错（KnownFields 硬校验，无静默忽略）

runaway:                       # 失控行为检测（非预算）
  max_repeated_calls: 3        # 同一 (tool, args_hash) 连续上限
  max_consecutive_failures: 5  # 执行期连续失败上限
  stall_warn_turns: 10         # 无进展提醒阈值（0 = 关闭）
  stall_timeout: 15m           # 停滞看门狗：距上次进展的活跃时长上限（0 = 禁用；挂起/等待不计时）
```

- 模型级覆盖：`providers[].models[].window_utilization`；
- 配置路径只有 `config.Load` 一条（沿用 CONFIG_DESIGN 既定决策：**无 env 覆盖层**，`LOOM_*` 配置 env 已整体删除）；
- 校验：`target_ratio < trigger_ratio`、`notice_levels` 严格升序且全部 < `trigger_ratio`、`0 < utilization ≤ 1` —— 启动 fail-fast。

## 7. 事件与可观测性

| 事件 | 变化 |
|------|------|
| `context.compacted` | 增加 `trigger`/`phase`/`occupancy_before`/`occupancy_after`/`truncated_user_messages` |
| `budget.notice`（新增） | `dimension`/`level`/`usage`/`limit` |
| `budget.wrapup_started`（新增） | `dimension`/`usage`/`limit`；崩溃恢复的重置依据；**v3**：dimension 取值 `tokens`/`cost_usd`/`stall`/`goal_tokens`（`wall_time` 废弃） |
| `run.completed` | 软着陆路径的 `budget_exhausted` 必有一条最终助手消息先行；**v3**：stall 触发的软着陆以 `failed`（附 stall 原因）终结，同样必有最终助手消息先行 |
| trace（Langfuse） | 压缩 span 记录 trigger 与 occupancy；notice/wrap-up 注入记录为 event |

状态栏：`ctx:~N/W` 口径统一为 occupancy/Effective；告警色由 notice 档位驱动（弃用累计 input/MaxInputTokens 口径）。**v3**：token 用量展示改会话累计值（未配预算时 `tok:~N`，配预算时 `tok:~N/M`）；wall-time 保留 per-prompt 计时展示，纯信息性。

## 8. 测试计划

1. **WindowModel 推导**：比例边界、无 context_window 回退、非法配置 fail-fast、**默认配置通过自身校验**（B1 回归）；
2. **触发**：实测 occupancy 越过 trigger 才压缩；纯估算增长（无实测）不误触发（回归：本次事故的 32k 误压缩场景）；
3. **软着陆状态机**：tokens/cost 硬命中 → 恰好一次 wrap-up → `budget_exhausted` 且有最终助手消息；stall 命中 → wrap-up → `failed`（附原因）且有最终助手消息；非 `PhasePreparing` 命中时先完成 phase 迁移；wrap-up 中发起工具调用 → 自动 deny（不经审批）+ 配对完整 + 强制终止；**崩溃恢复**：`budget.wrapup_started` 后崩溃 → RecoverRun 重置位继续软着陆；**会话累计**：`ContinueRun` 后 tokens/cost 从 checkpoint 继承、per-prompt 观测计数清零；
4. **梯度提醒**：各维度各档每 prompt 恰好一次；压缩后 occupancy 档重置、其他维度不重置；notice 档位低于 trigger 的生命周期走查（B1 回归：两档都发得出）；
5. **失控检测**：重复调用（含 prepare 失败、hash 口径）第 2 次警告第 3 次终止；连续失败只计执行期；只读调研任务 10 回合不误报无进展（M4 回归）；纯原地打转被提醒；**停滞看门狗**：持续活跃无进展超 `stall_timeout` → 软着陆 + `failed`；`awaiting_approval` 挂起时长不计入（回归：v2 复盘中 24 分钟审批等待不得误触发）；有进展信号则计时重置；
6. **ModelDownshift**：切换小窗口模型后下一 turn 自然触发压缩，事件 `trigger=downshift`；
7. **L3 降级**：summarize 连续 overflow 的逐条丢弃与最终兜底；
8. **截断**：48KB 头尾保留、警告文案、artifact 路径附带；统一收口后各工具输出均经过同一截断点；
9. **事件流**：prepare_failed 事件序列完整配对（含降级 payload）；`__malformed_arguments` 的模型可见错误为内嵌提示文案；
10. **e2e**：以事故会话为原型构造 200k 窗口核验任务，断言：零压缩或 ≤1 次压缩、无重读、正常 `succeeded`；另构造 >50 回合的合法长任务，断言不被任何"回合上限"终止（回归：删除 MaxTurns 后长任务可完成）。

## 9. 开放问题

1. ~~会话级累计预算~~ **已于 v3 落地**（§4.4.3：`MaxTokens` 会话级累计、cost 同步会话级、`MaxWallTime` 删除；tokens 默认 0=不限，opt-in）。
2. **跨窗口 notes**：codex token-budget 模式连总结都不做（直接开新窗口 + 持久 notes）。loom 已有 plan/goal Cell 外置状态，是否再进一步提供模型可写的跨压缩 notes（`new_context` 式工具）？二期评估；本设计先保留 L3 总结（loom 的 provider 均为第三方网关，无服务端压缩可用，总结是保真的唯一手段）。
3. **加权 token 计量**：codex 按 output×weight + 非缓存 input×weight 计费。glm 网关是否返回缓存命中数待确认；若返回，cost 维度改加权计量（独立小改动，不阻塞本设计）。
4. **`estTokens` 对 reasoning 的口径**：当前把 reasoning 全文计入（保守）。压缩后 occupancy 估算可能显著高估（provider 对 reasoning 的重放计费不透明）。维持保守口径，观察实测校准后的偏差再定。
