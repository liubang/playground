# Loom Surface/Log 分离设计

> 状态：**已全部实施**（2026-08-14；M1-M4 均经独立审查+真实环境验收；bazel 45/45 通过。M1 审查修正 2 major + 5 minor；M2 审查修正 2 major——budget.notice/goal.updated 投影缺口为顺带修复的预存 bug；M3/M4 审查修正 1 major（消息附件引用未入纯 log 重建路径）+ 2 minor。真实环境验收：glm-5.2 小窗口会话产生 context.masked/context.summarized 指令事件与 initial/resume 双 reason 的 model.request_header，request_started 全量锚定，inspect/resume/gc 均正常）
> 设计修订记录：v2 首轮自审修正 4 处（双批 mask 合并安全性论证、Level 3 不吞前级指令、投影器 nextSequence 重建、mask 与 normalizeMessage 修订语义对齐）；v3 二轮自审+ dsh 对照修正 4 处（SurfaceOps 包归属下移 domain、归档定位归纳不变量显式化、surface/wire 边界澄清、引入运行时不变量自检；并将 request/header 持久化纳入范围 §4.8）；实施期修正 2 处（revision 规则放宽为同层等值——运行时每条消息每层只 bump 一次；archive 允许零 artifact 承载 summary-overflow 的无保留 drop）
> 作者：liubang
> 日期：2026-08-13
> 参考：deepseek-harness（`packages/core/session`）的 "Model-visible ⟺ Logged" 原则与 surface/log 双层事件模型；docs/CONTEXT_DESIGN.md（压缩三级管线）

## 1. 背景与问题

### 1.1 概念定义

- **Log（事件日志）**：`events` 表中的 append-only 事件流，是会话的唯一事实源。消息经由三类事件入日志：`user.message_added`、`model.response_completed`、`tool.result_added`（`internal/domain/event.go`）。
- **Surface（模型可见面）**：某次模型调用时，模型实际看到的 transcript。它是 log 的派生视图——可能被压缩改写过的那一版。

设计目标是让两者满足 **"Model-visible ⟺ Logged"**：任何到达模型请求的内容，必须能从 log 重建；log 中的任何内容，都有明确的 surface 归宿（可见、被遮蔽、或被归档）。

**Surface 与 wire 的边界**：surface 中的图片以 `PartArtifact` 引用存在，模型实际看到的是 egress 处 materialize（vision 模型内联）或 strip（非 vision 模型）后的结果。wire 转换是 (surface, 模型能力) 的纯函数——surface 确定、模型确定（§4.8 的 request/header 持久化模型名），则 wire 输入确定。因此"模型可见"的可重建性在 surface 层成立即可，无需把 wire 形态落盘。

### 1.2 现状：压缩结果不在 log 里

当前压缩（`Loop.compact` → `Condenser.Condense`，`internal/agent/compact.go`）直接**原地改写内存中的 `Run.Messages`**：

- Level 1/2b：`maskMessageOutputs` 把超大工具输出替换为占位符，原文外化到 artifact store；
- Level 2a：`archiveOldestSpan` 把最老一段消息整体替换为 marker 消息，原文外化；
- Level 3：`buildSummaryReplacement` 用 LLM 摘要重建整个 transcript。

改写完成后，只向 log 追加一条 `context.compacted` 审计事件（`contextCompactedPayload`：trigger、条数、字节数、occupancy 前后值）。**压缩后的 surface 本身不进入 log**，唯一的载体是 checkpoint 的 `Messages` 字段（随 `AppendEventsAndCheckpoint` 原子落盘）。

### 1.3 由此产生的四个问题

**P1. 纯 log 回放与模型实际所见不一致。**
`session.Replay(events)`（`internal/session/transcript.go:45`）从事件重建 transcript，但事件里没有压缩指令，重建出的是**未压缩**的原始 transcript。`InspectSession`（`internal/session/sqlite_store.go:696`）在有 checkpoint 时走 `ReplayFromCheckpoint`（checkpoint 的压缩后消息 + 后续事件），无 checkpoint 时走 `Replay`——**同一个会话，两种入口得到的 transcript 语义不同**：一个是 surface，一个是全量原始记录，但两者共用一个类型、一个名字，调用方无法区分。

**P2. Checkpoint 从"性能优化"异化为"正确性依赖"。**
事件溯源架构里 checkpoint 应当是"可随意丢弃重建"的加速结构。但当前压缩结果只在 checkpoint 里，**丢掉 checkpoint 就丢掉了 surface**——log 无法重建它。这架空了事件溯源的核心承诺，也让 `RecoverRun` 必须信任 checkpoint 内容与 log 的一致性（目前没有任何交叉校验）。

**P3. 无法回答"第 N 次模型调用时模型看到了什么"。**
调试"模型为什么那样回答"需要复现当时的输入。`model.request_started` 事件只记录请求元数据，transcript 内容要靠回放推得；而回放只能给出未压缩版本。压缩发生得越多，log 与真实模型输入的偏差越大，审计价值越低。

**P4. 压缩行为不可重放，压缩逻辑的 bug 无法事后复现。**
`context.compacted` 只记录了压缩的"结果统计"，没有记录"决策内容"（mask 了哪条消息的哪个 part、归档了哪个区间、摘要替换了哪些消息）。一旦压缩产生坏 surface（如历史上出现过的 sequence 冲突，见 `transcript.go` 中 `repairCheckpointSequences` 的修复逻辑），事后无法从 log 还原当时的决策链。

### 1.4 deepseek-harness 的对照做法

dsh 的 session 事件分两层：

| 层 | 内容 | 性质 |
|----|------|------|
| Log-only 事件 | `assistant/chunk`、`step/start`、`request/header` 等 | 参与持久化与回放，不出现在模型可见面 |
| Surface 事件 | `user/message`、`assistant/message`、`tool/result` | 出现在模型可见的有序 surface 上 |

压缩是 surface 上的 **`replace` 操作**（surface op）：把一段 surface 区间原子替换为一个摘要节点。Log 永远不动、永远保真；surface 只是 log 之上一棵可改写的视图。回放保真度与压缩互不干扰。

Loom 不需要照搬其分层命名（loom 的消息事件天然就是 surface 事件），需要吸收的核心是：**压缩决策作为事件持久化，surface 成为 log 的纯函数**。

## 2. 设计原则

1. **Log 只增不改，全保真**。已成立，本设计将其固化并补齐最后一块缺口（压缩）。任何消息事件入 log 后永不改写、永不删除。
2. **Surface 是 log 的纯函数**。`surface = fold(events)`。给定相同事件序列，任何进程、任何时刻回放出的 surface 逐字节一致。
3. **压缩是数据，不是行为**。每次压缩的全部决策（遮蔽了哪个 part、归档了哪个区间、摘要替换内容）作为事件持久化；运行时应用与回放应用使用**同一份数据、同一段应用代码**，从构造上杜绝漂移。
4. **Checkpoint 降级为纯缓存**。任何 checkpoint 都可丢弃、可用 log 重建、可用 log 校验。发现不一致时以 log 为准。
5. **配对不变量不下放**。tool_call ↔ tool_result 配对、dense sequence、revision 单调等不变量，由指令生成侧（Condenser）保证，投影器只做机械应用与校验，不重复实现配对逻辑。

## 3. 总体架构

```
                 ┌──────────────────────────── 运行时 ───────────────────────────┐
                 │                                                                │
  Condenser ──── │ ──► SurfaceOps（压缩指令，纯数据）──► apply(surface) ──► Run.Messages
  （只产指令，     │                     │                                      │
   不改消息）      │                     ▼                                      │
                 │              appendEvent（同一事务落盘）                       │
                 └───────────────────────────┬─────────────────────────────────┘
                                             │
                 ┌───────────────────────────▼─────────────────────────────────┐
                 │ events 表（append-only）                                      │
                 │   消息事件：user.message_added / model.response_completed /   │
                 │            tool.result_added                                 │
                 │   指令事件：context.masked / context.archived /              │
                 │            context.summarized（本设计新增）                    │
                 │   请求头事件：model.request_header（本设计新增，§4.8）          │
                 │   审计事件：context.compacted（保留，不变）                    │
                 └───────────────────────────┬─────────────────────────────────┘
                                             │
                 ┌───────────────────────────▼─────────────────────────────────┐
                 │ Projector（回放）                                            │
                 │   消息事件 → 追加/修订消息                                    │
                 │   指令事件 → apply（与运行时同一函数）                         │
                 │   输出 = surface（模型当时所见）                               │
                 └─────────────────────────────────────────────────────────────┘
```

关键转变：**Condenser 从"改写 transcript 的函数"变成"生成压缩指令的纯函数"**。指令（SurfaceOps）是唯一被生成一次的东西；内存 surface 的更新与 log 的持久化都来自它，回放时再应用一遍。三处消费，一处生成。

## 4. 详细设计

### 4.1 指令事件模型

新增三个事件类型（加入 `EventType` 常量与 `Event.Validate` 白名单）：

```go
const (
    EventContextMasked     EventType = "context.masked"      // Level 1 / 2b
    EventContextArchived   EventType = "context.archived"    // Level 2a
    EventContextSummarized EventType = "context.summarized"  // Level 3
)
```

#### 4.1.1 `context.masked`（Level 1 / 2b 观测遮蔽）

```go
// MaskedPart 定位一次外化：哪条消息的哪个 tool result 的哪个 content。
type MaskedPart struct {
    MessageID    domain.MessageID   `json:"message_id"`
    PartIndex    int                `json:"part_index"`
    ContentIndex int                `json:"content_index"`
    OriginalBytes int               `json:"original_bytes"`
    Artifact     domain.ArtifactRef `json:"artifact"`
    // Placeholder 是替换后的完整占位文本（含可读路径），生成一次，
    // 运行时与回放共用，避免投影器重复依赖 artifactPathResolver。
    Placeholder  string             `json:"placeholder"`
}

type ContextMaskedPayload struct {
    Masks []MaskedPart `json:"masks"`
}
```

应用语义：定位消息 → 将 `Parts[PartIndex].ToolResult.Content[ContentIndex].Text` 替换为 `Placeholder` → 追加 `PartArtifact` → `Revision++`。与 `maskMessageOutputs` 的运行时效果逐字节一致。

一次 `Condense` 可能产生两批 mask（Level 1 与 Level 2b 分开调用 `maskRange`）：合并为一个 `context.masked` 事件是安全的，因为 mask 按 `MessageID + PartIndex + ContentIndex` 定位，与列表顺序及 archive 是否发生无关；同一 content 至多出现一次（Level 2b 重扫时被 placeholder 前缀跳过）。应用顺序上 `context.masked` 恒先于 `context.archived`——archive 可能删除已被 mask 的消息，先应用 mask 保证定位时消息仍在。

#### 4.1.2 `context.archived`（Level 2a 区间归档）

```go
type ContextArchivedPayload struct {
    // [FromSequence, ToSequence] 是被归档的消息区间（含两端），
    // 以压缩前的 message.Sequence 定位。
    FromSequence int64 `json:"from_sequence"`
    ToSequence   int64 `json:"to_sequence"`
    Artifact     domain.ArtifactRef `json:"artifact"`
    // Marker 是完整的替换消息（含 metadata 中的 artifact 引用清单）。
    // 事件携带完整消息而非参数，投影器零计算直接插入。
    Marker domain.Message `json:"marker"`
}
```

应用语义：删除区间内消息 → 插入 Marker → dense renumber（见 §4.3）。

**定位有效性的归纳不变量**：sequence 区间以压缩时的 surface 编号定位。回放能正确应用它的前提是：回放应用过的指令前缀与运行时完全一致（归纳可得——两边消费同一事件序列、同一应用函数），因此压缩时投影器表面的编号与运行时编号相同。任一指令应用失败都会立即报错终止回放，不会产生静默错位。

#### 4.1.3 `context.summarized`（Level 3 摘要替换）

```go
type ContextSummarizedPayload struct {
    // Replacement 是 buildSummaryReplacement 的完整产物（逐字保留的
    // 用户消息 + 摘要桥接消息），携带全套 ID/Parts/Metadata。
    Replacement []domain.Message `json:"replacement"`
    // DroppedUserMessages 记录因预算不足被丢弃的用户消息数（审计）。
    DroppedUserMessages int `json:"dropped_user_messages,omitempty"`
}
```

应用语义：整个 surface 替换为 Replacement → dense renumber。Level 3 总是整面替换，无需区间定位。

#### 4.1.4 为什么指令携带完整产物而不是参数

备选方案是事件只携带参数（区间、阈值），投影器重跑压缩算法。否决理由：

1. Level 3 依赖 LLM 输出，重跑不可复现；
2. 重跑要求投影器持有 artifact store 与 path resolver，把运行时依赖泄入只读回放路径；
3. 压缩算法演进后，旧事件用新算法重放会产生不同的 surface，破坏 §2 原则 2。

"生成一次、处处应用"用 log 体积（placeholder/marker/replacement 的冗余）换取了确定性与投影器的纯粹性。冗余量级：mask 事件每次几十至几百字节；archived/summarized 事件携带的替换消息量级 KB——相对消息事件本身可忽略。

#### 4.1.5 与 `context.compacted` 审计事件的关系

保留不变。`context.compacted` 继续记录 trigger、phase、occupancy 前后值等聚合统计，供 UI 与预算逻辑消费；三个指令事件承载可重放的决策内容。一次压缩产生的事件序列：

```
context.masked?      → context.archived? → context.summarized? → context.compacted
```

与消息变更在同一个 `AppendEventsAndCheckpoint` 事务中原子落盘（沿用 `Run.appendEvent` + `flushEvents` 现状机制），不存在"指令写了、surface 没写"的中间态。

### 4.2 Condenser 改造：从改写到生成指令

**包归属**：`SurfaceOps`、三个指令 payload 与 `ApplySurfaceOps` 全部放在 `domain` 包（如 `domain/surface_ops.go`）——事件 payload 类型本就集中在 `domain/event.go`（`MessageEventPayload`、`BudgetNoticePayload` 等），且 `agent`（生成侧）与 `session`（回放侧）都依赖 `domain`，放 domain 不引入新的依赖边；放 agent 会让 session 反向依赖 agent，层级倒置。

```go
// SurfaceOps 是一次 Condense 产出的全部压缩指令（纯数据，字段即事件 payload，
// 发事件无需二次包装）。三个字段在一次压缩中可同时非空（Level 1 mask 后仍
// 超标 → archive；机械压缩仍不达标 → Level 3 摘要整面替换），按序全部应用：
// mask → archive → replacement。Level 3 发生时不吞掉前两级指令：mask/archive
// 的 artifact 外化与审计轨迹已真实发生，且回放按序应用后被 Replacement 整体
// 覆盖，结果与运行时一致。
type SurfaceOps struct {
    Masks       *ContextMaskedPayload
    Archive     *ContextArchivedPayload
    Replacement *ContextSummarizedPayload
}

// Plan 替代现在的 Condense：只读 messages，产出指令，不做任何修改。
// 审计计数（masked 字节数、归档条数等）由调用方从返回的 ops 派生，
// 不维护平行账本（终审修正，消除双账本回滚逻辑）。
func (c Condenser) Plan(ctx context.Context, messages []domain.Message, artifacts domain.ArtifactStore, now time.Time) domain.SurfaceOps
```

- `Plan` 内部仍按 Level 1 → 2a → 2b 的成本顺序决策，但每一步只**记录**指令（在 messages 的只读副本上演算后续级别所需的视图，或直接操作副本来计算下一步的输入——实现细节，对外契约是"不触碰入参"）；
- artifact 外化（`externalize`）仍在 `Plan` 内完成：它是压缩的输入准备（产出 ArtifactRef），不是 surface 修改；外化失败的处理策略不变（跳过该条，保住原文）；
- Level 3 的 LLM 调用（`summarizeForCompaction`）保留在 Loop 侧：`Plan` 返回"需要摘要"的信号，Loop 拿到摘要后构造 `Replacement` 并并入 SurfaceOps。Condenser 保持纯函数，不持有 Model。

### 4.3 共享应用函数：一处实现，三处消费

```go
// ApplySurfaceOps 把压缩指令应用到消息列表，返回应用后的新列表。
// 运行时（Loop）、回放（projector）、校验工具共用这一个函数。
func ApplySurfaceOps(messages []domain.Message, ops SurfaceOps) ([]domain.Message, error)
```

职责：

1. 按序应用 masks → archive → replacement；
2. 应用后执行 dense renumber（`Sequence = index+1`），与现行 `Condense` 末尾的重编号行为一致；
3. 校验不变量：定位有效性（MessageID/PartIndex 存在）、配对完整性（应用后不存在 dangling tool call）、sequence 严格递增；违例返回错误而非静默修复。

投影器（`internal/session/transcript.go` 的 `projector.applyEvent`）为三个新事件类型各加一个 case：解析 payload → 调 `ApplySurfaceOps` → 用返回的列表重建 `messages`/`messageByID`/`messageBySequence` 索引，并按存活消息的最大 sequence 重算 `nextSequence`。archived/summarized 会删除消息，旧索引中残留的 sequence 映射必须随列表重建，否则压缩后的新消息会与墓碑 sequence 冲突——这是现状 `applyMessage` 的 sequence 唯一性检查会在回放路径上踩到的坑。

mask 应用与 projector 现有的单消息修订语义（`normalizeMessage`：同 ID 的消息 Sequence/Role 不变、Revision 严格递增）天然兼容：每条被 mask 的消息正是一次 `Revision+1` 的修订。`ApplySurfaceOps` 内部对 mask 逐条复用该校验，而不是绕过它——回放路径上的指令应用与逐消息事件应用接受同一套不变量约束。

### 4.4 Loop 侧接线

`Loop.compact` 的新流程：

```go
ops, result := cond.Plan(ctx, l.Run.Messages, l.Artifacts)
if needsSummary { /* LLM 摘要，构造 ops.Replacement，含 overflow 重试现状逻辑 */ }
newMessages, err := ApplySurfaceOps(l.Run.Messages, ops)   // 与回放同一函数
if err != nil { /* 指令自校验失败：记 warn，放弃本次压缩，不阻塞 run */ }
l.Run.Messages = newMessages
l.Run.appendEvent(domain.EventContextMasked, ...)      // ops 非空才追加
l.Run.appendEvent(domain.EventContextArchived, ...)
l.Run.appendEvent(domain.EventContextSummarized, ...)
l.Run.appendEvent(domain.EventContextCompacted, ...)   // 审计事件，不变
```

行为兼容点：

- 压缩失败降级策略不变（摘要失败保留 masked 历史；指令应用失败则整次放弃）；
- `lastCompactEst`、`ForceCompact`、notice re-arm 等簿记不变；
- 崩溃安全不变：指令事件与 checkpoint 同事务，崩溃后要么都在要么都不在；`RecoverRun` 现有逻辑不感知压缩，无需改动。

**运行时不变量自检（借自 dsh invariant 机制）**：dsh 在每次请求前校验 "从 log 折叠出的 messages == 即将发送的 messages"。本设计引入等价的 strict 模式自检（`LOOM_STRICT_REPLAY=1` 或 debug 构建）：`callModel` 组装完 `effectiveMessages()` 后，用 `session.Replay`（内存事件副本）折叠出的 surface 与之做深度比较，不一致即 panic/log-fatal。常态关闭（有回放成本），CI 的 loop 测试全程开启——它把 §5.1 的黄金断言从"事后测试"升级为"每次请求时的在线校验"，能在压缩 bug 造成污染的第一现场暴露。

### 4.5 回放与消费方的语义变化

| 消费方 | 现状 | 本设计后 |
|--------|------|----------|
| `session.Replay(events)` | 未压缩原始 transcript | **surface**（模型当时所见）——语义修正 |
| `ReplayFromCheckpoint` | checkpoint surface + tail 事件 | 不变；tail 中的指令事件照常应用 |
| `InspectSession` | 两个入口语义不一致 | 两个入口产出同一 surface |
| 审计/调试 | 无法复现模型输入 | `Replay` 到任意 event sequence 即得当时 surface |

新增一个显式的全保真视图，供需要原始内容的场景（审计、导出、未来的记忆管线重提取）：

```go
// ReplayFull 忽略压缩指令事件，重建未压缩的全保真 transcript。
func ReplayFull(events []domain.Event) (Transcript, error)
```

实现上就是 projector 的一个开关（`applySurfaceOps bool`），默认 `Replay` 应用指令，`ReplayFull` 跳过。**命名即语义**：`Replay` 给"模型所见"，`ReplayFull` 给"事实全部"，消除 P1 的歧义。

### 4.6 GC 与 artifact 生命周期

现状：artifact 存活引用由 `checkpointArtifactRefs`（`sqlite_store.go:1046`）从 checkpoint 消息收集，rewind 后由 `recomputeArtifactRefs` 重算。

本设计下引用来源不变且更完备：

- mask 的 artifact：同时出现在指令事件 payload 与被遮蔽消息的 `PartArtifact` 中；
- archive 的 artifact：同时出现在指令事件 payload 与 marker 的 `MetadataCompactedArtifacts` 中（沿用现状约定）。

`checkpointArtifactRefs` 无需改动（checkpoint 里的 surface 消息仍带引用）。`backfillArtifactRefs` 与 rewind 重算路径补充一个规则：扫描到三类指令事件时，把 payload 中的 `ArtifactRef` 计入存活集合——这样即使 checkpoint 被丢弃，从纯 log 也能重建完整的引用图（呼应原则 4：checkpoint 可丢弃）。

**明确取舍：log 不瘦身。** 被 mask 的原始文本仍完整保存在 log 的 `tool.result_added` 事件中——这正是保真的含义。log 体积随会话单调增长；若未来需要瘦身，另行设计 log snapshot + truncate（属于存储层优化，与 surface 语义无关），不在本设计范围。

### 4.7 兼容与迁移

- **旧会话（无指令事件）**：行为与现状完全一致——checkpoint 是 surface 的唯一来源，`ReplayFromCheckpoint` 路径不变。新代码读旧会话无迁移成本。
- **旧二进制读新会话**：`Event.Validate` 对未知事件类型报错，旧版本 loom 打开含指令事件的会话会失败。接受此前向不兼容：事件类型扩展不需要 schema 变更（payload 是 BLOB），但要求版本升级先于使用。在 release note 中声明。
- **混合状态**：一次会话先由旧版本压缩（只有审计事件）、后由新版本压缩（有指令事件）——投影器对缺失指令的历史区间无法修正（信息已随旧 checkpoint 存在），行为退化为现状，不产生新错误。

### 4.8 请求头持久化（`model.request_header`）

Surface 解决了"transcript 可重建"，但一次模型请求还有另一半天：system prompt、tools schema、模型配置。现状只有 `model.request_started` 携带 `ManifestID`/`ManifestHash`/`PromptHash` 三个哈希（`modelRequestAuditPayload`），`PromptBuilder` 的注释明确写着 prompt "never persisted"——哈希能证明"变了"，不能回答"是什么"。本节把请求头全文纳入 log，彻底闭合 P3。

#### 4.8.1 事件与 payload

```go
// domain 包；EventModelRequestHeader EventType = "model.request_header"

type RequestHeaderReason string

const (
    HeaderReasonInitial RequestHeaderReason = "initial" // 会话首个 header
    HeaderReasonResume  RequestHeaderReason = "resume"  // 进程重启/恢复后首个 header
    HeaderReasonChange  RequestHeaderReason = "change"  // 任一组成部分变化
)

type RequestHeader struct {
    ModelName   string                  `json:"model_name"`
    Reasoning   domain.ReasoningSpec    `json:"reasoning,omitempty"`
    MaxTokens   int64                   `json:"max_tokens,omitempty"`
    Temperature float64                 `json:"temperature,omitempty"`
    // System 是渲染后的完整 system prompt 文本（static + dynamic 拼接后）。
    System      string                  `json:"system,omitempty"`
    // Tools 是本次请求暴露的完整工具 schema 列表。
    Tools       []domain.ToolDefinition `json:"tools,omitempty"`
    // Rules 是 context manifest 中稳定的规则集引用。manifest 的
    // per-request 部分（message ranges/budget buckets/truncations）
    // 每次调用都变，纳入会让去重失效——各次调用的完整 manifest
    // 哈希仍由 model.request_started 携带（实施期修正）。
    Rules       []domain.ContextRuleRef `json:"rules,omitempty"`
}

type RequestHeaderPayload struct {
    Header RequestHeader       `json:"header"`
    Reason RequestHeaderReason `json:"reason"`
    Hash   string              `json:"hash"` // header canonical JSON 的 SHA-256
}
```

#### 4.8.2 变更去重（借自 dsh 的 initial/resume/change 三态）

请求头全文每次调用都落盘不可接受（prompt + tools schema 量级几十 KB，一个会话上百次调用）。dsh 的做法是**按变更去重**：每次 `buildRequest` 组装出 header 后与基线比较，仅在首次、resume 后首次、或内容变化时追加事件。loom 采用同样策略：

- Loop 持有 `lastHeaderHash`；`callModel` 组装请求后计算 header canonical hash，相同则跳过；
- 恢复后的新 Loop 实例首个请求以 `resume` 记一条（即使内容与崩溃前相同——它为"之后的 `request_started` 引用哪个 header"建立锚点，回放侧无需跨进程记忆）；
- `model.request_started` 的 audit payload 增加 `header_hash` 字段（新增字段对旧事件格式后向兼容），把每次请求锚定到具体 header 版本——第 N 次调用模型看到了什么 = 最近的 `header_hash` 对应的全文 + 当时的 surface。

#### 4.8.3 体积估算与取舍

去重后，header 事件的触发点是：会话开始、resume、`/model` 切换、工具集增删（MCP 热载）、prompt 模板/skill/memory 变更。常态会话个位数。单条几十 KB，与一次大工具输出同级，可接受。**明确不采纳** sidecar/哈希引用方案（仅存哈希、正文外置 artifact store）：artifact GC 与会话生命周期不同步会带来悬空引用风险，且调试时要多一次跳转；全文入 log 换来的"单文件自包含可审计"价值更大。

#### 4.8.4 与回放测试的衔接

`model.request_header` 是 docs/REPLAY_TESTING_DESIGN.md 中请求指纹校验的数据来源：回放时 ReplayModel 可对每次调用重建 header 并与录制基线比较，检测"请求侧漂移"（prompt 模板变了、工具集变了）——这类漂移在纯位置匹配下是静默的。

**已知盲区（有意保留）**：压缩的 summarize 调用（`summarizeForCompaction`）直调 `Model.Stream`，不落 `request_started` 也不落 header——它无 tools、请求形态不同，落 header 会造成去重抖动。其输入可重建（当时的 surface + 固定 summon prompt），输出经 `context.summarized` 的 Replacement 持久化，仅有用量会计入 budget.updated。若未来需要完整审计这次调用，应加一条轻量审计事件而非复用 header。

## 5. 测试策略

### 5.1 黄金一致性性质测试（核心）

对 `run_test.go` 现有全部 loop 场景统一追加一条断言：

```go
// 不变量：纯 log 回放出的 surface 与运行终态的 Run.Messages 深度相等。
transcript, err := session.Replay(events)
assert.DeepEqual(transcript.Messages, run.Messages)
```

这是"Model-visible ⟺ Logged"的可执行形式。重点覆盖：多次压缩交错、Level 1/2a/2b/3 各自触发、压缩后继续对话、压缩后崩溃恢复。

### 5.2 崩溃窗口测试

- **flush 前崩溃**：指令事件先进入 `pendingEvents`，与 surface 变更一同在下一次 `flushEvents` 时同事务落盘。压缩完成后、flush 前崩溃：指令与 surface 变更一起丢失，恢复后一致地回退到压缩前状态，重放安全（压缩是幂等可重试的）；
- **flush 后崩溃**：指令事件与 checkpoint 同事务可见，`RecoverRun` 从 checkpoint + tail 事件恢复，tail 中的指令事件被正常应用，surface 与不崩溃一致；
- **事务原子性**：用存储层故障注入验证 `AppendEventsAndCheckpoint` 不出现"事件写了、checkpoint 没写"的半提交状态。

### 5.3 GC 测试

- 指令事件中的 artifact 引用在 checkpoint 丢弃后仍被 `backfillArtifactRefs` 计入存活；
- rewind 穿越压缩点后引用集合正确（扩展 `TestSQLiteStoreRewindSessionRecomputesArtifactRefs`）。

### 5.4 旧会话回归

无指令事件的 session fixture：`Replay`、`ReplayFromCheckpoint`、`RecoverRun` 行为逐字节不变。

### 5.5 不变量负例

构造非法指令（定位不存在的 MessageID、破坏配对的 archive 区间）：`ApplySurfaceOps` 必须报错，Loop 侧降级为放弃压缩，投影器侧报错带出事件 ID。

## 6. 非目标

- **chunk 级流式持久化**（dsh 的 `assistant/chunk` 事件）：loom 的 stream delta 只经 runtimeevent 实时分发不落盘。token 级回放保真是独立增强，可在本设计之上叠加（同为 log-only 事件），不在本期；回放测试改在 model 边界独立录制（见 docs/REPLAY_TESTING_DESIGN.md）。
- **log 瘦身 / snapshot + truncate**：见 §4.6。
- **UI 变更**：mask 占位符与 archived marker 的展示逻辑已存在，无需改动。

## 7. 里程碑

| 里程碑 | 内容 | 验收 |
|--------|------|------|
| M1 | 三个事件类型 + payload 定义 + `Event.Validate` 白名单 + `ApplySurfaceOps` 共享函数 + 单测 | §5.5 负例通过 |
| M2 | Condenser 改 `Plan` + Loop 接线 + projector 应用指令 + `ReplayFull` | §5.1 黄金断言在现有全部 loop 测试上通过 |
| M3 | GC/rewind 引用扫描扩展 + `InspectSession` 语义统一 + 旧会话回归 fixture | §5.3、§5.4 通过 |
| M4 | `model.request_header` 事件 + 变更去重 + `request_started` 锚定字段（与 M1-M3 正交，可并行） | header 去重单测 + resume 锚点测试通过 |

M2 是语义切换点，落地前需全量回归（含 e2e 的 compaction 与 recovery 场景）。
