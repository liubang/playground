# Loom Server 模式设计（`loom serve`）

| 项目 | 内容 |
|------|------|
| 状态 | Draft v3（v1 自审 §15；v2 现状核对 §16；v3 架构定案 §17） |
| 日期 | 2026-08-03（v1：2026-07-26；v2/v3：2026-08-03） |
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
- ~~多 workspace~~ **已实现**（docs/WORKSPACE_DESIGN.md）：serve 单进程承载多 workspace，session 按 workspace 归属、过滤与隔离；
- 公网/局域网远程部署（仅 `127.0.0.1` 或 UDS；远程部署需要 TLS + 真鉴权，留作后续）;
- 会话删除/归档/purge API（只读历史 + 追加式会话，管理类 API 后续补）;
- Web 端内嵌终端（`run_cmd` 实时输出交互）。若未来需要，事件通道可平滑升级到 WebSocket（见 §14 开放问题）；
- MCP 工具暴露给 server 客户端（MCP 是工具侧扩展协议，与前端协议正交）。

---

## 2. 现状评估

### 2.1 已有地基（不需要重写的部分）

| 能力 | 位置 | 说明 |
|------|------|------|
| 无头会话控制器 | `internal/app/controller.go` | `Controller` 提供 `SubmitPrompt/CancelTurn/ResolveApproval/AnswerQuestion/NewSession/ResumeSession/RequestSnapshot/RequestCompaction/SetModel/SetReasoning/SubagentView/ListSessions/ListSkills/ListMCPServers/Shutdown/Subscribe`，全部经 `cmdCh` 串行化，天然并发安全。这就是 RPC 方法集的原型（注意：v1 之后 API 面已扩张，见 G12）。 |
| 忙时投稿（steer） | `app/controller.go` `handleSteer` + `agent.SteerCell` | turn 忙时的新投稿进入 SteerCell 排队、由运行中的 loop 在下一次模型调用前注入；满了软拒绝（`SubmitResult{Steered, QueueLen}`）。server 端点应复用该语义而非简单 409（见 §5.3 与 §16.3 D1）。 |
| 问答桥 | `app.ChannelQuestioner` + `Controller.AnswerQuestion` | `ask_user` 的挂起/决议通道，与 `ChannelApprover` 同构（pending map + 一次性 `Resolve` + `SkipAll`）。 |
| 子代理事件桥 | `app.WireSubagentObserver` | 子 run 的 started/progress/finished 事件**挂父 session envelope** 发布——SSE 按会话过滤与 per-session ReplayLog 天然兼容，无需特殊路由。 |
| 版本化事件协议 | `internal/runtimeevent/` | `RuntimeEvent{Version, Sequence, SessionID, RunID, Turn, Kind, Durable, Payload}`，JSON 可直接上线；durable/ephemeral 分离；`ModelResponseCompletedPayload.Text` 携带 canonical 文本用于草稿校正。 |
| 非阻塞事件扇出 | `runtimeevent.Broker` | 慢订阅者 ephemeral 丢弃、durable 断连，绝不阻塞 agent。 |
| 审批桥 | `app.ChannelApprover` | channel 解耦 + `ApprovalBinding{ApprovalID, CallID, ArgsHash}` 一次性 CAS，防重复决议。 |
| 事件溯源持久化 | `session.SQLiteStore` | events + checkpoints；`InspectSession` 返回 `SessionInspection{Session, Checkpoint, Transcript, Events}`；`TranscriptPage` 自带分页（`AfterSequence/NextAfter/HasMore`）；单连接（`SetMaxOpenConns(1)`）天然串行化写入。 |
| 展示数据服务端算好 | `app/controller.go` `publishingStore` | `ToolPreparedPayload.Diff`、`ToolCompletedPayload.Preview` 等在服务端生成，纯展示客户端零 diff 逻辑。 |
| 装配与生命周期 | `app.Bootstrap` | store/artifact/registry/model/policy/tracing 一处装配，`Close` 统一释放。 |
| 安全基座 | `workspace.PathValidator`、`process.Runner` 沙箱、`permission.Policy` | 路径边界、命令沙箱、风险分级审批与 server 模式正交，直接复用。 |
| 观测 | `internal/trace`（OTel/Langfuse） | Recorder 已接入 agent loop，server 层只需补 HTTP 层观测。 |

### 2.2 差距清单（本设计要解决的问题）

| # | 差距 | 根因（代码位置） | 后果（若不改） |
|---|------|------|------|
| G1 | 无会话注册表 | `cmd/loom/main.go` 每进程只建 1 个 Controller | server 无法承载多会话 |
| G2 | 会话态工具绑定进程级单例（v2 扩围） | `Bootstrap.GoalCell/PlanCell/SteerCell/Questioner`（`bootstrap.go:221-224`），`update_goal/update_plan/ask_user` 注册时各自绑定；`handleSteer` 亦直读 `bootstrap.SteerCell` | 多会话并发互相覆盖 goal/plan/steer 队列，ask_user 问题路由错会话 |
| G3 | SessionEnv 是进程级单值 | `process.AtomicSessionEnv` + `RunnerOptions.SessionEnv func() map[string]string`（无 ctx 参数）；v2 核对：v1 所述 `controller.publishSessionEnv` 已不存在，现仅 headless `runAgent` 写该原子值（`main.go:443`），chat 路径根本未设置归因 | server 引入归因必须走 ctx 注入，不能复活全局写入 |
| G4 | 事件无回放层 | Broker 只做在线扇出，不存历史 | SSE 断线重连丢 durable 事件 |
| G5 | Snapshot 无事件水位 | `app.Snapshot` 不含 broker sequence | 客户端无法无缝衔接"快照 + 增量" |
| G6 | 审批无 actor、无超时 | `ResolveApproval` 不记录操作者 | 审计缺失；挂起审批无兜底 |
| G7 | 命令无幂等键 | `SubmitPrompt` 重复提交即重复 turn | 网络重试/双击产生重复 turn |
| G8 | 装配逻辑两份 | v2 核对：已部分收敛——`runAgent` 现复用 `Bootstrap`（store/registry/runner/policy 共享），仅 `agent.Loop` 装配仍手工（`main.go:460-469`） | 残留风险低；server 走 Controller 路径即完全绕开 |
| G9 | 无传输/认证/治理层 | 不存在 | — |
| G10 | serve 与 direct 模式可同时写同一数据目录 | 无实例互斥（DESIGN.md §31 要求排他锁） | 两进程并发写 SQLite，状态不可预期 |
| G11 | 挂起问题无快照兜底（v2 新增） | `question.asked` 经 `publishEphemeral` 发布（`NewController` 的 `BindPublish`），`Snapshot` 有 `PendingApprovals` 却无 `PendingQuestions` | 客户端重连后丢失进行中的 ask_user 卡片，问题挂起到 turn 结束 |
| G12 | 协议面未覆盖已扩张的 Controller API（v2 新增） | v1 之后新增 steer、`AnswerQuestion`、`SetModel/SetReasoning`、`RequestCompaction`、`SubagentView`、`ListSkills/ListMCPServers` | 照 v1 端点表实现则 Web 客户端能力显著弱于 TUI |

---

## 3. 总体架构

### 3.1 进程拓扑

```text
┌─────────────────────── loom 进程（chat 与 serve 同一套装配）──────────────────────┐
│                                                                                  │
│  客户端层（全部平权，只依赖 internal/client.Client）                              │
│  ┌──────────────┐    ┌──────────────┐              ┌────────────────────────┐    │
│  │ TUI          │    │ Web SPA      │              │ curl / 第三方 SDK      │    │
│  │ (loom chat)  │    │ (embed.FS)   │              │                        │    │
│  └──────┬───────┘    └──────┬───────┘              └───────────┬────────────┘    │
│         │ inproc            │ HTTP/SSE（127.0.0.1:PORT / UDS） │                  │
│         │（零序列化）        ▼                                 ▼                  │
│         │            ┌───────────────────────────────────────────────┐           │
│         │            │ 传输适配层（可插拔；只做格式映射+连接管理）      │           │
│         │            │ internal/server：REST+SSE（适配器 #1）           │           │
│         │            │ 未来：JSON-RPC/WS、gRPC（适配器 #2/#3，§5.6）    │           │
│         │            └───────────────────────┬───────────────────────┘           │
│         │                                    │ 进程内调用（Go 接口）              │
│         ▼                                    ▼                                  │
│                    ┌─────────────────────────────────────────┐                   │
│                    │ app.SessionService（协议无关应用层）       │                   │
│                    │ 会话注册表/生命周期/幂等/typed errors/     │                   │
│                    │ SubscribeEvents（pump+ReplayLog 在此层）   │                   │
│                    └───────────────────┬─────────────────────┘                   │
│            ┌───────────────────────────┼───────────────────┐                     │
│            ▼                           ▼                   ▼                     │
│     ┌─────────────┐             ┌─────────────┐     ┌─────────────┐              │
│     │ Controller  │             │ Controller  │     │ Controller  │ 每会话一个    │
│     │ +Approver   │             │ +Approver   │     │ +Approver   │（现有代码）   │
│     │ +Runtime    │             │ +Runtime    │     │ +Runtime    │              │
│     │(cells/qr/reg)│            │(cells/qr/reg)│    │(cells/qr/reg)│             │
│     └──────┬──────┘             └──────┬──────┘     └──────┬──────┘              │
│            └───────────────────────────┼───────────────────┘                     │
│                        ┌───────────────▼─────────────┐                           │
│                        │ runtimeevent.Broker         │ 全局单例：唯一事件总线      │
│                        │  (全局单调 sequence)        │                           │
│                        └───────────────┬─────────────┘                           │
│                        ┌───────────────▼─────────────┐                           │
│                        │ app.Bootstrap               │ 共享资源：Store/Artifact/  │
│                        │                             │ Model/Runner/Policy/       │
│                        │                             │ BaseRegistry/SessionRules  │
│                        └─────────────────────────────┘                           │
└────────────────────────────────────────────────────────────────────────────────┘
```

关键决策：

- **单 Broker 全局序号**：`RuntimeEvent.Sequence` 在 server 内全局单调。它兼作 SSE 的 `Last-Event-ID` cursor，跨会话唯一、可比较。会话过滤发生在 SessionService 分发层。
- **事件订阅下沉应用层**（v3）：SessionService 内部唯一 pump 订阅 Broker，写 per-session `ReplayLog`，经 `SubscribeEvents(id, cursor)` 对上层暴露"补拉-订阅原子衔接"的流（§4.5）。SSE handler 只负责格式化；未来 JSON-RPC/gRPC 适配器复用同一流。
- **TUI 是 client，不是特殊公民**（v3 定案）：`loom chat` 通过 `client.NewInProc(service)` 消费同一接口，协议从 M1' 起即一等公民；`loom chat --attach` 走 HTTP。v1/v2 的"TUI 直连 + M5 收敛"路线废弃（§17.4）。
- **单一构造路径**（v3 定案）：Controller 的会话态只来自 SessionRuntime，无 nil 回落；headless `loom run` 不经 Controller，保留自己的装配（§17.4）。

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
├── client/                     # M1' 新增：Client 接口 + inproc 实现（M2 加 http 实现）
│   ├── client.go               #   Client 接口（协议无关；全部类型 JSON 可序列化）
│   └── inproc.go               #   委托 SessionService；拷贝语义（禁共享可变引用）
├── app/
│   ├── session_service.go      # 新增：SessionService + pump + SubscribeEvents
│   ├── session_runtime.go      # 新增：SessionRuntime（per-session cells/questioner/registry overlay）
│   └── controller.go           # 改造：Runtime 必填（单一路径）、Snapshot 水位+PendingRequests、actor
├── agent/
│   └── run.go                  # 改造：ToolRegistry 支持 parent overlay
├── process/
│   └── types.go                # 改造：SessionEnv 函数签名带 ctx
├── runtimeevent/
│   └── replay.go               # 新增：ReplayLog（per-session 有界环形缓冲）
├── server/                     # M2 新增：REST+SSE 适配层（适配器 #1）
│   ├── server.go               #   Server 装配、监听（TCP/UDS）、优雅停机
│   ├── auth.go                 #   token 中间件
│   ├── handlers_sessions.go    #   REST 端点（参数解码+typed error 映射，无业务语义）
│   ├── handlers_events.go      #   SSE 格式化层（消费 SubscribeEvents 流，无 pump/hub 逻辑）
│   ├── lock.go                 #   数据目录排他锁
│   └── web/                    #   内嵌 SPA（embed.FS，M3）
└── rpc/（预留）                # M5+：JSON-RPC/WS 或 gRPC 适配器（§5.6）
```

`server` 只允许依赖 `app`/`runtimeevent`/`domain`/`session` 的公开 API，禁止触碰 `agent` 内部。这保证"客户端不能直接访问 SQLite 表"（DESIGN.md §15）在代码组织层面被依赖规则固化。

---

## 4. 运行时与应用层改造（M1'，与传输层无关，先行落地）

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
| `SubscribeEvents(ctx, id, afterSeq) (<-chan RuntimeEvent, error)` | 补拉-订阅原子衔接的事件流（§4.5）；cursor 失效返回 `ErrCursorInvalid`，调用方走 snapshot resync |
| `Shutdown(ctx)` | 停收新会话与 prompt → 全部 Controller 优雅关闭 |

不变量：

- **一个 SessionID 全进程最多一个 Controller**（DESIGN.md "同一 Run 只有一个 active owner" 的进程内实现）；
- handle 创建/获取在 `mu` 下完成，`ResumeSession` 与 `Get` 竞态不会产生第二个 Controller；
- 空闲回收：后台 sweeper 每分钟扫描，`idle && now-lastActive > idleTTL` 的 handle 执行 `Controller.Shutdown` 并摘除；`Controller.State() != idle` 的 handle 永不回收；
- `closing=true` 后 `CreateSession/ResumeSession/SubmitPrompt` 返回 `ErrDraining`（映射 HTTP 503）。
- **协议无关硬约束**（v3，§17.5）：错误全部为导出哨兵错误（`ErrDraining/ErrSessionNotFound/ErrNotIdle/ErrCursorInvalid/ErrBindingMismatch`…），由各传输适配器自行映射；幂等键、actor 为方法一等参数。应用层不出现任何传输层概念。

### 4.2 per-session 会话态与注册表 overlay（G2）

**问题**（v2 扩围：从两处 cells 扩大到全部四处进程级会话态）：`update_goal`/`update_plan`/`ask_user` 在 `Bootstrap.registerBuiltinTools` 里分别绑定进程级 `GoalCell`/`PlanCell`/`Questioner`；`Controller.runTurn` 把 cells 传给每个 `agent.Loop`，`handleSteer` 直读 `bootstrap.SteerCell`。四处单例在多会话下全部串台。

**方案**：

1. `agent.ToolRegistry` 增加 overlay：

```go
// NewOverlayRegistry returns a registry whose lookups fall through to
// parent. Registrations land in the overlay only; the shared parent is
// never mutated after bootstrap.
func NewOverlayRegistry(parent *ToolRegistry) *ToolRegistry
func (r *ToolRegistry) Lookup(name string) (domain.Tool, bool) // local → parent
```

2. base registry 的处理（M1' 落地修订）：`registerBuiltinTools` **保留**三工具的注册（headless `runAgent` 直接使用 base registry + bootstrap cells，改动为零）；serve/TUI 路径经 overlay **遮蔽（shadow）**三工具为会话级绑定——隔离由"会话 loop 只使用 overlay"保证，而非由 base registry 不含会话态工具保证。
3. 新增 `SessionRuntime`，每次 `CreateSession/ResumeSession` 时构建：

```go
type SessionRuntime struct {
    GoalCell   *agent.GoalCell
    PlanCell   *agent.PlanCell
    SteerCell  *agent.SteerCell   // handleSteer 改读这里
    Questioner *ChannelQuestioner // ask_user 的会话级问答通道
    Registry   *agent.ToolRegistry // overlay(base)：三工具绑定本 session 的 cells/questioner
}
```

`SessionHandle.Questioner` 即 `SessionRuntime.Questioner`；其 `BindPublish` 在该 session 的 Controller 构造时挂到事件流（逻辑同现有 `NewController`，只是 questioner 来源从外部注入变为 runtime 持有）。

4. `Controller` 的会话态**只**来自 `Runtime *SessionRuntime`（`ControllerConfig.Runtime`，必填——v3 定案：单一路径，无 nil 回落，§17.4）。`runChat` 与 `loom serve` 都经 SessionService 构造 Runtime，行为天然一致；headless `runAgent` 不经 Controller（直接装配 `agent.Loop`），保留使用 bootstrap 的进程级 cells，不受影响。现有 controller 测试经 `NewSessionRuntime(bootstrap)` helper 适配。

### 4.3 per-session SessionEnv（G3）

**问题**：`RunnerOptions.SessionEnv func() map[string]string` 无 ctx 参数，进程内只能有一个生效归因值。v2 核对：v1 所述 `Controller.publishSessionEnv` 已不存在，现仅 headless `runAgent` 写 `AtomicSessionEnv` 原子值（`main.go:443`），chat 路径根本未设置归因；serve 多会话若沿用该通道必然互相覆盖。

**方案**（ctx 传递，不动 runner 执行语义）：

1. `process` 包签名升级：`SessionEnv func(ctx context.Context) map[string]string`；runner 在每次执行处把已有的 `ctx` 传入（runner 的 Run/Start 均已持有 ctx，改动为内部一线）。
2. 新增 `process.ContextWithSessionEnv(ctx, env) ctx` 与 `process.SessionEnvFromContext(ctx) map[string]string`。
3. `Controller.runTurn` 开头：`ctx = process.ContextWithSessionEnv(ctx, process.LoomSessionEnv(version, c.sessionID.String()))`。agent loop 的工具执行 ctx 派生自 turn ctx，归因自然正确，且**并发 session 互不影响**（纯新增——controller 现无任何全局写入）。
4. `Bootstrap.SessionEnv`/`AtomicSessionEnv` 保留给 headless `runAgent` 路径（其 boot 装配不变），Controller 路径不再使用；runner 的 `SessionEnv` 实现变为"先查 ctx，再回落原子值（兼容 runAgent 与既有 runner 测试）"。

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
3. `Snapshot` 增加 `PendingRequests`（G11 + v3 统一抽象）：审批与问答在应用层统一为"挂起的、一次性可决议的请求"——`PendingRequest{Kind: approval|question, ID, Payload}`，吞并现有 `PendingApprovals`（TUI 在 M1' 迁移时同步切换）。`approval.requested` 与 `question.asked` 均为 ephemeral 事件，重连即丢；快照兜底让客户端据此重建审批卡/问答卡。REST 适配器暴露 `POST .../approvals|questions/{id}` 决议，未来 JSON-RPC 适配器映射为 `ServerRequest`/`ClientResponse`——两种表达打到同一个一次性 `Resolve`（§4.6、§17.2）。
4. `Controller` 的 turn ctx 注入 per-session SessionEnv（§4.3）；v2 核对：controller 本就无全局写入（v1 所述 `publishSessionEnv` 已移除），本改造为纯新增。

### 4.5 事件回放层（G4）：per-session ReplayLog + SessionService pump（v3：下沉应用层）

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

2. **SessionService**（而非传输层）启动唯一 **pump** goroutine：订阅全局 Broker（`WithDurableQueue(4096)`，普通订阅者的 16 倍），对每个事件：`SessionHandle.Replay.Append` + 扇出到该 session 的活跃订阅队列。pump 内所有写都非阻塞；慢订阅（队列满，默认 256）由 SessionService 主动断开——各传输层自行表现（SSE 断开靠 `Last-Event-ID` 重连自愈；inproc client 走 cursor 重订阅）。
3. **补拉-订阅原子衔接**：SessionService 对每 session 持一把锁；`SubscribeEvents(id, cursor)` = `lock { events, ok := Replay.Since(cursor); register(subscriber) } unlock` → 先 flush 补发再进入实时流。pump 分发走同一把锁，保证补发窗口内的事件不会同时出现在两侧。**任何传输适配器（SSE/未来 JSON-RPC/gRPC）消费同一接口，重连语义只有一份实现。**
4. **pump 被 Broker 断开**（极端背压）：SessionService 进入 resync 流程——重新 `Subscribe()`，并向所有活跃订阅发送 `server.resync` 后断开；客户端按"snapshot + 新 cursor"恢复。此路径计入 metrics 与告警日志（设计预期：永不发生；发生即容量或负载异常）。
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

设计原则：REST+SSE 是 SessionService 的**传输适配器 #1**——端点是 §4 应用层 API 的机械映射，事件格式就是 `runtimeevent.RuntimeEvent` 本身（零翻译成本），所有响应 JSON，错误模型统一。适配器只做四件事：认证、参数解码、typed error → 状态码映射、SSE 格式化；不含任何业务语义。多协议演进路线见 §5.6。

### 5.1 传输与监听

- `loom serve --listen 127.0.0.1:7680`（默认）或 `--listen unix:<datadir>/loom.sock`。
- `<datadir>` 即会话数据库所在目录，默认 `~/.loom/sessions`（loom home 由 `LOOM_HOME` 定位，默认 `~/.loom`，`sessions/` 由其派生）；`serve.token`、`loom.lock`、artifacts 均位于该目录。
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
| `GET /v1/sessions/{id}/snapshot` | — | `app.Snapshot`（含 `event_seq`、`pending_requests`） | 实时状态 + 当前消息投影 + 挂起审批/问答 |
| `POST /v1/sessions/{id}/prompts` | `{prompt}` + `Idempotency-Key` | `202 {turn}` / `202 {steered:true, queue_len}` / `200 {turn, deduplicated:true}` | 投稿：idle → 新 turn；busy → steer 入队（对齐 TUI，§16.3 D1）；SteerCell 满 → 409 |
| `POST /v1/sessions/{id}/cancel` | — | `202` | 取消当前 turn；无可取消 → 409 |
| `POST /v1/sessions/{id}/approvals/{approval_id}` | `{call_id, args_hash, decision, rule_hint?:{tool_name, arguments}, client?}` | `200 {note}` | binding 不匹配 → 409；重复决议 → 409 |
| `POST /v1/sessions/{id}/questions/{question_id}` | `{answers:[..], skipped?:bool}` | `200` / 未知或已决议 → 409 | ask_user 决议（`ChannelQuestioner.Resolve` 一次性语义，G12） |
| `POST /v1/sessions/{id}/model` | `{provider, model}` | `200 {model_name}` | 会话级切模型，下个 turn 生效（透传 `SetModel`） |
| `POST /v1/sessions/{id}/reasoning` | `{effort}` | `200 {reasoning_effort}` | 会话级 reasoning 拨盘（透传 `SetReasoning`） |
| `POST /v1/sessions/{id}/compact` | — | `202` | 请求下个 turn 强制 compaction（透传 `RequestCompaction`） |
| `GET /v1/sessions/{id}/subagents/{child_session_id}` | — | `SubagentView` | 子 run 只读钻取视图（可裁切到 M3） |
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

### 5.6 多协议演进路线（v3）

SessionService 协议无关 ⇒ 新协议 = 新增适配器，不重设协议语义（§17.3 选型记录）。候选与触发条件：

| 适配器 | 触发条件 | 要点 | 量级 |
|---|---|---|---|
| JSON-RPC 2.0 / WebSocket | Web 端需要内嵌终端等真双向交互；或要与 codex 生态互操作 | 审批/问答映射为 `ServerRequest`/`ClientResponse`（应用层 PendingRequest 语义已对齐）；事件 = notification 直接包 `RuntimeEvent`；重连复用 `SubscribeEvents(cursor)` | 3–4 人日 |
| gRPC（server-streaming） | 出现机器对机器消费者（CI 编排、Go/Rust IDE 插件） | `SubscribeEvents` → server-streaming RPC；需 .proto 镜像 `RuntimeEvent`（双 schema 税）；浏览器仍需 grpc-web 翻译层，故不做首适配器 | 2–3 人日 |

不做首适配器的理由：本期客户端图谱 = TUI（inproc）+ 浏览器 SPA + curl，REST+SSE 对三者全优（EventSource 自动重连、零新依赖、事件 JSON 零翻译、seq cursor 增量重连）。

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
  4. 审批卡片完全由 `approval.requested` payload 渲染（含 `risk`、`description`；diff 不随审批载荷重复传输，复用 `tool.prepared` 已渲染在工具块上的那份），决议提交 binding 三元组；收到 `approval.resolved` 即撤卡；问答卡片同理：首屏由 `snapshot.pending_requests`（`kind=question` 的条目）重建、此后由 `question.asked` 驱动，收到 `question.answered` 即撤卡；
  5. `server.resync` → 清本地状态重走 snapshot；SSE 首帧 `instance` 与上一连接不同 → 等同 resync 处理；`server.draining` → 停止重连并提示；
  6. 所有模型/工具文本输出经 sanitize 后渲染；diff 用服务端给的 `diff` 字符串做语法着色即可；
  7. token 存 `sessionStorage`（关页即清），不落 `localStorage`。
- MVP 功能：token 引导页、会话列表（含实时状态徽标）、新会话/恢复、消息流（text/reasoning 折叠块/tool 块折叠展开、diff 视图、approval 卡片）、输入框（IME 安全）、取消按钮、usage/ctx 占用条、重连状态条。

## 10. Client 层与 TUI 平权（M1' 交付，v3 由 M5 提前）

- `internal/client`：`Client` 接口对齐 §4.1 应用层 API（命令方法 + `SubscribeEvents` + `Snapshot`）。**接口硬约束**（§17.5）：所有请求/响应类型 JSON 可序列化（roundtrip property test 守护）；inproc 实现禁止返回调用方可变写的共享引用（Snapshot/Messages 一律拷贝），保证 inproc 与 http 行为一致。
- 两个实现：`inproc`（M1'，委托 `app.SessionService`）与 `http`（M2，包 §5 协议）。**同一套契约测试打两个实现**（M2 起 CI 双跑），保证永不漂移。
- TUI 的依赖从 `*app.Controller` 收窄到 `client.Client`：迁移为纯依赖注入改动（`update.go`/`view.go` 状态机与渲染不动；`app.Snapshot`/`app.SubmitResult` 等类型挪入 client 包或别名）。`loom chat` 默认 inproc，`loom chat --attach <addr>` 走 http。验收 = 现有 `ui_test.go` 原样跑绿。
- 收益：协议从第一天就是一等公民（TUI 新特性必经 client 接口）；web/TUI/未来 IDE 插件能力自动对齐；接口完整性由最难的消费者（TUI）验证后，REST+SSE 适配器只剩机械映射；DESIGN.md 的"IDE 客户端无需链接内部包"同步达成。

## 11. 测试策略

| 层 | 内容 | 工具 |
|---|---|---|
| 单元 | ReplayLog 环形/旋转语义（property-based：随机 seq 写入，`Since` 不重不漏）；overlay registry；ctx SessionEnv 注入；幂等 LRU；client 类型 JSON roundtrip（序列化硬约束，§17.5） | `go test`，fakes |
| 契约 | 每端点表驱动：正常/错误码/错误体 schema；事件 kind↔payload JSON schema golden | `httptest` + `fakes`（已有 fake model/tool/store/approver） |
| 串台回归 | 两 session 并发 turn，断言 goal/plan/steer/question/SessionEnv/审批六路各自隔离（**G2/G3 的防回归锁**，CI 必跑） | fakes + 并发 orchestrator |
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
| **M1' client 化**（v3 重排：原 M1 + 原 M5 主体） | §4 全部（SessionService/SessionRuntime 单一路径/ctx env/Snapshot 水位+PendingRequests/SubscribeEvents 下沉/幂等应用层）+ `internal/client`（接口+inproc）+ **TUI 迁移** + 序列化硬约束测试 | `ui_test.go` 原样全绿（走 client 接口）；串台回归六路隔离绿；JSON roundtrip property test 绿；`loom run/chat` 冒烟通过 | 4–5 人日 |
| **M2 REST+SSE 适配器** | `internal/server`：监听（TCP/UDS）、flock、token、REST 全端点、SSE 格式化层、优雅停机、审计日志 + **契约测试双跑**（inproc/http） | `curl` 全流程脚本（建会话→prompt→SSE 流→审批→问答→取消→恢复→断线重连）；契约/断连/停机/互斥/负例测试绿 | 3–4 人日 |
| **M3 Web SPA** | §9 MVP；多会话合并事件流端点（若 M2 裁切） | 浏览器完整跑通：chat/流式/工具块/diff/审批/问答/取消/恢复/刷新重连/resync；XSS 负例不执行 | 3–5 人日 |
| **M4 生产加固** | 资源闸门、metrics、trace span、性能基线、故障注入（kill -9 恢复、ENOSPC）、运维文档 | SLO 基线报告；故障注入后 session 恢复率 100%（durable 无丢）；`loom serve` 运行手册入库 | 2–3 人日 |
| **M5+ 多协议适配器**（按需） | JSON-RPC/WS 或 gRPC 适配器（§5.6 触发条件） | 契约测试三跑；新适配器零 SessionService 改动 | 2–4 人日/个 |

总计约 12–17 人日（M5+ 按需另计）。M1' 把最大不确定性（接口正确性）消灭在无网络、有现成测试网的阶段；M1'+M2 交付"curl 可驱动的 headless server"且 TUI 已是协议公民；M3 交付"类似 Codex App 的网页版"。

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
| O2 | 是否引入 WebSocket / gRPC | 暂不（v3 定案）；SSE+POST 覆盖当前全部交互；多协议走"新增适配器"路线，触发条件与要点见 §5.6 |
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

---

## 16. v2 现状核对与修订记录（2026-08-03）

### 16.1 核对方式

对照 v1 的"现状断言"逐条重读当前代码（`controller.go`、`bootstrap.go`、`process/types.go`、`runtimeevent/broker.go`、`subagent_bridge.go`、`cmd/loom/main.go`），确认哪些 gap 仍在、哪些已漂移、哪些是 v1 遗漏。

### 16.2 核对结论

| # | v1 断言 | v2 核对结果 | 处置 |
|---|---------|-------------|------|
| V1 | G1 无会话注册表 | 仍成立（`cmd/loom` 每进程 1 个 Controller） | 设计不变 |
| V2 | G2 仅 Goal/Plan cells 串台 | **范围扩大**：`SteerCell`（`bootstrap.go:223`，`handleSteer` 直读）与 `Questioner`（`ask_user` 注册时绑定）同为进程级会话态，v1 遗漏 | §2.2/§4.2 扩为四处；串台回归改为六路隔离断言 |
| V3 | G3 `controller.publishSessionEnv` 全局覆盖 | **v2 核对有误**（grep 遗漏）：该方法当时仍存在，`handleNewSession/handleResumeSession` 均调用它写全局原子值 | M1' 已删除该方法及其调用点，改为 runTurn ctx 注入（§18）；v2 的结论（chat 路径归因问题）以更准的方式成立 |
| V4 | G4–G7、G9、G10 | 仍成立（无 ReplayLog / Snapshot 水位 / actor / 幂等 / server 包 / flock） | 设计不变 |
| V5 | G8 `runAgent` 手工装配 | **部分收敛**：已复用 `Bootstrap`，仅 `agent.Loop` 装配仍手工 | G8 降级为低风险残留；M1 不再含 runAgent 收敛 |
| V6 | Broker 仅 channel 订阅 | 新增 `Observer` 接口与 `WithDurableQueue` 选项 | pump 直接用 `WithDurableQueue(4096)`，设计不变 |
| V7 | Controller API = v1 列出的 9 个方法 | **已扩张**：steer、`AnswerQuestion`、`SetModel/SetReasoning`、`RequestCompaction`、`SubagentView`、`ListSkills/ListMCPServers` | G12；§5.3 端点表扩展 |
| V8 | v1 未考虑 ask_user 重连 | `question.asked` 为 ephemeral 且快照无 `PendingQuestions` | G11；§4.4 加快照兜底、§9 契约同步 |
| V9 | v1 未考虑子代理事件路由 | `WireSubagentObserver` 把子 run 事件挂**父 session envelope** | 与 per-session 过滤/ReplayLog 天然兼容，无需新设计（§2.1 记录） |

### 16.3 v2 引入的待拍板决策

| # | 决策点 | 建议 |
|---|--------|------|
| D1 | `POST prompts` 在 turn busy 时的语义：v1 的 409 vs 对齐 TUI 的 steer 入队 | steer 入队（协议与 TUI 行为一致，Web 端无需发明新交互；响应体 `{steered:true, queue_len}` 直接映射 `SubmitResult`） |
| D2 | model/reasoning/compact/subagent/skills/mcp 端点的里程碑归属 | questions+steer 必须 M2（核心交互闭环）；model/reasoning/compact 建议 M2（均为 Controller 透传，成本极低）；subagent view/skills/mcp 可 M3 |
| D3 | `Snapshot.PendingQuestions` | 做（M1，与 `EventSeq` 同批落地） |
| D4 | 本期实施范围 | 建议先 M1+M2（curl 可驱动的 headless server 即达成本期"大活"的最小可用形态），M3 Web SPA 视反馈再启 |

> v3 注：D1–D3 采纳；D4 被 §17.4 的重排取代（TUI 先迁，M1' 取代原 M1）。

---

## 17. v3 架构定案记录（2026-08-03）

### 17.1 定案背景

v2 评审中围绕四个问题展开讨论并定案：(a) Controller 兼容策略（nil 回落 vs 单一路径）；(b) 是否对齐 codex 的 JSON-RPC/WS；(c) 是否抽象协议无关应用层、TUI 与其他 UI 平权；(d) 落地顺序（HTTP 先行 vs TUI 先迁）；(e) gRPC 可行性。本节记录结论与理由。

### 17.2 codex 对照要点（OpenAI codex，Rust 实现）

| codex 做法 | 对本设计的影响 |
|---|---|
| TUI 从第一天走 `AppServerClient`（InProcess/Remote 两实现），core 之上永远隔着 app-server 层 | 证明"TUI 平权 + 单一构造路径"终态正确；v3 采纳为 M1' 目标 |
| app-server 为每个 thread 显式构造 SessionConfiguration，无线程级 fallback | 终结 A/B 之争：Controller 会话态只来自 SessionRuntime |
| 审批建模为 JSON-RPC `ServerRequest`（带 request_id 等响应） | **语义采纳**（PendingRequest 统一抽象 + 一次性 Resolve），**传输不采纳**（见 §17.3） |
| 断线重连 = 全量历史快照 + 原子订阅 + 挂起请求重放（无全局事件序号） | 我们的 Broker seq + ReplayLog 增量补发更优，保留；G11 的 PendingRequests 快照兜底与其"挂起请求重放"同构 |
| Submission ID 用 UUID v7 兼作对外 turn_id | 记录备选；当前 `Idempotency-Key` 已够用 |

### 17.3 传输协议选型：REST+SSE 为适配器 #1

讨论过 JSON-RPC/WS（对齐 codex）与 gRPC（repo 已有工具链）两个替代，结论均为"作为后续适配器，不做首适配器"：

- **JSON-RPC/WS 不首选**：审批的 ServerRequest 语义我们用 PendingRequest + 一次性 Resolve 等价获得；WS 丢失 EventSource 自动重连、握手期 token 传递变丑、引入新依赖；我们也没有存量 JSON-RPC 客户端要兼容。触发条件见 §5.6。
- **gRPC 不首选**：明星客户端是浏览器（grpc-web 需翻译层且不支持 bidi streaming）；事件模型是 JSON 原生，proto 镜像带来双 schema 税。机器客户端出现时再加（§5.6）。
- **REST+SSE 首选**：本期客户端图谱（TUI-inproc / 浏览器 / curl）全优；零新依赖；事件零翻译；seq cursor 增量重连是 codex 不具备的优势。

### 17.4 落地顺序：TUI 先迁（原 M5 提前为 M1' 主体）

理由：(1) 接口从最难的真实消费者长出，完整性被证明而非先验设计；(2) 验收标准现成且严格——`ui_test.go` 原样跑绿；(3) 迁移是纯依赖注入改动，渲染/状态机不动；(4) 之后 REST+SSE 适配器沦为机械映射；(5) 顺序本身消解 A/B 之争——TUI 经 SessionService 拿会话后，fallback 路径没有存在机会，单一路径自然成立。

### 17.5 接口硬约束（防抽象泄漏）

1. client 接口全部请求/响应/事件类型 JSON 可序列化，roundtrip property test 守护；
2. inproc 实现禁共享可变引用（拷贝语义），保证 inproc/http 行为一致；
3. 应用层只出 typed 哨兵错误；幂等键、actor 为一等参数，不得藏在传输层概念里；
4. 事件订阅（pump/ReplayLog/补拉-订阅）实现于 SessionService，传输适配器只消费 `SubscribeEvents`。

---

## 18. M1' 落地记录（2026-08-03）

M1'（client 化）已完成并全量测试绿。本节记录与设计正文有出入的实现决策，正文相应处已同步修订。

### 18.1 落地内容对照

| 设计项 | 落地 | 偏差 |
|---|---|---|
| ToolRegistry overlay | `agent.NewOverlayRegistry`：Lookup 落穿 parent、Register 仅本地、List 合并且本地遮蔽 | 无 |
| SessionRuntime 四件套 | `app/session_runtime.go`：`NewSessionRuntime`（复用 bootstrap cells，存量路径）/ `NewIsolatedSessionRuntime`（全新 cells，serve 路径） | 新增隔离构造变体 |
| Controller 单一路径 | `c.runtime` 为唯一会话态来源；`ControllerConfig.Runtime` 为 nil 时从 bootstrap **派生**（复用其 cells），存量测试零改动；SessionService 恒传 isolated Runtime | "必填"软化为"派生"，单一路径不变量在 Controller 内部成立（永远只读 `c.runtime`） |
| ctx SessionEnv | `process.ContextWithSessionEnv/SessionEnvFromContext`；runner hook 签名升级为 `func(ctx)`；bootstrap hook 实现"ctx 优先、原子值回落"；runTurn 注入；`publishSessionEnv` 及其调用点已删除 | `StartSession/StartSessionWithGrant` 原无 ctx 参数（设计假设有），已补；调用链（exsession Manager.Start 本就有 ctx）已贯通 |
| Snapshot 水位 | `appliedSeq` 在全部投影更新临界区采样（checkpoint、runTurn、审批卡、问答卡）；`Snapshot.EventSeq` | 无 |
| PendingRequests | `Snapshot.PendingRequests`（审批卡含完整 `ApprovalRequestedPayload`，问答含完整 `domain.Question`），投影源为 controller 的 `pendingCards/pendingQuestions` | 保留 `PendingApprovals` 字段（TUI 消费中），统一抽象以新增字段方式落地 |
| actor | `ResolveApprovalWithActor`；actor 经 `approvalActors` 投影透传进 `approval.resolved` payload（新增 optional `actor` 字段）并落审计日志 | 无 |
| ReplayLog | `runtimeevent/replay.go`：`Since` 按量值比较（允许洞）；`maxEvicted`/`maxSeen` 双判负（旋转出界/未来 cursor）；property 测试覆盖 500 seq × 全 cursor 扫描 | 无 |
| SessionService | `app/session_service.go`：注册表/单例不变量/幂等 LRU(128)/全局并发 turn 闸门(4)/空闲回收(30min)/pump+补拉-订阅原子衔接/慢订阅断开/pump 断连重订阅+全量断流 | 幂等命中返回 `deduplicated=true`；闸门仅拦新 turn（steer 不受限） |
| internal/client | `client.Client`（21 方法，session 作用域）+ 类型别名（app 类型为 canonical）+ `NewInProc`；JSON roundtrip 约束测试（含 Snapshot/PendingRequest 全字段样本） | 接口方法集按 TUI 真实用量定稿（含 ListCheckpoints/Rewind/SubagentView/ListSkills/ListMCPServers/ListRules/ForgetRule） |
| TUI 迁移 | ui 的 `controller` 字段类型改为 `client.Client`（名称保留）；`subscribeEvents` 适配器把 ctx 取消包装为 unsubscribe func；`loom chat` 装配 = SessionService + inproc（与 serve 同一套） | SubmitPromptWithImages 在 client 接口中统一为 `SubmitPrompt(ctx, prompt, images)` |

### 18.2 测试证据

- `ui_test.go` 原样全绿（2594 行，走 client 接口）；
- `TestServeSessionsDoNotCrossTalk`：六路隔离回归锁（goal/plan/steer/question/SessionEnv/approval），bootstrap 携带 trap cells 捕获任何进程级泄漏；
- `TestControllerInjectsSessionEnvViaTurnContext`：ctx 归因 + 进程级原子值零写入；
- ReplayLog property 测试（500 seq 全 cursor 扫描，不重不漏）；
- client 类型 JSON roundtrip 约束测试；
- `bazel test //go/...` 全绿。

### 18.3 遗留到 M2 的已知事项

- `SubscribeEvents` 尚不携带 `server.resync`/`server.draining` 线级事件与 instance ID（M2 SSE 适配层补，应用层语义已具备）；
- `loom serve` 子命令、flock、token、审计日志未开始（M2 范围）；
- direct 模式（chat/run）数据目录 flock 未加（R5/O7，M4）；
- 审批超时自动 deny（§4.6，`LOOM_SERVE_APPROVAL_TIMEOUT`）未实现，属 M2 server 侧定时器。

---

## 19. M1' 审查修复与真实模型验收记录（2026-08-04）

### 19.1 独立审查与处置

M1' 落地后经过一轮独立代码审查（并发正确性/overlay/迁移/兼容性六个风险面），全部发现与处置：

| # | 发现 | 严重度 | 处置 |
|---|------|--------|------|
| H1 | TUI 会话切换（/new、/resume、选择器）后事件流断裂：client 订阅绑定会话，旧实现订阅 broker 全局流 | 高 | `handleSessionSwitched` 成功分支重建订阅（`update.go`）；订阅语义差异是本次迁移唯一真实行为变化 |
| H2 | `ReplayLog.Since` 的"未来 cursor 判负"基于 per-session maxSeen，与 `Snapshot.EventSeq` 的全局水位契约矛盾——多会话下"快照→订阅"必然误拒 | 高 | 判负上移：`Since` 只管旋转出界；`SubscribeEvents` 对"超全局 broker 水位"判负（前一世代 cursor）；回归锁 `TestSessionServiceSnapshotWatermarkHandoff` |
| M1 | actor 在 ResolveApproval 之后写入，与 turn goroutine 的持久化存在时序竞态 | 中 | actor 先写、失败回滚（`handleResolveApproval`） |
| M2 | turn 取消时 pendingCards/pendingQuestions/approvalActors 幽灵残留 | 中 | `onTurnFinished` 统一清空三投影；`handleAnswerQuestion` 未命中也清（自愈）；`handleResumeSession` 对称重置 |
| M3 | `publishForEvent` 权限分支的中间采样使水位瞬态越过 messages 投影 | 中 | 批次内只更新投影不采样，统一由批次末（checkpoint/runTurn）采样——水位宁落后不超前 |
| M4 | sweeper 回收会话不断开订阅者 | 中 | 回收先 `dropSubscribers` |
| M5 | TUI 断线重订阅恒从 0 全量回放，delta 重复追加 | 中 | Model 跟踪 `lastEventSeq`，重订阅/会话切换从该 cursor 续订 |
| M6 | pump 断连重订阅间隙静默丢事件 | 中 | `ReplayLog.Invalidate(floor)` 毒化 pre-gap cursor + `SubscribeLatest` 快照后纯直播重挂 + inproc 自动回落 |
| M7 | 幂等键 check-then-act 竞态 | 中 | single-flight（`idemInFlight`），并发重试共享首个执行结果 |
| L1–L9 | gofmt、快照深拷贝、nil channel 挂起、roundtrip 样本不全等 | 低 | 已修（`Controller.Subscribe` 死代码已删；订阅失败返回 closed channel 驱动重试路径） |

### 19.2 真实模型验收（LOOM_E2E_LLM=1，真实 provider）

`go test -run TestServeRealModelE2E ./pl/loom/e2e/`（52s）七项全过：

1. 真实工具循环：模型经 read_file 读出只有读文件才能知道的口令（49 个事件）；
2. 事件流经 SessionService pump/订阅（turn.started/response_completed/turn.finished/tool.*）；
3. 快照水位衔接：以 `Snapshot.EventSeq` 订阅成功且直播不断；
4. steer：忙时投稿入队并注入后续模型调用；
5. 幂等：同 key 重投 `deduplicated=true`，不新增 turn；
6. 恢复：新 client ResumeSession 后 5 个 turn 的 transcript 完整；
7. 审批/问答请求自动决议链路可用（审批在策略要求时经事件→决议闭环）。

headless 路径冒烟：`loom run` 真实模型应答正常（session 持久化、MCP/rules/subagent 装配无回归）。

### 19.3 验收结论

M1' 达到 §12 验收标准：ui_test 原样全绿（走 client 接口）、六路串台回归绿、JSON roundtrip 约束绿、`bazel test //go/...` 全绿、真实模型 e2e 验收通过。可以开工 M2。
