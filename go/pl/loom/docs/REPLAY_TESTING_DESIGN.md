# Loom LLM 录制回放与 Snapshot 测试设计

> 状态：已落地 v3（2026-08-15；R1-R5 全部完成并通过真实模型录制 + keyless replay 验收；落地偏差见 §7 附注）
> 作者：liubang
> 日期：2026-08-14
> 参考：deepseek-harness（`packages/test-support/llm-replay`、`packages/test-support/acp-snapshot`）的录制回放与 snapshot 机制；docs/SURFACE_DESIGN.md §4.8（request/header 持久化）

## 1. 背景与问题

### 1.1 现状

**真实模型 e2e 门槛高、覆盖弱。** `e2e/serve_real_model_e2e_test.go` 等 6 个真实模型测试要求 `LOOM_E2E_LLM=1` + 本地 `~/.loom/config.yaml` 里有有效 key，否则 skip。后果：CI 永远不跑它们，真实 provider 的流式协议细节（reasoning signature 回放、tool call ID 冲突、畸形参数）只在开发者手动跑时才被验证。

**FakeModel 与真实流脱节。** `internal/fakes/fake_model.go` 的 `ScriptEntry` 是手写脚本：只支持 text/tool_calls/usage/error，**不支持 reasoning 事件流、reasoning signature、cached tokens、provider warning、stream error**。凡是涉及这些路径的代码（`StreamAggregator` 的 reasoning 聚合、ID 重写、signature 传递），单测无法触达。

**e2e 基建重复。** 每个真实模型测试各自重复 30+ 行的进程级组装（`NewProcessRuntime` + `NewWorkspaceBootstrap` + `Broker` + `SingletonWorkspaceService` + `client.NewInProc` + eventCollector），没有共享 harness。

**trace 不能当录制源。** `internal/trace` 的 `GenerationRecord` 只记录聚合后的最终消息，不记录流式 `ModelEvent` 序列，且是面向 Langfuse 的单向导出管道，不可逆。

### 1.2 目标

1. **录制一次，无 key 回放**：真实模型会话的流式响应落盘为 fixture，之后任何人、任何 CI 都能确定性重放完整 agent 循环。
2. **全保真事件流**：回放 `[]domain.ModelEvent` 原始序列（reasoning、signature、usage、cached tokens、warning、error 全保留），不经过 `ScriptEntry` 有损中间层。
3. **Snapshot 断言**：对归一化后的 runtime 事件流与持久化事件做黄金文件对比，行为变化以 diff 形式进入 code review。
4. **录制即测试**：record 模式本身就是真实模型验收测试（沿用 `LOOM_E2E_LLM=1` 语义）。

### 1.3 非目标

- 不做 LLM 输出的语义评估（eval 框架是另一个主题）；
- 不替代现有 FakeModel 单测——loop 内部逻辑的精细场景（特定相位交错、特定预算边界）仍由手写脚本驱动更直接；
- 不做 chunk 级持久化进 session log（见 SURFACE_DESIGN §6 非目标）——录制文件是测试资产，不是会话数据。

## 2. 总体设计

```
record 模式（LOOM_SNAPSHOT=record，需 LOOM_E2E_LLM=1 + key）
  Loop ──► RecordingModel ──► 真实 provider
                │
                └──► e2e/testdata/snapshots/<scenario>/calls.jsonl（逐 ModelEvent 落盘）
                     + events.expected.jsonl / transcript.expected.json（归一化黄金文件）

replay 模式（默认，无 key）
  Loop ──► ReplayModel ◄── calls.jsonl（按序回放 ModelEvent 流）
                │
                └──► 产出 runtime 事件 + session 事件 ──► 归一化 ──► 与黄金文件 diff
```

关键决策：**录制发生在 `domain.Model` 边界**（`Stream(ctx, req) (ModelStream, error)`），而不是 HTTP/SSE 层。理由：

- `ModelEvent` 是 vendor-neutral 的归一化形态，已有完整 JSON tag，直接序列化；
- 一次录制对 Anthropic 与 OpenAI 两套 wire 协议通用——fixture 与 provider 解耦，换协议实现不用重录；
- 接线点唯一且已有接口约束：`controller.go` 中 `provider.ModelFor(current.Model)` 的返回值包装一层即可，不动 `Loop`、`StreamAggregator` 与任何 provider。

## 3. 录制（RecordingModel）

### 3.1 接线

```go
// internal/model/replay/record.go

// RecordingModel 包装真实 Model，把每次调用的请求与事件流落盘。
type RecordingModel struct {
    inner domain.Model
    sink  *Recorder // 追加写 calls.jsonl
}

func (m *RecordingModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
    stream, err := m.inner.Stream(ctx, req)
    rec := m.sink.BeginCall(req) // 写 {"type":"call_start", request_fingerprint, request}
    if err != nil {
        rec.Error(err)           // 写 {"type":"call_error"}
        return nil, err
    }
    return &recordingStream{inner: stream, rec: rec}, nil // Recv() 逐事件透传并落盘
}
```

在 controller 构造 Loop 处按环境变量插入包装（`LOOM_SNAPSHOT=record` 时生效），生产路径零侵入。

### 3.2 录制格式（calls.jsonl）

每行一个 JSON 对象，按调用顺序追加：

```jsonl
{"type":"call_start","seq":1,"fingerprint":"sha256:...","request":{...完整 ModelRequest...}}
{"type":"event","kind":"response_start"}
{"type":"event","kind":"reasoning_start"}
{"type":"event","kind":"reasoning_delta","reasoning_delta":"..."}
{"type":"event","kind":"reasoning_end","reasoning_signature":"...","reasoning_redacted":false}
{"type":"event","kind":"tool_call_start","tool_index":0,"tool_id":"...","tool_name":"read_file"}
{"type":"event","kind":"tool_arguments_delta","tool_index":0,"tool_args":"{...}"}
{"type":"event","kind":"tool_call_end","tool_index":0}
{"type":"event","kind":"usage","input_tokens":1234,"output_tokens":56,"cached_input_tokens":1000}
{"type":"event","kind":"response_end","stop_reason":"tool_use"}
{"type":"call_end","seq":1}
{"type":"call_start","seq":2,...}
```

- `request` 完整落盘（含 messages/tools/reasoning/manifest）：用于调试与 §4.3 的指纹校验；它是测试资产，体积不敏感；
- `fingerprint` 是请求的规范化哈希（见 §4.3）；
- 取消场景的挂起语义用特殊行表示：`{"type":"call_hang","until":"cancelled"}`——回放时该调用阻塞直到 ctx 取消（对应 dsh 的 `hang` entry，用于 steering/cancel 场景 fixture）。

### 3.3 子代理会话

子代理（`delegate_task` / V2 async）共享进程内同一个 `domain.Model` 包装实例。**全局单一调用序列不可重放**：`delegate_task` 声明了 `ConcurrentSafe`，同一批次多个委派并行执行时子代理的调用相互交错；V2 异步子代理与父代理更是天然并发——交错顺序在录制与回放两次运行间不稳定。因此按会话分流录制：

- **绑定键**：子会话以**父 tool call ID** 为键（`calls.<call_id>.jsonl`），主会话以 `root` 为键（`calls.jsonl`）。父 tool call ID 记录在子会话的 `run.created` 事件里，且在回放模式下一定稳定——它来自录制的模型流（assistant 消息的 tool call 原样回放），不受并行交错影响。这比 dsh 的"按 sessionId 首次出现顺序绑定"更强：出现顺序在并行委派下本身就是不稳定的；
- **请求侧标识**：`ModelRequest` 目前不携带会话信息——record/replay 模式下经 `ctx` 注入（`replay.WithSessionRef(ctx, sessionID, parentCallID)`，Loop 在 `Execute` 入口设置，子代理 loop 同理），这是唯一的生产代码触点，且为测试模式专有路径。

## 4. 回放（ReplayModel）

### 4.1 按序消费（与 dsh 一致的纯位置匹配）

```go
// internal/model/replay/replay.go

type ReplayModel struct {
    scripts map[string]*Script // 绑定键（root / 父 tool call ID）→ 录制脚本
    // ...
}

func (m *ReplayModel) Stream(ctx context.Context, req domain.ModelRequest) (domain.ModelStream, error) {
    key := replay.BindingKeyFrom(ctx)   // root 或父 tool call ID
    script := m.bind(key)               // 按绑定键定位 calls.*.jsonl，确定性
    entry := script.next()              // cursor++，纯位置消费
    // script 耗尽 / 未录制会话 → 明确报错
    return entry.stream(ctx), nil       // call_hang 条目阻塞至 ctx 取消
}
```

**为什么纯位置匹配是安全的**（dsh 论证同样适用于 loom）：回放模式下输入完全确定——prompt 脚本固定、模型响应来自 fixture、工具结果由确定性 workspace 种子产生，因此第 K 次调用的请求必然与录制时一致。任何偏离（代码改动导致多调一次/少调一次/顺序变化）都会以 `script exhausted` 或 teardown 的 `unconsumed entries` 报错暴露。

### 4.2 不匹配检测

- **脚本耗尽**：调用次数超过录制 → `Stream` 返回明确错误；
- **未录制会话**：出现录制时没有的 session → 明确报错；
- **消费不足**：teardown `AssertConsumed()` 校验每个脚本被完整消费——agent 提前结束或改走不同路径会被发现（dsh 同款）；
- **挂起条目未取消**：`call_hang` 条目被正常关闭而非取消 → 报错（防止取消语义回归静默通过）。

### 4.3 请求指纹校验（loom 相对 dsh 的增强）

纯位置匹配有一种静默失败：**请求内容变了但次数没变**（prompt 模板改动、工具集增删、压缩行为变化），回放照样通过，snapshot 也只有间接体现。loom 利用 SURFACE_DESIGN §4.8 的 `model.request_header` 机制补上这层：

- 录制时 `fingerprint` = 请求头（model/reasoning/system/tools）+ messages 结构摘要的规范化哈希（剔除 message ID、时间戳等易变字段）；
- 回放时每次调用重建指纹与录制值比较：不一致默认**告警并继续**（`LOOM_REPLAY_STRICT=1` 升级为失败）——因为有些改动（如 prompt 文案微调）是合法的，fixture 不必重录，但 diff 应该在测试输出里可见；
- 这一层是 dsh 没有的。dsh 靠 pinning fixture（system-prompt.expected.md / tool-schemas.expected.json 单独快照）达到类似目的；loom 的指纹内联在 calls.jsonl 中，更轻但只报"变了"不报"变成什么"——prompt 全文 diff 由 §5 的 snapshot 黄金文件中的 header 事件承担（M4 落地后）。

### 4.4 与 FakeModel 的关系

保留、不合并。两者服务不同场景：

| | FakeModel（ScriptEntry） | ReplayModel |
|---|---|---|
| 场景 | loop 内部逻辑的精确场景构造 | 真实会话的端到端重放 |
| 数据源 | 手写 | 真实录制 |
| 保真度 | 有损（无 reasoning/signature） | 全保真 ModelEvent 流 |
| 位置 | `internal/fakes` | `internal/model/replay` |

## 5. Snapshot 黄金文件

### 5.1 断言对象与归一化

回放结束后产出两份归一化视图与黄金文件 diff：

1. **runtime 事件流**（broker 收集的 `RuntimeEvent` 序列）→ `events.expected.jsonl`
2. **持久化 transcript**（`InspectSession` 的 surface，SURFACE_DESIGN 落地后含指令事件）→ `transcript.expected.json`

归一化规则（对照 dsh normalize.ts，按 loom 数据结构调整）：

| 易变字段 | 归一化 |
|---|---|
| session/message/event/tool-call/artifact ID（UUID） | 按首次出现顺序重写为 `{{id:1}}`、`{{id:2}}`… |
| 时间戳 | `0` |
| workspace 绝对路径 | `{{cwd}}`（边界感知，避免误替换子串） |
| artifact 目录路径 | `{{artifacts}}` |
| loom home 路径 | `{{home}}` |
| 用量数字 | 保留（回放下确定，是有效断言） |

### 5.2 Fixture 布局

```
e2e/testdata/snapshots/<scenario>/
├── input.json                  # 驱动脚本：prompt/steer/approve/cancel 步骤序列
├── workspace/                  # 工作区种子文件（可选）
├── calls.jsonl                 # 主会话录制
├── calls.<call_id>.jsonl       # 子代理会话录制（按父 tool call ID 命名，可选多个）
├── events.expected.jsonl       # 归一化 runtime 事件黄金文件
└── transcript.expected.json    # 归一化 transcript 黄金文件
```

`input.json` 步骤经进程内 client 驱动（loom 没有 ACP 层，直接调 `client.InProc` 的 SubmitPrompt/Approve/Cancel）：

```json
{"steps": [
  {"op": "prompt", "text": "读一下 main.go 并总结"},
  {"op": "wait_turn_end"},
  {"op": "prompt", "text": "把总结写进 SUMMARY.md"},
  {"op": "approve", "decision": "allow"},
  {"op": "wait_turn_end"}
]}
```

### 5.3 三种模式

| 模式 | 触发 | 行为 |
|---|---|---|
| `replay`（默认） | 无环境变量 | 从 fixture 回放，diff 黄金文件，无需 key |
| `record` | `LOOM_SNAPSHOT=record` + `LOOM_E2E_LLM=1` | 打真实 API，重写 calls.jsonl 与黄金文件 |
| `refresh` | `LOOM_SNAPSHOT=refresh` | 回放运行，仅重写黄金文件（prompt 微调等合法漂移后的快速更新） |

record 模式复用现有真实模型测试的环境隔离约定（config 复制到 `t.TempDir()`，loom home 指向临时目录）。

### 5.4 共享 e2e harness

抽取现有重复的进程级组装为 `e2e/harness`（仅测试代码）：

```go
type Env struct {
    Client    *client.InProc
    Broker    *runtimeevent.Broker
    Collector *EventCollector
    Store     domain.SessionStore
    // ...
}

func NewEnv(t *testing.T, opts ...Option) *Env // config 隔离 + runtime 组装 + 清理
```

真实模型测试与 snapshot 测试共用；存量 6 个真实模型测试迁移到 harness（机械改动）。

## 6. 与 deepseek-harness 的对照与刻意差异

| 维度 | dsh | 本设计 | 差异理由 |
|---|---|---|---|
| 录制源 | 从 session log 的 `assistant/chunk` 事件**派生**回放脚本 | 在 Model 边界**独立录制** JSONL | loom 的 chunk 不落盘（SURFACE_DESIGN §6）；若未来引入 chunk 持久化，可切换为派生模式消除冗余 |
| 调用匹配 | 纯位置（cursor++） | 纯位置 + 请求指纹告警 | loom 没有 dsh 的 header pinning fixture 体系，指纹内联更轻（§4.3） |
| 子会话绑定 | 按 sessionId 首次出现顺序绑定脚本 | 按**父 tool call ID** 绑定 | 并行 delegate_task 下出现顺序本身不稳定；call ID 来自录制流，回放时恒定 |
| 归一化 | JSON-RPC stdout 帧 + session log | runtime 事件流 + domain 事件 | loom 无 ACP 层，断言对象是其自身两类事件 |
| refresh 稳定化 | normalized bijection 保留易变字段拼写 | 直接全量重写黄金文件 | loom 归一化已在比较前抹平易变字段，黄金文件不含平台相关拼写，无需 bijection 保留 |
| 挂起/取消 | `hang` entry + readyFile | `call_hang` 行 + ctx 取消 | loom 测试进程内运行，无需跨进程 readyFile 握手 |

## 7. 里程碑

| 里程碑 | 内容 | 验收 | 状态 |
|---|---|---|---|
| R1 | `internal/model/replay`：RecordingModel + ReplayModel + calls.jsonl 读写 + AssertConsumed | 录制/回放 round-trip 单测 | ✅ |
| R2 | e2e/harness 抽取 + 存量真实模型测试迁移 | 迁移后测试全绿 | ✅（8 个 LOOM_E2E_LLM 套件迁移） |
| R3 | ctx 绑定键注入（`WithSessionRef`） + 子会话分流录制/按父 call ID 绑定 | subagent e2e 的回放 fixture 通过 | ✅（`Loop.ParentToolCallID` + delegate/manager 两处注点） |
| R4 | snapshot 归一化 + 黄金文件 diff + 三模式 | 首批 3 个场景 fixture 入库（简单问答 / 工具循环 / 压缩触发） | ✅（4 场景：+ subagent） |
| R5 | 请求指纹校验（依赖 SURFACE_DESIGN M4 的 header 机制） | prompt 改动场景下指纹告警可见 | ✅（含字段级 drift diff；`LOOM_REPLAY_STRICT=1` 升级失败） |

依赖关系：R1-R4 独立于 SURFACE_DESIGN 可先行；R5 依赖其 M4。SURFACE_DESIGN M2 落地后，snapshot 场景库应扩充压缩/恢复类场景——那时回放 fixture 同时成为 surface/log 一致性的端到端验证（黄金文件中的 transcript 即 surface，指令事件在 log 中）。

### §7 附注：落地偏差（v3 实录）

1. **接线点上移**：RecordingModel/ReplayModel 不在 controller 插入，而是由 `e2e/harness` 在 `ResolvedConfig.Providers` 层整体装饰/替换（生产路径零侵入；同时覆盖 `PublishSubagentSnapshot` 的子代理模型通道）。生产侧唯一触点是 `Loop.Execute` 入口的 `replay.WithSessionRef` ctx 注入与 `Loop.ParentToolCallID` 字段。
2. **稳定场景根目录**：录制的工具调用参数是任意碎的流式 delta，绝对路径可能跨越事件边界、无法事后 tokenize——replay 因此必须运行在与 record 相同的路径。record/replay 统一使用 `/tmp/loom-snapshot/<scenario>/` 固定根（go test 与 bazel 沙箱共享），fixture 内完整路径仍做 `{{cwd}}/{{home}}/{{artifacts}}` tokenize/detokenize 双保险。
3. **events 黄金文件只保留 durable 事件**：ephemeral delta 的碎片化文本与 `delta_bytes` 等派生字段由 calls.jsonl 全保真锁定，不进黄金文件。
4. **config.recorded.yaml**：record 时把生效 config 脱敏（api_key 占位、api_key_env 删除）入 fixture，replay 原样加载——工具集/limits/窗口与录制时严格一致，否则 request_header 的 tools 序列必然 diff。
5. **归一化补充**（对照 dsh normalize.ts 的新增项）：loom ID/内容 hash/trace id 按首现序 token 化（map 键序遍历保证序号确定）；`Platform/Shell`、`Current date` 行归一（bazel 沙箱无 $SHELL）；macOS `/private` 路径别名；`WallTime/duration_ms/delta_bytes/occupancy_tokens/started_at/finished_at` 归零；反引号包裹路径的边界感知替换。
6. **R5 实现未依赖 M4 事件**：指纹直接在 Model 边界对 `ModelRequest` 投影计算（header 字段 + 全消息内容，剥离 ID/时间戳后哈希），比设计的"header 机制"更直接；告警附字段级 diff。
