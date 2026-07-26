# Loom Server 模式设计（`loom serve`）

| 项目 | 内容 |
|------|------|
| 状态 | Draft v1（含一轮自审修订，见 §15） |
| 日期 | 2026-07-26 |
| 关联文档 | `DESIGN.md`（§4 总体架构、§15 扩展与前端协议、§31 Runtime Ownership 与 Daemon）、`TUI_DESIGN.md` |
| 目标读者 | loom 运行时与前端贡献者 |

---

## 1. 背景与目标

loom 当前只有一个真实前端：TUI（`internal/ui`，Bubble Tea），外加两个演示性质的非交互渲染器（`render.Linear`、`render.JSONL`）。但架构在设计之初就是"无头"的：`app.Controller` 提供命令式会话 API，`runtimeevent` 提供版本化 JSON 事件协议，TUI 只是协议的一个进程内消费者。

本设计将 loom 扩展为 **Server 模式**：`loom serve` 启动一个本地守护进程，对外提供版本化 HTTP/SSE API，TUI 与 Web 版（及未来的 IDE 插件、移动端）都是**纯展示渲染客户端**，不持有任何运行时逻辑。对标能力为 Codex App 的本地体验：

1. **多会话并行**：同一 server 进程内多个 session 各自独立推进 turn，互不串台；
2. **纯渲染 Web 客户端**：浏览器打开即用，消息流、工具调用、diff、审批全部通过事件协议驱动；
3. **断线重连**：网络闪断/页面刷新后不丢 durable 状态，流式草稿可校正；
4. **远程审批**：审批请求实时推送到任意已连接客户端，决策一次性生效并记录操作者；
5. **会话持久化与恢复**：会话列表、历史回放、跨进程重启恢复（现有 SQLite 事件溯源直接支撑）；
6. **生产级运维**：认证、资源治理、优雅停机、审计日志、健康检查、可观测性。

### 1.1 与非目标

**非目标（本期不做）**：

- 多用户/多租户与真实身份体系（server 面向单机单用户，token 仅为防本机误连与浏览器误提交）；
- 多 workspace（server 启动时绑定单一 workspace，与 `PathValidator` 一致）；
- 公网/局域网远程部署（仅 `127.0.0.1` 或 UDS；远程部署需要 TLS + 真鉴权，留作后续）;
- 会话删除/归档/purge API（只读历史 + 追加式会话，管理类 API 后续补）;
- Web 端内嵌终端（`run_cmd` 实时输出交互）。若未来需要，事件通道可平滑升级到 WebSocket（见 §14 开放问题）；
- MCP 工具暴露给 server 客户端（MCP 是工具侧扩展协议，与前端协议正交）。

---

## 2. 现状评估

### 2.1 已有地基（不需要重写的部分）

| 能力 | 位置 | 说明 |
|------|------|------|
| 无头会话控制器 | `internal/app/controller.go` | `Controller` 提供 `SubmitPrompt/CancelTurn/ResolveApproval/NewSession/ResumeSession/RequestSnapshot/Shutdown/Subscribe/ListSessions`，全部经 `cmdCh` 串行化，天然并发安全。这就是 RPC 方法集的原型。 |
| 版本化事件协议 | `internal/runtimeevent/` | `RuntimeEvent{Version, Sequence, SessionID, RunID, Turn, Kind, Durable, Payload}`，JSON 可直接上线；durable/ephemeral 分离；`ModelResponseCompletedPayload.Text` 携带 canonical 文本用于草稿校正。 |
| 非阻塞事件扇出 | `runtimeevent.Broker` | 慢订阅者 ephemeral 丢弃、durable 断连，绝不阻塞 agent。 |
| 审批桥 | `app.ChannelApprover` | channel 解耦 + `ApprovalBinding{ApprovalID, CallID, ArgsHash}` 一次性 CAS，防重复决议。 |
| 事件溯源持久化 | `session.SQLiteStore` | events + checkpoints；`InspectSession` 返回 `SessionInspection{Session, Checkpoint, Transcript, Events}`；`TranscriptPage` 自带分页（`AfterSequence/NextAfter/HasMore`）；单连接（`SetMaxOpenConns(1)`）天然串行化写入。 |
| 展示数据服务端算好 | `app/controller.go` `publishingStore` | `ApprovalRequestedPayload.Diff`、`ToolCompletedPayload.Preview` 等在服务端生成，纯展示客户端零 diff 逻辑。 |
| 装配与生命周期 | `app.Bootstrap` | store/artifact/registry/model/policy/tracing 一处装配，`Close` 统一释放。 |
| 安全基座 | `workspace.PathValidator`、`process.Runner` 沙箱、`permission.Policy` | 路径边界、命令沙箱、风险分级审批与 server 模式正交，直接复用。 |
| 观测 | `internal/trace`（OTel/Langfuse） | Recorder 已接入 agent loop，server 层只需补 HTTP 层观测。 |

### 2.2 差距清单（本设计要解决的问题）

| # | 差距 | 根因（代码位置） | 后果（若不改） |
|---|------|------|------|
| G1 | 无会话注册表 | `cmd/loom/main.go` 每进程只建 1 个 Controller | server 无法承载多会话 |
| G2 | Goal/Plan cells 是进程级单例 | `Bootstrap.GoalCell/PlanCell`（`bootstrap.go:181-183`），`update_goal/update_plan` 工具注册时绑定 cell | 多会话并发互相覆盖 goal/plan |
| G3 | SessionEnv 是进程级单值 | `process.AtomicSessionEnv` + `RunnerOptions.SessionEnv func() map[string]string`（无 ctx 参数），`controller.publishSessionEnv` 全局覆盖 | 并发时 spawned 命令归因到错误的 session |
| G4 | 事件无回放层 | Broker 只做在线扇出，不存历史 | SSE 断线重连丢 durable 事件 |
| G5 | Snapshot 无事件水位 | `app.Snapshot` 不含 broker sequence | 客户端无法无缝衔接"快照 + 增量" |
| G6 | 审批无 actor、无超时 | `ResolveApproval` 不记录操作者 | 审计缺失；挂起审批无兜底 |
| G7 | 命令无幂等键 | `SubmitPrompt` 重复提交即重复 turn | 网络重试/双击产生重复 turn |
| G8 | 装配逻辑两份 | `runChat` 走 `Bootstrap`，`runAgent` 手工装配（`main.go:240-475`） | server 将成为第三份，腐化风险 |
| G9 | 无传输/认证/治理层 | 不存在 | — |
| G10 | serve 与 direct 模式可同时写同一数据目录 | 无实例互斥（DESIGN.md §31 要求排他锁） | 两进程并发写 SQLite，状态不可预期 |

---

## 3. 总体架构

### 3.1 进程拓扑

```text
┌──────────────────────────── loom serve（单实例守护进程）──────────────────────────┐
│                                                                                  │
│  ┌──────────────┐   HTTP/SSE (127.0.0.1:PORT or UDS)                            │
│  │  Web SPA     │◄──────────┐        ┌──────────────► 任意第三方客户端（curl/SDK）│
│  │ (embed.FS)   │           │        │                                          │
│  └──────────────┘           ▼        ▼                                          │
│                    ┌─────────────────────────┐                                  │
│                    │ internal/server         │  适配层：认证、路由、限流、        │
│                    │ (handlers + sse hub)    │  REST↔SessionService、SSE↔Replay  │
│                    └───────────┬─────────────┘                                  │
│                                │ 进程内调用（Go 接口，无序列化）                  │
│                    ┌───────────▼─────────────┐                                  │
│                    │ app.SessionService      │  会话注册表、生命周期、幂等键、    │
│                    │  map[SessionID]*Handle  │  空闲回收、并发闸门               │
│                    └───────────┬─────────────┘                                  │
│            ┌───────────────────┼───────────────────┐                           │
│            ▼                   ▼                   ▼                           │
│     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                    │
│     │ Controller  │     │ Controller  │     │ Controller  │  每会话一个         │
│     │ +Approver   │     │ +Approver   │     │ +Approver   │  （现有代码）       │
│     │ +Cells      │     │ +Cells      │     │ +Cells      │                    │
│     │ +ReplayLog  │     │ +ReplayLog  │     │ +ReplayLog  │                    │
│     └──────┬──────┘     └──────┬──────┘     └──────┬──────┘                    │
│            └───────────────────┼───────────────────┘                           │
│                    ┌───────────▼─────────────┐                                 │
│                    │ runtimeevent.Broker     │  全局单例：唯一事件总线          │
│                    │  (全局单调 sequence)    │                                 │
│                    └───────────┬─────────────┘                                 │
│                    ┌───────────▼─────────────┐                                 │
│                    │ app.Bootstrap           │  共享资源：Store / Artifact /    │
│                    │                         │  Model / Runner / Policy /       │
│                    │                         │  BaseRegistry / SessionRules     │
│                    └─────────────────────────┘                                 │
└────────────────────────────────────────────────────────────────────────────────┘
```

关键决策：

- **单 Broker 全局序号**：`RuntimeEvent.Sequence` 在 server 内全局单调。它兼作 SSE 的 `Last-Event-ID` cursor，跨会话唯一、可比较。会话过滤发生在 server 分发层。
- **server 不直接让 SSE 客户端挂 Broker**：server 内部用一个永不阻塞的 pump 订阅 Broker，写入 per-session `ReplayLog` 并向本连接的 SSE 客户端扇出（§4.5）。慢客户端由 server 自己断开，Broker 看到的永远是一个健康订阅者。
- **TUI 暂不走 server**：第一阶段 TUI 保持进程内直连（`loom chat` 不变）；协议收敛（TUI 改走 HTTP client）作为独立里程碑 M5（§10）。

### 3.2 与 DESIGN.md §31 的对齐与偏离

| §31 要求 | 本设计 | 状态 |
|----------|--------|------|
| 单实例 Daemon + 数据目录内核排他锁 | `loom serve` 启动时对 `<datadir>/loom.lock` 取 `flock(LOCK_EX\|LOCK_NB)`，失败即退出 | 对齐（M2） |
| Direct 与 Daemon 不得同时拥有数据目录 | 一期：`serve` 加锁，`chat/run` 暂不加锁（保持兼容），风险登记 R5；二期：direct 模式也加同一把锁 | 部分对齐 |
| UDS + 本地客户端认证 + 协议版本协商 | 支持 `--listen unix:<path>` 与 `--listen 127.0.0.1:<port>` 两种监听；token 认证；`/v1` 前缀 + `/v1/meta/version` 协商 | 对齐（M2） |
| RPC 命令带幂等键 | `Idempotency-Key`（§4.7） | 对齐（M2） |
| 事件订阅使用持久化 cursor，断线先补拉再订阅 | Broker seq + per-session ReplayLog（§4.5） | 对齐（M2） |
| 慢客户端可丢弃可重建 Delta 或断开，不得阻塞 Runtime | server pump 非阻塞；慢 SSE 客户端断开并提示重连 | 对齐（M2） |
| 客户端断开不等于取消 Run | SSE/HTTP 连接与会话生命周期完全解耦；turn 只被显式 cancel 或进程停机取消 | 对齐（M2） |
| 审批记录 actor/client identity | ResolveApproval 增加 actor 参数 + 审计日志（§4.6） | 对齐（M2） |
| 升级前停收新 Run、挂起/转交活动任务、Flush Store、清理托管进程 | 优雅停机流程（§7.3）：活动 turn 等待有界时间后取消；进程树清理由 runner 进程组机制保证 | 对齐（M2） |
| fencing token 防旧 owner 副作用 | 单机单实例 + 排他锁已排除双 owner；跨机器 fencing 不做 | 偏离（有意，单机场景） |

### 3.3 包结构演进

```text
go/pl/loom/internal/
├── app/
│   ├── session_service.go      # 新增：SessionService（会话注册表/生命周期/幂等）
│   ├── session_runtime.go      # 新增：SessionRuntime（per-session cells + registry overlay + env）
│   └── controller.go           # 改造：cells 注入、Snapshot 带事件水位、actor
├── agent/
│   └── run.go                  # 改造：ToolRegistry 支持 parent overlay
├── process/
│   └── types.go                # 改造：SessionEnv 函数签名带 ctx
├── runtimeevent/
│   └── replay.go               # 新增：ReplayLog（per-session 有界环形缓冲）
├── server/                     # 新增：HTTP/SSE 适配层
│   ├── server.go               #   Server 装配、监听（TCP/UDS）、优雅停机
│   ├── auth.go                 #   token 中间件
│   ├── handlers_sessions.go    #   REST 端点
│   ├── handlers_events.go      #   SSE 端点 + pump/hub
│   ├── idempotency.go          #   幂等键缓存
│   ├── lock.go                 #   数据目录排他锁
│   └── web/                    #   内嵌 SPA（embed.FS）
└── client/                     # M5 新增：Go client SDK（TUI 收敛用）
```

`server` 只允许依赖 `app`/`runtimeevent`/`domain`/`session` 的公开 API，禁止触碰 `agent` 内部。这保证"客户端不能直接访问 SQLite 表"（DESIGN.md §15）在代码组织层面被依赖规则固化。

---

## 4. 运行时改造（M1，与传输层无关，先行落地）

### 4.1 SessionService（`app/session_service.go`）

```go
// SessionService owns every live session in a serve process. It replaces
// the single-Controller assumption in cmd/loom with a registry that a
// transport (HTTP today, in-process TUI tomorrow) can multiplex.
type SessionService struct {
    bootstrap *Bootstrap
    broker    *runtimeevent.Broker
    logger    *slog.Logger

    mu       sync.Mutex
    sessions map[domain.SessionID]*SessionHandle
    closing  bool

    maxSessions    int           // 默认 32，LOOM_SERVE_MAX_SESSIONS
    idleTTL        time.Duration // 默认 30min，LOOM_SERVE_SESSION_IDLE_TTL
}

type SessionHandle struct {
    ID         domain.SessionID
    Controller *Controller
    Approver   *ChannelApprover
    Runtime    *SessionRuntime   // §4.2/§4.3：per-session cells + registry + env
    Replay     *runtimeevent.ReplayLog
    idem       *idempotencyCache // §4.7

    lastActiveNanos atomic.Int64  // 每次命令/事件触及即刷新
}
```

公开方法（即 server handlers 与未来的 Go SDK 共享的应用层 API）：

| 方法 | 语义 |
|------|------|
| `CreateSession(ctx) (*SessionHandle, error)` | 新建 session（复用 `Controller.NewSession` 语义） |
| `ResumeSession(ctx, id) (*SessionHandle, error)` | 恢复已有 session；已存活则直接返回现有 handle |
| `Get(id) (*SessionHandle, bool)` | 仅查找，不创建 |
| `ListSessions(ctx, limit)` | 透传 store |
| `SubmitPrompt(ctx, id, prompt, idemKey) (turn int, err error)` | 幂等包装 `Controller.SubmitPrompt` |
| `CancelTurn(ctx, id)` / `ResolveApproval(ctx, id, binding, decision, hint, actor)` / `Snapshot(ctx, id)` | 透传 + actor/水位扩展 |
| `Shutdown(ctx)` | 停收新会话与 prompt → 全部 Controller 优雅关闭 |

不变量：

- **一个 SessionID 全进程最多一个 Controller**（DESIGN.md "同一 Run 只有一个 active owner" 的进程内实现）；
- handle 创建/获取在 `mu` 下完成，`ResumeSession` 与 `Get` 竞态不会产生第二个 Controller；
- 空闲回收：后台 sweeper 每分钟扫描，`idle && now-lastActive > idleTTL` 的 handle 执行 `Controller.Shutdown` 并摘除；`Controller.State() != idle` 的 handle 永不回收；
- `closing=true` 后 `CreateSession/ResumeSession/SubmitPrompt` 返回 `ErrDraining`（映射 HTTP 503）。

### 4.2 per-session Goal/Plan cells 与注册表 overlay（G2）

**问题**：`update_goal`/`update_plan` 工具在 `Bootstrap.registerBuiltinTools` 里绑定进程级 cells；`Controller.runTurn` 把这两个 cells 传给每个 `agent.Loop`。

**方案**：

1. `agent.ToolRegistry` 增加 overlay：

```go
// NewOverlayRegistry returns a registry whose lookups fall through to
// parent. Registrations land in the overlay only; the shared parent is
// never mutated after bootstrap.
func NewOverlayRegistry(parent *ToolRegistry) *ToolRegistry
func (r *ToolRegistry) Lookup(name string) (domain.Tool, bool) // local → parent
```

2. `Bootstrap.registerBuiltinTools` 不再注册 `update_goal`/`update_plan`；base registry 只含无状态工具（这些工具本身无会话状态，共享安全）。
3. 新增 `SessionRuntime`，每次 `CreateSession/ResumeSession` 时构建：

```go
type SessionRuntime struct {
    GoalCell *agent.GoalCell
    PlanCell *agent.PlanCell
    Registry *agent.ToolRegistry // overlay(base)：update_goal/update_plan 绑定本 session 的 cells
}
```

4. `Controller` 增加可选字段 `Runtime *SessionRuntime`（`ControllerConfig.Runtime`）；`runTurn` 中 `Loop.GoalCell/PlanCell/Registry` 优先取 `c.Runtime`，为 nil 时回落到现状（`bootstrap.GoalCell/...`），保证 headless `runAgent` 路径与现有测试零改动。

### 4.3 per-session SessionEnv（G3）

**问题**：`RunnerOptions.SessionEnv func() map[string]string` 无 ctx 参数，`Controller.publishSessionEnv` 在 create/resume 时覆盖全局原子值。

**方案**（ctx 传递，不动 runner 执行语义）：

1. `process` 包签名升级：`SessionEnv func(ctx context.Context) map[string]string`；runner 在每次执行处把已有的 `ctx` 传入（runner 的 Run/Start 均已持有 ctx，改动为内部一线）。
2. 新增 `process.ContextWithSessionEnv(ctx, env) ctx` 与 `process.SessionEnvFromContext(ctx) map[string]string`。
3. `Controller.runTurn` 开头：`ctx = process.ContextWithSessionEnv(ctx, process.LoomSessionEnv(c.sessionID.String()))`。agent loop 的工具执行 ctx 派生自 turn ctx，归因自然正确，且**并发 session 互不影响**。
4. `Bootstrap.SessionEnv`/`AtomicSessionEnv` 保留给 headless `runAgent` 路径（其 boot 装配不变），server 路径不再使用；`Controller.publishSessionEnv` 改为 ctx 注入，原原子值写入删除。runner 的 `SessionEnv` 实现变为"先查 ctx，再回落原子值（兼容）"。

### 4.4 Controller 扩展（G5/G6/G7 的服务端部分）

1. `Snapshot` 增加事件水位：

```go
type Snapshot struct {
    // ...现有字段...
    EventSeq uint64 `json:"event_seq"` // 投影已应用到的事件水位（见下）
}
```

`EventSeq` **不能**直接取 `broker.Sequence()`：`publishingStore` 的时序是"先 `broker.Publish`（分配 seq）→ 后持锁更新 controller 投影"，快照若读在两者之间，会出现"某事件 seq ≤ 水位、但其效果尚未进投影"，客户端按水位订阅即永久漏掉该事件的可见效果。正确实现是维护**投影水位（applied watermark）**：`publishingStore` 在更新投影的同一 `controller.mu` 临界区内采样 `broker.Sequence()` 存入 `c.appliedSeq`；`handleRequestSnapshot` 读 `appliedSeq`。安全性论证：同一 session 的 publish 与投影更新严格同线程串行（publish 全部完成后才更新投影），故临界区内采样的水位之前的本 session 事件必然已含于投影；其他 session 的事件与本投影无关，其 seq 造成的"水位虚高"只会导致多补发无害的他会话事件——而 SSE 端点按 session 过滤，实际无影响。

客户端首屏协议：`GET snapshot` → 以 `event_seq` 为 cursor 打开 SSE，不重不漏（回放层对"快照后、订阅前"的事件窗口负责，见 §4.5）。

2. `ResolveApproval` 增加 actor：`ResolveApprovalWithActor(ctx, binding, decision, hint, actor string)`；原方法委托空 actor 保持 TUI 兼容。actor 进入审计日志，并透传到 `ApprovalResolvedPayload`（见 §4.6）。
3. `Controller` 不再在 `publishSessionEnv` 写全局原子值（§4.3）。

### 4.5 事件回放层（G4）：per-session ReplayLog + server pump

**问题**：Broker 断线不补；`render.JSONL` 证明协议可序列化，但缺"先补拉再订阅"的持久 cursor 机制。

**方案**：

1. `runtimeevent/replay.go`：

```go
// ReplayLog is a bounded per-session ring of runtime events, ordered by the
// broker's global sequence. It backs SSE reconnection (Last-Event-ID).
type ReplayLog struct{ /* ring, cap 默认 2048（LOOM_SERVE_REPLAY_CAP） */ }

func (l *ReplayLog) Append(evt RuntimeEvent)
// Since returns events with Sequence > seq, oldest first. ok=false means the
// requested cursor has already rotated out — the client must resync via snapshot.
func (l *ReplayLog) Since(seq uint64) (events []RuntimeEvent, ok bool)
```

2. server 启动唯一 **pump** goroutine：订阅全局 Broker（队列容量调到 4096，是普通订阅者的 16 倍），对每个事件：`SessionHandle.Replay.Append` + 写入该 session 的在线 SSE 客户端队列。pump 内所有写都非阻塞；慢 SSE 客户端（队列满，默认 256）被 server 主动断开——客户端用 `Last-Event-ID` 重连自愈。
3. **补拉-订阅原子衔接**：hub 对每 session 持一把锁；SSE attach 流程 = `lock { events, ok := Replay.Since(cursor); register(client) } unlock` → 先 flush 补发再进入实时流。pump 分发走同一把锁，保证补发窗口内的事件不会同时出现在两侧。
4. **pump 被 Broker 断开**（极端背压）：server 进入 resync 流程——重新 `Subscribe()`，并向所有 SSE 客户端发送 `event: server.resync` 后断开；客户端按"snapshot + 新 cursor"恢复。此路径计入 metrics 与告警日志（设计预期：永不发生；发生即容量或负载异常）。
5. cursor 失效检测（两种，均触发 `server.resync` 后断开，客户端重走 snapshot）：
   - **旋转出界**：cursor 小于 ring 中现存最小 seq（`Since` 返回 `ok=false`）；
   - **cursor 来自未来**：cursor 大于 pump 已见的最大 seq。这只可能发生在 server 重启后——broker sequence 是进程内从 0 开始的内存计数，重启即重置，客户端持有的旧 cursor 在新 seq 空间里无意义。仅靠"空补发"会静默丢失整个重启窗口，因此必须显式判负。
   此外，SSE 首帧（`: connected` 注释行）携带 `instance=<server 启动时生成的随机 ID>`，客户端发现 instance 与上一连接不同时直接走 resync（双保险，也覆盖"cursor 恰好落在合法区间但语义已错"的极端巧合）。
   Web 客户端必须实现该路径（§9 契约）。

### 4.6 审批路由、actor 与超时（G6）

- **广播决议**：`approval.requested` 事件经 SSE 广播到该 session 所有客户端；任何客户端都可决议；`ChannelApprover.ResolveApproval` 的 CAS 保证只有一个生效；其余客户端收到 `approval.resolved` 后撤销本地审批 UI。多客户端竞决体验一期接受（同用户多窗口，先到先得），owner 指定/认领留作开放问题。
- **actor**：REST 决议请求可带 `client` 字段（默认取 token 名）；`ApprovalResolvedPayload` 增加可选字段 `actor`（协议内向后兼容的新增 optional 字段）；审计日志落 `session/approval/call/args_hash/decision/actor`。
- **超时**：`LOOM_SERVE_APPROVAL_TIMEOUT`（默认 0 = 永不，与 TUI 一致）。配置非 0 时，挂起超过该时长的审批由 server 自动 `deny` 并记录 `actor="system:timeout"`。turn 上下文的取消仍由现有 ctx 链路兜底。

### 4.7 命令幂等（G7）

- `POST /v1/sessions/{id}/prompts` 接受可选 `Idempotency-Key` header（或 body 同名字段）。
- `SessionHandle.idem` 为 `map[key]turn` 的有界 LRU（容量 128/session，进程内内存态）：命中直接返回首次结果（`200 {turn, deduplicated: true}`）；未命中执行并记录。
- 语义边界：防"同一 server 进程内的重试/双击"，不承诺跨重启持久（与 DESIGN.md §31 的幂等键目标一致，持久化留作后续增强）。

---

## 5. Wire 协议 v1

设计原则：端点是 §4 应用层 API 的机械映射；事件格式就是 `runtimeevent.RuntimeEvent` 本身，零翻译成本；所有响应 JSON；错误模型统一。

### 5.1 传输与监听

- `loom serve --listen 127.0.0.1:7680`（默认）或 `--listen unix:<datadir>/loom.sock`。
- TCP 与 UDS 共用同一个 `http.ServeMux` 与 handler 集；UDS 模式下 socket 文件权限 `0600`。
- Go 标准库 `net/http`（Go 1.25 的 method-based pattern routing），**不引入 HTTP 框架**；SSE 用 `http.Flusher` 实现，零新依赖。
- HTTP server 参数：`ReadHeaderTimeout=10s`、`MaxHeaderBytes=1MB`、请求体上限 4MB（prompt 可能粘贴大段文本）；SSE 响应无写超时（长连接），其余端点 `WriteTimeout=30s`。

### 5.2 认证

- 启动解析 token：优先 `--token`；否则读 `<datadir>/serve.token`（`0600`）；不存在则生成 32 字节随机 hex 写入该文件，并向 stderr 打印**一次**带 token 的 URL（方便首次复制，此后不再打印，日志不落 token）。
- 所有 `/v1/*` 请求要求 `Authorization: Bearer <token>`（恒定时间比较）。`/healthz`、`/readyz` 豁免（不泄露会话数据）。Web SPA 静态资源豁免，但 SPA 引导页要求用户粘贴 token，之后所有 API 调用走 header（token 不进入 cookie → 天然免疫 CSRF；同源 SPA 无 CORS 需求）。
- CORS：默认不输出任何 `Access-Control-Allow-Origin`（即全部拒绝跨域）；`--allow-origin <origin>` 可显式白名单（仅用于开发调试远程前端）。
- UDS 模式 token 仍强制（多用户机器上 UDS 也可能被同机其他用户访问，`0600` + token 双保险）。

### 5.3 REST API（全部 `/v1` 前缀）

| 方法/路径 | 请求体 | 响应 | 说明 |
|---|---|---|---|
| `GET /v1/meta/version` | — | `{protocol: 1, version, commit?}` | 协议/构建版本协商 |
| `GET /v1/sessions?limit=` | — | `{sessions: [SessionSummary]}` | 会话列表（store 直读，含非活跃） |
| `POST /v1/sessions` | `{resume?: session_id}` | `201 {session_id, state}` 或 `200`（已存活） | 幂等：对存活 session 的 resume 直接返回现状 |
| `GET /v1/sessions/{id}` | — | `SessionInspection`（不含 events）| 会话元数据 + checkpoint 摘要 |
| `GET /v1/sessions/{id}/transcript?after=&limit=` | — | `TranscriptPage` | 历史分页（复用 store 投影，默认 limit=200，上限 1000） |
| `GET /v1/sessions/{id}/snapshot` | — | `app.Snapshot`（含 `event_seq`） | 实时状态 + 当前消息投影 + pending approvals |
| `POST /v1/sessions/{id}/prompts` | `{prompt}` + `Idempotency-Key` | `202 {turn}` / `200 {turn, deduplicated:true}` | 提交 turn；非 idle → 409 |
| `POST /v1/sessions/{id}/cancel` | — | `202` | 取消当前 turn；无可取消 → 409 |
| `POST /v1/sessions/{id}/approvals/{approval_id}` | `{call_id, args_hash, decision, rule_hint?:{tool_name, arguments}, client?}` | `200 {note}` | binding 不匹配 → 409；重复决议 → 409 |
| `GET /v1/sessions/{id}/events?after=<seq>` | SSE | 事件流 | §5.4 |
| `GET /v1/events?sessions={id},{id}` | SSE | 多会话合并流 | 会话列表页的实时徽标；可按 M2 范围裁切到 M3 |
| `GET /healthz` / `GET /readyz` | — | `200 {status:"ok"}` | readyz 检查 store 可写、broker 未关闭 |

错误模型：

```json
{"error": {"code": "not_idle|not_found|unauthenticated|draining|binding_mismatch|rate_limited|invalid_input", "message": "...", "state": "running"}}
```

状态码映射：`invalid_input→400`、`unauthenticated→401`、`not_found→404`、`not_idle/binding_mismatch→409`、`session_closed→410`、`rate_limited→429`、`draining→503`。`app.Controller` 的现有错误字符串映射为上述 code（在 server 层集中转换，不改 controller 错误语义）。

### 5.4 SSE 事件通道

```text
GET /v1/sessions/{id}/events?after=1234
（也支持标准 Last-Event-ID header，query 参数优先）

: connected, instance=7f3a9c, replay_from=1235
id: 1235
event: turn.started
data: {"version":1,"sequence":1235,"session_id":"sess_…","kind":"turn.started","durable":true,"payload":{…}}

id: 1236
event: model.text_delta
data: {…}

: hb 1690000000        ← 每 15s 心跳注释行，防代理断连
```

- `event:` = `RuntimeEvent.Kind`；`data:` = `RuntimeEvent` 完整 JSON；`id:` = 全局 `Sequence`。
- 特殊服务端事件（非 runtimeevent，不进 ReplayLog）：`server.resync`（cursor 失效/pump 重订阅，客户端必须重走 snapshot）、`server.draining`（进程停机，客户端提示并停止重连）。
- 断线恢复：浏览器 `EventSource` 自动带 `Last-Event-ID` 重连；server 走 §4.5 的"补拉-订阅原子衔接"。ephemeral delta 允许在重连间隙丢失——`model.response_completed` 的 canonical `Text` 会校正草稿（现有协议设计原样生效）。
- 背压：每客户端有界队列 256，溢出即断开（客户端重连自愈）；每 session 最多 8 个并发 SSE（`LOOM_SERVE_MAX_SSE_PER_SESSION`）。

### 5.5 版本演进策略

- 协议主版本 = 路径前缀 `/v1` + `RuntimeEvent.Version`；破坏性变更（字段删除/语义变化）→ `/v2`。
- v1 内允许：新增端点、新增 optional 请求/响应字段、新增事件 kind、新增 optional payload 字段（如 `actor`）。客户端必须忽略未知字段与未知事件 kind（写入 Web 客户端契约 §9）。
- `GET /v1/meta/version` 供客户端启动时校验；server 对未知 `X-Loom-Protocol` major 版本请求返回 `426`（预留，一期可不实现）。

---

## 6. 安全设计

| 面 | 措施 |
|----|------|
| 网络暴露面 | 默认仅 `127.0.0.1`；`--listen` 显式才可改；UDS `0600` |
| 认证 | Bearer token，`0600` token 文件，日志/事件流永不出现 token |
| CSRF | token 走 header 不走 cookie，无浏览器自动携带凭证 → 免疫；CORS 默认全拒 |
| XSS（Web SPA） | markdown 渲染走 sanitize 白名单（§9）；diff/tool 输出一律 `textContent` 注入，禁止 `innerHTML` 拼接 |
| 越权操作 | 工具层 `PathValidator` + 沙箱 + Policy 与传输层正交，server 不新增任何绕过路径；审批 binding 三元组 CAS 防伪造/重放 |
| 审计 | slog JSON 审计流：session create/resume、prompt 提交（长度+hash，不落正文）、approval 决议（含 actor）、cancel、admin 操作；`LOOM_SERVE_AUDIT_LOG` 可定向到独立文件 |
| 限流 | 全局并发 turn 闸门（§7）；SSE 连接数上限；prompt 频率软限制（burst 10/s 后 429） |
| 数据目录互斥 | `flock` 排他锁（G10），锁文件 `<datadir>/loom.lock` |
| 密钥卫生 | `LOOM_API_KEY` 等只存在于 server 进程环境，任何 API 响应不回显配置 |

威胁模型边界（明确写出）：信任本机持有 token 的客户端 == 信任用户本人；不防御能读用户 home 目录取 token 的本机恶意进程（与 SSH agent 同一假设）。

## 7. 并发与资源治理

### 7.1 并发模型

- `Bootstrap.Store`（SQLite 单连接）是全进程写入串行点——这是**特性**：事件顺序天然全序，多会话追加由 `database/sql` 排队，吞吐足够（每事件亚毫秒）。
- 每 session 一个 turn（Controller 现有不变量）；全局并发 turn 闸门 `LOOM_SERVE_MAX_ACTIVE_TURNS`（默认 4），超出返回 `429`（排队语义留给后续）。
- model provider（openai provider）本身并发安全；每 turn 一个 HTTP 流，无共享连接状态。

### 7.2 资源上限一览

| 项 | 默认 | 配置 |
|---|---|---|
| 存活 session 数 | 32 | `LOOM_SERVE_MAX_SESSIONS` |
| 全局并发 turn | 4 | `LOOM_SERVE_MAX_ACTIVE_TURNS` |
| session 空闲回收 | 30min | `LOOM_SERVE_SESSION_IDLE_TTL` |
| 每 session SSE 连接 | 8 | `LOOM_SERVE_MAX_SSE_PER_SESSION` |
| 每 session 回放环 | 2048 events | `LOOM_SERVE_REPLAY_CAP` |
| SSE 客户端队列 | 256 | 固定（溢出断开重连） |
| 请求体 | 4MB | 固定 |
| 审批超时 | 0（永不） | `LOOM_SERVE_APPROVAL_TIMEOUT` |

### 7.3 优雅停机

`SIGINT/SIGTERM` → ① 置 draining（`POST /sessions|prompts` 返回 503）→ ② SSE 广播 `server.draining` → ③ 等待活动 turn 完成，上限 `LOOM_SERVE_DRAIN_TIMEOUT`（默认 60s），超时 `CancelTurn` → ④ 全部 Controller `Shutdown`（pending approvals 自动 deny，现有语义）→ ⑤ broker.Close → ⑥ store/artifact flush & close（`Bootstrap.Close`）→ ⑦ 释放 flock。runner 的进程组/沙箱清理由现有 `process` 包在 turn ctx 取消时完成（设计验证点纳入 §11 测试）。

## 8. 可观测性

- **日志**：server 模式 slog JSON 到 stderr（或 `LOOM_SERVE_LOG_FILE`）；审计流独立 logger（§6）。
- **Metrics**（M4）：进程内计数器 + 可选 `/metrics`（Prometheus 文本，默认关，`--metrics` 开启）：活跃 session/turn 数、SSE 连接数与断开原因、事件吞吐与回放命中/失效率、审批等待时长分位、pump resync 次数（应为 0）、SQLite 写入延迟。
- **Trace**：agent 层沿用现有 Langfuse recorder；M4 可选为 REST handler 包一层 OTel span（复用 `internal/trace` 的 provider），不引入新 exporter。

## 9. Web 客户端（M3）

- 交付：`internal/server/web/` 静态资源，`embed.FS` 内嵌，`GET /` 服务 SPA；`--no-web` 可关闭（纯 API 模式）。
- 技术选型（开放问题 O1，默认建议）：**无构建链 vanilla ES modules + vendored markdown 渲染器**（自包含、零 npm、供应链面最小）；若后续交互复杂度上来，再迁 React+Vite 产物 embed。
- **客户端契约**（纯渲染铁律，写进代码评审 checklist）：
  1. 状态来源仅两个：`GET snapshot`（首屏）+ SSE（此后全部）；不本地推导 agent 状态机；
  2. 未知事件 kind / 未知 payload 字段必须忽略；
  3. 流式文本以 delta 拼草稿，`model.response_completed.payload.text` 到达即替换草稿（canonical 校正）；
  4. 审批卡片完全由 `approval.requested` payload 渲染（含 `diff`、`risk`、`description`），决议提交 binding 三元组；收到 `approval.resolved` 即撤卡；
  5. `server.resync` → 清本地状态重走 snapshot；SSE 首帧 `instance` 与上一连接不同 → 等同 resync 处理；`server.draining` → 停止重连并提示；
  6. 所有模型/工具文本输出经 sanitize 后渲染；diff 用服务端给的 `diff` 字符串做语法着色即可；
  7. token 存 `sessionStorage`（关页即清），不落 `localStorage`。
- MVP 功能：token 引导页、会话列表（含实时状态徽标）、新会话/恢复、消息流（text/reasoning 折叠块/tool 块折叠展开、diff 视图、approval 卡片）、输入框（IME 安全）、取消按钮、usage/ctx 占用条、重连状态条。

## 10. TUI 协议收敛（M5，可选但强烈建议）

- 新增 `internal/client`：Go SDK，接口对齐 §4.1 应用层 API（`Events()` 返回事件 channel + 命令方法），两个实现：`inproc`（包 `app.SessionService`）与 `http`（包 §5 协议）。
- TUI 的依赖从 `*app.Controller` 收窄到该接口（UI 实际只用 9 个方法 + Subscribe，收口成本低），`loom chat` 默认 inproc，`loom chat --attach <addr>` 走 http。
- 收益：协议永远是一等公民（TUI 新特性必经 wire 协议），web/TUI 能力自动对齐；DESIGN.md 的"IDE 客户端无需链接内部包"同步达成。

## 11. 测试策略

| 层 | 内容 | 工具 |
|---|---|---|
| 单元 | ReplayLog 环形/旋转语义（property-based：随机 seq 写入，`Since` 不重不漏）；overlay registry；ctx SessionEnv 注入；幂等 LRU | `go test`，fakes |
| 契约 | 每端点表驱动：正常/错误码/错误体 schema；事件 kind↔payload JSON schema golden | `httptest` + `fakes`（已有 fake model/tool/store/approver） |
| 串台回归 | 两 session 并发 turn，断言 goal/plan/SessionEnv/审批各自隔离（**G2/G3 的防回归锁**，CI 必跑） | fakes + 并发 orchestrator |
| 断连恢复 | SSE 中途断 → `Last-Event-ID` 重连 durable 不丢不重；cursor 旋转出界 → `server.resync` | e2e（真实 server + httptest client） |
| 审批 | 双客户端竞决恰好一个成功；超时自动 deny；actor 落审计 | e2e |
| 停机 | draining 拒绝新 prompt；活动 turn 完成/超时取消；runner 进程树无残留；锁释放后第二个实例可启动 | e2e |
| 互斥 | 双实例同数据目录，第二个启动失败且退出码非 0 | e2e |
| 安全负例 | 无/错 token→401；CORS 拒绝；body 超限；`{id}` 路径穿越 | 表驱动 |
| 性能基线 | delta 扇出 P99 延迟、单进程 8 会话并发 turn 的内存/RSS、SQLite 写入吞吐 | benchmark（非门禁，出基线报告） |

Bazel：新增 `//go/pl/loom/internal/server/...` 目标与 `go test` 目标纳入 `bazel test //go/...`（CLAUDE.md 工作流）。

## 12. 里程碑与验收

| 里程碑 | 范围 | 验收标准 | 量级 |
|---|---|---|---|
| **M1 运行时多会话化** | §4.1–4.4、§4.7 应用层部分；`runAgent` 收敛到 Bootstrap+Controller（G8，顺手消灭第三份装配） | 串台回归测试绿；TUI 行为零变化（现有测试全绿）；`loom run/chat` 手工冒烟通过 | 2–3 人日 |
| **M2 serve 骨架** | `internal/server`：监听（TCP/UDS）、flock、token、REST 全端点、SSE+pump+ReplayLog、幂等、优雅停机、审计日志 | `curl` 全流程脚本（建会话→prompt→SSE 流→审批→取消→恢复→断线重连）；契约/断连/停机/互斥/负例测试绿 | 3–4 人日 |
| **M3 Web SPA** | §9 MVP；多会话合并事件流端点（若 M2 裁切） | 浏览器完整跑通：chat/流式/工具块/diff/审批/取消/恢复/刷新重连/resync；XSS 负例（payload 注入 markdown/HTML）不执行 | 3–5 人日 |
| **M4 生产加固** | 资源闸门、metrics、trace span、性能基线、故障注入（kill -9 恢复、ENOSPC）、运维文档 | SLO 基线报告；故障注入后 session 恢复率 100%（durable 无丢）；`loom serve` 运行手册入库 | 2–3 人日 |
| **M5 TUI 收敛**（可选） | `internal/client` 双实现 + TUI 改造 | `loom chat --attach` 与 web 同时操作同一会话；协议一致性测试（inproc/http 双实现跑同一契约套件） | 3–4 人日 |

总计约 13–19 人日（M5 计入）。M1+M2 即可交付"curl 可驱动的 headless server"，M3 交付"类似 Codex App 的网页版"。

## 13. 风险登记册

| # | 风险 | 等级 | 缓解 |
|---|------|------|------|
| R1 | cells/env 下沉有遗漏路径（如未来新工具再绑进程级单例） | 高 | 串台回归测试进 CI；code review checklist 增加"工具不得持有会话态单例" |
| R2 | pump 被 Broker 断开导致全量客户端 resync | 中 | pump 队列 16× 容量、内部全非阻塞；resync 计数告警；客户端 resync 路径 M2 必测 |
| R3 | SQLite 单连接在高并发 turn 下成为尾延迟瓶颈 | 低 | M4 基线量化；必要时 batch append（现有 `AppendEventsAndCheckpoint` 已批量） |
| R4 | token 经 URL/日志泄漏 | 中 | 仅 stderr 打印一次；日志脱敏测试；token 文件 0600 |
| R5 | serve 与 `chat/run` 并发写同一数据目录（direct 模式暂无锁） | 中 | M2 文档明示互斥；M4 给 direct 模式补同一把 flock（对齐 §31） |
| R6 | 多客户端审批"人人可点"体验争议 | 低 | 先到先得语义写进协议文档；owner 指定留开放问题 |
| R7 | Web SPA XSS（模型输出 markdown 注入） | 高 | sanitize 白名单 + 负例测试进 M3 验收；`textContent` 渲染铁律 |
| R8 | snapshot 消息投影与 transcript 投影差异困惑（compaction 后内存投影更新） | 低 | 文档定义：snapshot=实时权威，transcript=历史权威；客户端首屏只用 snapshot |
| R9 | 浏览器 EventSource 连接数限制（每域 6）影响多开标签页 | 低 | 每 session SSE 上限 8 已兜住 server 侧；文档建议单标签或 SPA 单连接多路复用（`/v1/events?sessions=`） |

## 14. 开放问题

| # | 问题 | 默认倾向 |
|---|------|----------|
| O1 | Web 技术栈：vanilla embed vs React 构建链 | 先 vanilla；交互复杂后迁 React（embed 产物） |
| O2 | 是否引入 WebSocket（为将来 web 端内嵌终端/双向通道） | 暂不；SSE+POST 覆盖当前全部交互，WS 作为 `/v2` 演进选项 |
| O3 | 多客户端审批 owner 语义（指定/认领） | 一期广播先到先得；按真实多窗使用反馈再定 |
| O4 | artifact 内容下载端点（大输出查看） | 一期用 transcript/preview 足够；M4 后按需加 `GET /v1/sessions/{id}/artifacts/{aid}`（流式 + 范围请求） |
| O5 | 远程/团队部署（TLS、SSO、多用户隔离） | 明确出范围；协议设计已预留（header 认证可换 OIDC，无 cookie 依赖） |
| O6 | 幂等键跨重启持久化 | 一期内存态；后续可落 SQLite 表 |
| O7 | direct 模式（chat/run）何时加同一把 flock | M4（见 R5） |

---

## 15. 设计自审记录（2026-07-26）

本节为设计完成后的自我审查结论，含发现的问题与处置。

### 15.1 审查方式

对照当前代码逐条核验设计中的"现状断言"与"改造点"：(a) 重读 `controller.go`、`bootstrap.go`、`rule_approver.go`、`process/types.go`、`session/sqlite_store.go`、`runtimeevent/*` 的相关片段；(b) 检查每个端点是否能由现有应用层 API 支撑；(c) 走查三个端到端时序（首屏加载、审批、断线重连）。

### 15.2 审查发现与处置

| # | 发现 | 严重度 | 处置 |
|---|------|--------|------|
| F1 | 初版自评遗漏：`app.Snapshot` 不含事件水位，首屏"快照+订阅"存在补发窗口竞态 | 高（协议级） | 已在 §4.4 增加 `Snapshot.EventSeq` 改造点，§5.3 snapshot 端点与 §9 客户端契约同步更新 |
| F2 | 初版设计让 SSE 客户端直挂 Broker：慢客户端会被 Broker 断开且无法补发，与 §31"先补拉再订阅"冲突 | 高 | 改为 server 内部单 pump + ReplayLog + hub 原子衔接（§4.5），并补充 pump 自身被断开时的 resync 降级路径 |
| F3 | `RunnerOptions.SessionEnv` 现签名为 `func() map[string]string` 且无 ctx；§4.3 方案依赖 runner 在执行点持有 ctx——已核验 runner 的 Run/Start 均持有 ctx，改动可控 | 中 | 保留方案；在 §4.3 明确"回落原子值兼容 headless 路径"，避免 M1 破坏 `runAgent` 与 runner 既有测试 |
| F4 | `RuleApprover` 实际共享 `Bootstrap.SessionRules`（进程内内存态），跨 session 共享"allow always"记忆是既有语义而非 bug | 低 | 设计中不改语义；在 §4.1 SessionService 注释级说明：规则记忆为进程级共享，跨会话生效与 TUI 现状一致；持久化列入 O6 相邻后续项 |
| F5 | 单 Broker 全局 seq 作 cursor：会话过滤后 seq 有洞，`Since` 语义必须按"大于即补"而非"连续"实现 | 中 | §4.5 ReplayLog.Since 契约已写明按 seq 比较、允许洞；property 测试覆盖（§11） |
| F6 | `POST /v1/sessions` 若允许带 workspace 参数将需要 per-session Bootstrap（PathValidator 绑定根目录）——超出范围 | 中 | §1.1 非目标已明确单 workspace；端点不收 workspace 字段 |
| F7 | 审批超时自动 deny 需要有人"看表"——挂起审批在 `ChannelApprover.RequestApproval` 阻塞等待中，server 侧超时须走 `ResolveApproval(deny)` 而非取消 ctx，否则 turn 语义变化 | 中 | §4.6 明确由 server 定时器走正常 deny 决议路径（CAS 保证与人工决议互斥），不触碰 turn ctx |
| F8 | `/v1/events?sessions=a,b` 多路复用端点在 M2 范围过重 | 低 | 已标注可裁切到 M3（§5.3、§12） |
| F9 | `Snapshot.Messages` 对大会话可能超响应体舒适区 | 低 | §5.3 保留 transcript 分页端点；snapshot 的消息投影现状即有界（checkpoint 投影），M4 基线再评估是否加 `?include=` 开关 |
| F10 | 直接复用 `Controller` 错误字符串做 code 映射较脆弱 | 低 | §5.3 已限定"server 层集中转换"；后续若 Controller 导出 typed error 则平滑替换，不动协议 |
| F11 | **首屏水位竞态**（时序走查发现）：`Snapshot.EventSeq` 若直接取 `broker.Sequence()`，因 `publishingStore` 先 publish（分配 seq）后更新投影，快照读在两者之间会导致客户端永久漏掉一条已分配 seq 但尚未入投影的事件 | 高 | 已修订 §4.4：`EventSeq` 改为投影更新临界区内采样的"投影水位"，并附安全性论证 |
| F12 | **进程重启 seq 空间重置**（断线重连走查发现）：broker sequence 进程内从 0 计数，重启后客户端持旧 cursor 重连会被"空补发"假象静默丢失整个重启窗口 | 高 | 已修订 §4.5/§5.4/§9：`Since` 对"cursor 来自未来"判负触发 resync；SSE 首帧携带 `instance` ID，客户端检测到变化即 resync |

### 15.3 残余风险声明

- G2/G3 的改造依赖"所有会话态都经 cells/env 两通道传递"这一现状判断；若未来新增携带会话态的工具，R1 的 CI 回归是唯一防线，需在 review checklist 中固化。
- 性能数字（2048 ring、256 客户端队列、4 并发 turn）均为经验初值，M4 基线后可能调整；协议不受其影响（全部走配置）。
