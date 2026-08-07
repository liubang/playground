# Loom 多 Workspace 设计（单用户）

| 项目 | 内容 |
|------|------|
| 状态 | v1（已实现；schema 版本实际为 v5——v4 已被 `archived_at` 迁移占用） |
| 日期 | 2026-08-06 |
| 关联文档 | `DESIGN.md`（§4 总体架构）、`SERVE_DESIGN.md`（§1.1 非目标、§4 装配）、`CONFIG_DESIGN.md`、`PERMISSION_DESIGN.md`、`MEMORY_DESIGN.md`、`SKILL_DESIGN.md` |
| 范围 | P1：单用户多 workspace。user 体系不实现，仅在模型与存储层预留演进接缝（§17） |

---

## 1. 背景与动机

loom 今天的"工作区"不是一个实体，而是启动时解析出的一个根路径：`cmd/loom/main.go` 的 `resolveWorkspace()`（显式 flag → `BUILD_WORKSPACE_DIRECTORY` → cwd）算出 root，经 `BootstrapConfig.WorkspaceRoot` 传入 `NewBootstrap`，此后进程内一切与目录相关的组件——`PathValidator`、project rules、prompt builder、skills 发现、子代理工厂——全部绑定这唯一 root。

`SERVE_DESIGN.md` 因此把 `loom serve` 限定为"启动时绑定单一 workspace"（§1.1 非目标）。这带来两个真实痛点：

1. **一个 serve 进程服务不了多个项目**：在 monorepo 子项目、多仓库并行开发时，用户必须为每个目录各起一个 serve（各自占用端口与数据目录锁），Web 客户端无法统一查看；
2. **会话没有项目归属**：session 列表是进程内全部历史的平铺，无法回答"这个 session 是在哪个项目里跑的"，跨进程重启后历史 session 与目录的关联彻底丢失。

与此同时，现有架构已经为多 workspace 做好了大部分准备（§4 详列）：TUI 与 serve 共用同一套 `SessionService` 装配（`main.go:296-300`，SERVE_DESIGN §10 的"平权客户端"已落地）；per-session 状态已收敛进 `SessionRuntime`（`session_runtime.go:36`）；`PathValidator`、rules、prompt 都是"以 root 为参数的纯装配"，没有任何组件要求进程与 workspace 一一绑定。

本设计将 workspace 提升为一等实体：**有 ID、有注册表、可持久化、session 创建时绑定且终身不变**，同时保持单用户语义与现有 CLI/TUI 行为零变化。

## 2. 目标与非目标

### 2.1 目标

1. `loom serve` 单进程承载多个 workspace，各 workspace 的 session 并发推进、互不越界；
2. session 持久化归属关系，跨进程重启后可按 workspace 检索历史；
3. workspace root 变更（移动/删除）有明确的失败语义，不静默写错目录；
4. `loom chat` / `loom run` 行为与今天完全一致（隐式 default workspace）；
5. 为后续 user 体系预留接缝，但不预先实现（§17）。

### 2.2 非目标（本期不做）

- 多用户/多租户、认证身份体系（沿用 SERVE_DESIGN §1.1：server 面向单机单用户，token 仅防误连）；
- session 跨 workspace 迁移（§3.3 论证不可变绑定；提供 fork 语义属后续）；
- workspace 级配额（session/turn 配额仍为进程级，user 化时再按归属拆分）；
- workspace 间资源共享（artifact、memory 的归属维持现状，见 §9 矩阵）；
- workspace 归档的生命周期管理（删除已实现为纯元数据移除，见 §16.1；归档暂不提供）。

## 3. 核心概念与领域模型

### 3.1 实体

```go
// internal/domain

// WorkspaceID 复用 loom 现有的 ID[T] 泛型机制（ids.go:29-35）：
// type workspaceIDT string; type WorkspaceID = ID[workspaceIDT]，
// 生成形如 "ws_<32 hex>"（newID("ws")），与 sess_/run_/evt_ 同构。
// 不用 hash(root_path)：root 可移动，ID 必须与路径解耦（§7.4）。
type WorkspaceID = ID[workspaceIDT]

// Workspace 是一个有独立根路径、独立规则/提示词/技能装配、
// 可归属多个 session 的逻辑项目单元。不是 git repo 的同义词
// （root 可以是 repo 子目录），也不是进程沙箱。
type Workspace struct {
    ID        WorkspaceID `json:"id"`
    Name      string      `json:"name"`       // 展示名，默认 root basename
    RootPath  string      `json:"root_path"`  // canonical（EvalSymlinks 后），全库唯一
    CreatedAt time.Time   `json:"created_at"`
    UpdatedAt time.Time   `json:"updated_at"`
}
```

### 3.2 关系与不变式

```text
Workspace 1 ──── n Session   （创建时绑定，终身不可变）
```

- **W1（绑定不可变）**：session 创建时持久化 `workspace_id`，之后不可更改。理由：`file_changes` 表记录的是 workspace 相对路径（rewind 依赖其重放）；`permission.AttachRules` 与 prompt builder 在装配期按 root 构建。允许迁移会使历史 change ledger 语义错乱；
- **W2（root canonical 唯一）**：注册时 `filepath.Abs` + `EvalSymlinks` 规范化，`root_path` 加 UNIQUE 约束。`/a/b` 与 `/tmp/link-to-a-b` 解析到同一 workspace。注意这与 `PathValidator` 的行为一致——`NewPathValidator`（path.go:77-87）本就 canonical 化 root，用户从 symlink 路径启动时 validator root 是物理路径，现状即如此；
- **W3（嵌套允许）**：workspace root 可以互为前缀（monorepo 场景：`/repo` 与 `/repo/sub` 各自注册）。二者的 `PathValidator` 边界独立；`/repo` 的 session 本就可以写 `/repo/sub`，与今天单 workspace 行为一致，不构成新风险；
- **W4（root 不可换绑）**：MVP 不提供 repair/换绑 API。目录被移动 = 注册新 workspace，旧 workspace 的 session 保留只读历史（transcript 可查，resume 报 `ErrWorkspaceUnavailable`）。换绑的真实痛点与 rewind 风险见 §16；
- **W5（default workspace）**：每个进程启动时按现有 `resolveWorkspace()` 逻辑解析出 default root，幂等注册（已存在则复用）。所有不显式指定 workspace 的入口（旧客户端、CLI）都落在它上面；
- **W6（session 删除不影响 workspace）**：`1—n` 关系靠 session 侧的 `workspace_id` 列维系；`DeleteSession`（handlers_sessions.go:104）只删 session 及其级联数据，workspace 实体与其它 session 不受影响。空 workspace（零 session）合法存在。

### 3.3 为什么 session 绑定不可变

除了上述 file_changes/rules/prompt 的技术约束，还有一层语义约束：session 的 system prompt、可用工具集、审批规则都是 workspace 装配的函数。一个跑着 `/repo-A` 规则的 session 中途切到 `/repo-B`，等价于运行时替换整个运行时上下文，其复杂度远超"在 B 下新开 session"。跨 workspace 复用上下文的正当需求由"fork transcript 到新 workspace"满足（后续，不本期）。

## 4. 现状：`WorkspaceRoot` 绑定点清单

`Bootstrap`（`internal/app/bootstrap.go`）当前一次性装配全部组件。下表按与 root 的耦合方式分类（行号基于撰写时代码，实施前需复核）：

| # | 组件 | 位置 | root 耦合方式 | 目标层 |
|---|------|------|--------------|--------|
| B1 | `PathValidator` | `bootstrap.go:190` | 直接以 root 构造 | **workspace** |
| B2 | `process.Runner` | `bootstrap.go:203` | 持有 validator（沙箱边界 = root） | **workspace** |
| B3 | base `ToolRegistry` 内建工具 | `bootstrap.go:242` `registerBuiltinTools` | 工具闭包持有 validator/runner/book | **workspace** |
| B4 | `FileStateBook` | `bootstrap.go:228` | drift 检测按路径记账 | **workspace** |
| B5 | `exsession.Manager` | `bootstrap.go:237` | 持有 runner | **workspace** |
| B6 | `Policy`（project rules） | `bootstrap.go:268` `AttachRules` | project 层从 root 读 `.loom/rules`；user 层从 `RulesDir()` 读，共享 | **workspace**（user 层规则在进程级预载） |
| B7 | `PromptBuilder` + `FileRulesProvider` | `bootstrap.go:310,329` | 读 `<root>/LOOM.md`；user 层 `~/.loom/LOOM.md` 共享 | **workspace** |
| B8 | Skills 发现 | `bootstrap.go:320` `WireSkills` | repo-scope 扫 `<root>/.loom/skills`；user-scope roots 共享 | **workspace**（user roots 参数由进程级传入） |
| B9 | Subagent factory/manager + researcher/coder prompt | `bootstrap.go:381-448` | 子 registry 持 validator/runner；prompt 绑 root；`factory.Workspace` 字段 | **workspace** |
| B10 | MCP servers | `bootstrap.go:337-368` | 无 root 耦合：`srv.Cwd` 为 config 显式值，空则继承进程 cwd | **process**（§9 决策 D2） |
| B11 | `SessionStore` / `ArtifactStore` | `bootstrap.go:177,183` | 无 | **process** |
| B12 | Model gateway / trace.Recorder | `bootstrap.go:280-301` | 无 | **process** |
| B13 | `SessionRules` / `RememberedStore` | `bootstrap.go:253-265` | user-global（`RulesDir()`），现状即跨目录共享 | **process**（§16 开放问题） |
| B14 | `MemoryStore`/`MemoryExtractor`/`MemoryConsolidator` | `bootstrap.go:499`（字段 `:152-160`） | user-global（`MemoriesDir()`） | **process**（§16 开放问题） |
| B15 | `AtomicSessionEnv` | `bootstrap.go:202` | 已按 ctx 注入 per-session 归因（SERVE_DESIGN G3 已解） | **process** |
| B16 | `GoalCell/PlanCell/SteerCell/Questioner` | `session_runtime.go:36-45` | per-session | **session**（不变） |
| B17 | `Bootstrap.Questioner`（进程级兜底） | `bootstrap.go:115-117,233-236` | headless 路径的 `AutonomousQuestioner` 实例 | **process**（作为 `newSessionRuntime` 的 fallback，见 `session_runtime.go:96`） |

入口路径（`cmd/loom/main.go`）：chat（`:281`）、headless run（`:411`）、serve（`:503`）三处都以 `resolveWorkspace()` 结果构造同一个 `BootstrapConfig`。改造后三处统一改为"解析 default root → 注册/复用 default workspace"。

## 5. 总体设计：三层装配

```text
┌──────────────────────────── loom 进程 ────────────────────────────┐
│ ProcessRuntime（进程级单例）                                       │
│   Store / Artifact / Models / Recorder / Logger /                │
│   RememberedStore / MemoryStore / MCPManager / SessionEnv        │
│                                                                   │
│   WorkspaceRegistry（§6）                                         │
│   ┌────────────────────┐   ┌────────────────────┐                 │
│   │ WorkspaceRuntime A │   │ WorkspaceRuntime B │  懒加载，常驻    │
│   │  Validator/Runner  │   │  Validator/Runner  │  （LRU 留后续）  │
│   │  Registry/Policy/  │   │  Registry/Policy/  │                 │
│   │  Prompt/Skills/    │   │  Prompt/Skills/    │                 │
│   │  Subagent/ExSession│   │  Subagent/ExSession│                 │
│   └─────────┬──────────┘   └─────────┬──────────┘                 │
│             │ 每 session 一个         │                            │
│   ┌─────────▼──────────┐   ┌─────────▼──────────┐                 │
│   │ SessionRuntime     │   │ SessionRuntime     │  cells/overlay   │
│   │ (goal/plan/steer/  │   │ (goal/plan/steer/  │  （现状不变）     │
│   │  ask_user overlay) │   │  ask_user overlay) │                  │
│   └────────────────────┘   └────────────────────┘                 │
└───────────────────────────────────────────────────────────────────┘
```

### 5.1 Bootstrap 的拆分

现有 `Bootstrap` 结构体按 §4 表格拆为两个结构：

```go
// internal/app

// ProcessRuntime 持有与 workspace 无关的进程级资源，
// 生命周期 = 进程。由 NewProcessRuntime（现 NewBootstrap 改名拆分而来）构建。
type ProcessRuntime struct {
    Resolved        *config.ResolvedConfig
    Current         config.ProviderModelRef
    Store           domain.SessionStore
    Artifact        domain.ArtifactStore
    Recorder        trace.Recorder
    Logger          *slog.Logger
    Version         string
    SessionEnv      *process.AtomicSessionEnv
    SessionRules    *permission.SessionRules
    RememberedStore *permission.RememberedStore
    MCPManager      *mcp.Manager
    MemoryStore     *memory.Store
    MemoryExtractor *memory.Extractor
    MemoryConsolidator *memory.Consolidator
    // Questioner 是 headless 路径的 AutonomousQuestioner 兜底（B17）；
    // SessionRuntime 构造时 session 级 ChannelQuestioner 优先于它。
    Questioner      domain.Questioner
    // ... 其余 B10-B15 字段
    traceProvider   *trace.Provider
}

// WorkspaceRuntime 持有一个 workspace 的全部装配产物。
// 由 WorkspaceRegistry 懒加载构建；P1 常驻，进程退出时统一 Close。
type WorkspaceRuntime struct {
    WS               domain.Workspace
    Validator        *workspace.PathValidator
    Runner           *process.Runner
    Registry         *agent.ToolRegistry  // base registry（B3）
    Policy           agent.Policy         // 含 project rules（B6）
    permissionPolicy *permission.Policy
    PromptBuilder    agent.PromptBuilder  // B7
    Skills           *SkillsHandle        // B8
    FileStateBook    *workspace.FileStateBook
    SessionManager   *exsession.Manager   // B5
    SubagentFactory  *subagent.Factory    // B9
    SubagentManager  *subagent.Manager
    SubagentModels   *subagent.ModelSource
}

func (rt *WorkspaceRuntime) Close() error // SessionManager/SubagentManager 回收
```

`NewBootstrap` 拆为 `NewProcessRuntime(ctx, resolved, cfg)` + `WorkspaceRegistry.Resolve(ctx, workspaceID)`（懒加载，§6）。装配代码本身几乎原样搬运，只是入参从 `cfg.WorkspaceRoot` 换成 `ws.RootPath`，依赖从 `Bootstrap` 字段换成 `ProcessRuntime` 字段。现有测试的改造量集中在 `app` 包的构造夹具。

### 5.2 SessionRuntime 与 Controller

- `SessionRuntime`（`session_runtime.go`）**结构不变**；`NewIsolatedSessionRuntime(b *Bootstrap, ...)` 的签名改为接收 `(proc *ProcessRuntime, ws *WorkspaceRuntime, questioner)`，overlay 的 base registry 取自 `ws.Registry`；
- `ControllerConfig.Bootstrap *Bootstrap` 改为 `{Process *ProcessRuntime, Workspace *WorkspaceRuntime}`。Controller 内部所有 `b.Validator/b.Policy/...` 引用改走对应层；
- `SessionHandle` 增加 `WorkspaceID domain.WorkspaceID` 字段，供列表过滤与归属断言。

### 5.3 SessionService 改造

```go
// 签名变化（internal/app/session_service.go）
func NewSessionService(proc *ProcessRuntime, reg *WorkspaceRegistry, broker *runtimeevent.Broker, cfg SessionServiceConfig) *SessionService
func (s *SessionService) CreateSession(ctx context.Context, workspaceID domain.WorkspaceID) (*SessionHandle, error)
func (s *SessionService) ResumeSession(ctx context.Context, id domain.SessionID) (*SessionHandle, error) // workspace_id 从 store 读出后 Resolve
```

- `CreateSession`：`registry.Resolve(workspaceID)` → `newHandle(wsRT)` → `Controller.NewSession`（持久化时写入 `workspace_id`）。**底层 `SessionStore.CreateSession(ctx, sessionID)` 的签名需增加 `workspaceID` 参数**（`sqlite_store.go` 的 `INSERT INTO sessions` 携带该列），调用点为 `Controller.NewSession`（app）与 `main_test.go` / `subagent_v2_e2e_test.go` 等构造夹具——测试夹具统一传 default workspace ID；
- `ResumeSession`：先从 store 读 session 归属（需要 `SessionStore` 新增轻量查询，§7.3），再 Resolve 对应 workspace。**子代理 session 的父归属**随之天然成立：子 session 的 `run.created` 事件由父 workspace 的 subagent Manager 写入，其 `workspace_id` 与父 session 相同（子代理在父的 `WorkspaceRuntime` 上装配，`SubagentFactory` 属于 workspace 层）；**workspace root 已不可达时返回 `ErrWorkspaceUnavailable`**，transcript/inspect 类只读路径不受影响（它们不走 runtime 装配）；
- `newHandle` 失败路径沿用现有的 Shutdown 清理（`session_service.go:263-265` 的模式）；
- `ListSessions` 增加可选 `workspaceID` 过滤参数。

### 5.4 并发与生命周期

- 同一 workspace 的多 session 共享该 workspace 的 `Validator/Runner/Policy/FileStateBook`——与今天多 session 共享 Bootstrap 对应字段的并发语义完全一致（这些组件现状即线程安全）；
- 不同 workspace 的 session 之间零共享可变状态（各自 validator/runner/book/registry），隔离性**强于**现状；
- `WorkspaceRegistry` 内部 `sync.Mutex` 保护 `byID/byRoot` 两张索引；`Resolve` 的懒加载路径用 single-flight 防并发重复装配（装配含 skills 扫描等 I/O，并发 `Resolve` 同一 workspace 必须只装一份）；
- 进程关闭顺序：SessionService.Close（drain sessions）→ WorkspaceRegistry.Close（逐 workspace Close）→ ProcessRuntime.Close（store/artifact/MCP/trace）。

## 6. WorkspaceRegistry

```go
// internal/app/workspace_registry.go

type WorkspaceRegistry struct {
    mu      sync.Mutex
    byID    map[domain.WorkspaceID]*WorkspaceRuntime
    byRoot  map[string]domain.WorkspaceID // canonical root → id
    proc    *ProcessRuntime               // 装配依赖
    store   domain.WorkspaceStore         // 持久化（§7）
    buildSF singleflight.Group // golang.org/x/sync 已在 go.mod（indirect），引入仅转为直接依赖
}

// Register 注册（或按 canonical root 复用）一个 workspace，完成持久化与索引，
// 并立即装配返回 runtime（eager：注册即用，default workspace 启动路径依赖此语义）。
// root 必须存在且为目录；name 为空取 basename。
func (r *WorkspaceRegistry) Register(ctx context.Context, root, name string) (*WorkspaceRuntime, error)

// Resolve 按 ID 取 runtime；未装配时懒加载（进程重启后内存索引为空，从 store
// 读实体再装配；root 必须仍存在且 canonical 一致，否则 ErrWorkspaceUnavailable）。
// Resolve = get-or-lazy-build，是唯一的装配入口（single-flight 去重）。
func (r *WorkspaceRegistry) Resolve(ctx context.Context, id domain.WorkspaceID) (*WorkspaceRuntime, error)

// List 返回全部已注册 workspace（含未装配的，直接读 store）。
func (r *WorkspaceRegistry) List(ctx context.Context) ([]domain.Workspace, error)
```

**P1 不做 LRU 回收**：workspace runtime 均为内存对象（MCP 子进程在进程级，见 §9-D2），常驻成本是每份 rules/skills catalog/prompt builder 的内存，量级可忽略。回收（连同 refcount、`WorkspaceRuntime.Close` 的调用时机）留作后续优化，结构上的 `Close` 方法本期就位。

**健康检查时机**：`Resolve` 时 `os.Stat(root)` + canonical 一致性校验（防止注册后目录被 symlink 替换）。`Register` 时同样校验。失败语义：`ErrWorkspaceUnavailable`（typed error，transport 层映射 409/410）。

## 7. 存储设计（schema v5）

> 实现备注：现有库的 `schema_migrations` 已到 v4（`archived_at`），故本期 workspace 迁移落为 **v5**（`migrateV5`），下文所有 v4 字样均指 v5。

### 7.1 迁移内容

沿用 `schema_migrations` 版本机制（`sqlite_store.go:133` 起的 v2/v3 先例）：

```sql
-- v5
CREATE TABLE IF NOT EXISTS workspaces (
    workspace_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    root_path TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    created_at_unix_nano INTEGER NOT NULL,
    updated_at TEXT NOT NULL,
    updated_at_unix_nano INTEGER NOT NULL
);
ALTER TABLE sessions ADD COLUMN workspace_id TEXT NOT NULL DEFAULT '';
CREATE INDEX IF NOT EXISTS idx_sessions_workspace_updated
    ON sessions(workspace_id, updated_at_unix_nano DESC);
```

- events/checkpoints/file_changes/artifact_refs 不动：归属经 `session_id` 间接获得，避免每行事件冗余；
- `workspace_id` 不加外键：workspace 实体可能被删除（未来），历史 session 必须保留（对齐现有 `archived_at` 软删除语义）。

### 7.2 两阶段迁移：schema 与 backfill 分离

`OpenSQLiteStore` 打开 DB 时**不知道**进程的 default root（store 装配早于 workspace 解析），因此：

1. **阶段一（OpenSQLiteStore 内）**：纯 schema 迁移，`workspace_id` 默认 `''`；
2. **阶段二（进程装配期）**：default workspace 注册成功后，执行一次幂等 backfill：

```sql
UPDATE sessions SET workspace_id = ?1 WHERE workspace_id = '';
```

CLI 每次启动都执行该语句（幂等，`workspace_id=''` 的行只会越来越少）。**并发安全**：同一 base_dir 同时只有一个写进程（data-dir 排他锁 `loom.lock`，见 `ResolvedStorage.SessionsDir` 注释与 serve 的 `lock.go`），backfill 不存在多进程竞态。**已知让步**：如果用户长期在多个目录下使用 chat 后又升级，全部历史 session 会归属到"升级后第一次启动的 default workspace"。这是可接受的——归属信息此前根本不存在，任何启发式（如按 cwd 猜）都更错；文档与 release note 中说明。

只读打开路径（`sqlite_store.go:81` 的 read-only 模式）的版本检查同步接受 v5。

### 7.3 WorkspaceStore 接口

```go
// internal/domain

type WorkspaceStore interface {
    UpsertWorkspace(ctx context.Context, ws Workspace) error           // 按 root_path 幂等
    GetWorkspace(ctx context.Context, id WorkspaceID) (Workspace, error)
    GetWorkspaceByRoot(ctx context.Context, canonicalRoot string) (Workspace, error)
    ListWorkspaces(ctx context.Context) ([]Workspace, error)
    // SessionWorkspace 是 ResumeSession 的轻量归属查询（不加载事件）。
    SessionWorkspace(ctx context.Context, sessionID SessionID) (WorkspaceID, error)
    // BackfillSessionWorkspaces 是阶段二迁移（§7.2）。
    BackfillSessionWorkspaces(ctx context.Context, id WorkspaceID) (int64, error)
}
```

由 `session.SQLiteStore` 实现（同一个 DB 连接，复用单连接串行化）；`SessionSummary` 增加 `WorkspaceID` 字段（JSON additive，前端可直接展示）。

### 7.4 Workspace ID 与 root 变更语义

- ID 随机生成，与路径解耦。`Register` 的 dedupe 键是 canonical root（`byRoot` 索引 + DB UNIQUE 约束双保险）；
- root 被 `mv`：旧 workspace 的 `Resolve` 失败（`ErrWorkspaceUnavailable`），新路径可走 `Register` 成为新实体。历史 session 的 inspect/transcript 可读，resume 被拒绝；
- root 被删除后重建（同路径）：canonical 一致，`Resolve` 恢复成功。`file_changes` 里的相对路径在新内容下的 rewind 风险与今天"进程重启后 rewind"完全同构，不引入新问题。

## 8. API 与客户端

### 8.1 REST 端点（`internal/server`）

```
GET    /v1/workspaces              # 列举（含每 workspace 的 session 计数）
POST   /v1/workspaces              # 注册 {root_path, name?} → 201（新建）/ 200（canonical 复用）/ 400（root 不存在、非目录或 canonical 解析失败）
GET    /v1/workspaces/{id}         # 详情
DELETE /v1/workspaces/{id}         # 删除元数据（§16.1）→ 204 / 404（未知 id）/ 409（default workspace 或有存活会话）
POST   /v1/sessions                # 请求体新增可选 workspace_id；省略 → default workspace
GET    /v1/sessions?workspace_id=  # 过滤（与既有 archived/limit/cursor 参数正交，handlers_sessions.go:50-62）

# 不提供 GET /v1/workspaces/{id}/sessions 子资源端点：与 ?workspace_id= 查询参数
# 语义完全重复，只保留查询参数一种表达，控制 API 面。
```

- 协议**不 bump**：全部 additive（新端点 + 可选字段），`ProtocolVersion` 保持 1。旧客户端创建 session 不带 `workspace_id`，落 default workspace，行为与今天一致；
- 错误模型沿用 typed sentinel（`session_service.go:38-52` 的模式）：新增 `ErrWorkspaceNotFound`（404）与 `ErrWorkspaceUnavailable`（410 Gone：实体存在但 root 已不可达，用于 `Resolve`/resume 路径）。**不设 `ErrWorkspaceRootConflict`**——canonical 重复按 W2 语义复用返回 200，不存在冲突场景；注册时 root 校验失败是普通的 400 invalid input；
- SSE 事件协议不变（session 事件自带 SessionID，路由不变）；session 元信息事件（run.created 一类）的 payload 增加 `workspace_id` 字段，additive；
- 审计日志（`AuditLogger`）在 session create/resume 行追加 `workspace_id`/`workspace_root`。

### 8.2 client 包

`internal/client.Client` 接口增加：

```go
ListWorkspaces(ctx context.Context) ([]domain.Workspace, error)
RegisterWorkspace(ctx context.Context, root, name string) (domain.Workspace, error)
DeleteWorkspace(ctx context.Context, id domain.WorkspaceID) error          // §16.1
CreateSessionIn(ctx context.Context, workspaceID domain.WorkspaceID) error // CreateSession 的带参版
```

`InProc` 实现直连 SessionService；HTTP 实现走 §8.1 端点。TUI 通过 `client.NewInProc`（`main.go:300`）无感升级。

## 9. 组件归属矩阵与显式决策

| 决策 | 内容 | 理由 |
|------|------|------|
| **D1** | base `ToolRegistry` 整体随 workspace 装配 | 内建工具闭包持有 validator/runner/book（B2/B3/B4），拆"无状态工具共享"的优化收益小、边界易错，不本期做 |
| **D2** | MCP 保持进程级单例 | MCP 配置在 user-global config，`srv.Cwd` 显式；workspace 级装配会导致 N workspace × M server 子进程爆炸。MCP 工具本就不经 `PathValidator`，无越界语义变化。per-workspace MCP 留 §16 |
| **D3** | `FileStateBook` per-workspace | 现状多 session 共享一个 book；按 workspace 拆分后隔离性更强（A 的 session 看不到 B 文件的 drift 状态），且 rewind 恢复时的 book 更新天然按 workspace 分组 |
| **D4** | `RememberedStore`（"allow always" 记忆）维持 user-global | 与现状一致；按 workspace 隔离更合理（A 项目的 `make deploy` 记忆不应作用于 B），但属于 PERMISSION_DESIGN 的范畴演进，不在本设计内改 |
| **D5** | `MemoryStore` 维持 user-global | `memory.OpenStore(MemoriesDir())` 现状即全局；记忆按 workspace 分桶是 MEMORY_DESIGN 的演进，本设计只在 §17 预留 `workspace_id` 关联列的演进方向 |
| **D6** | skills user-scope roots 进程级预载，repo-scope 随 workspace | `WireSkills` 的 `userRoots` 参数由 ProcessRuntime 算好传入，每个 WorkspaceRuntime 只替换 `workspaceRoot` 参数（`skills.go:78` `skill.NewLoader` 的第一参） |
| **D7** | `SessionEnv` 归因不变 | per-session ctx 注入已实现（`bootstrap.go:213-218`），追加 `LOOM_WORKSPACE_ID` 环境变量（additive）供下游 CLI 归因 |

## 10. 配置演进

`config.File` 新增可选顶层段（`internal/config/schema.go`）：

```yaml
workspaces:
  - name: playground
    root: ~/workspace/github/liubang/playground
  - name: loom
    root: ~/workspace/github/liubang/playground/go/pl/loom   # 嵌套，合法（W3）
```

- 语义：启动时预注册（幂等 dedupe），仅是"免手动注册"的便利；`Register` 的校验规则（存在、目录、canonical）同样适用，失败记 warn 不阻断启动；
- default workspace 不在列表中也会自动注册（W5）；
- `resolve.go` 的 `ResolvedConfig` 增加 `ResolvedWorkspaces []ResolvedWorkspace`。**root 的 `~` 展开是新增能力**：config 现状无任何 `~` 展开（`resolveStorage` 只做 `filepath.Abs`，resolve.go:174），`skills.extra_roots` 同样不展开。因此 `ResolvedWorkspace` 的 root 解析需在 load 期做 `~` → `$HOME` 前缀替换 + `Abs` + `EvalSymlinks`（先展开再 Abs，`~` 开头才替换），与 canonical 校验的规则对齐。若决定不支持 `~`，YAML 示例中的 `~/...` 写法必须改为绝对路径；
- 其余配置项归属不变（limits/approval 进程级、rules.user 层共享、tracing 进程级）。

## 11. 前端影响

### 11.1 TUI（`loom chat`）

- 行为零变化：`resolveWorkspace()` → default workspace → `CreateSessionIn(defaultID)`；
- session picker 的数据已带 `WorkspaceID`（`SessionSummary` 新字段），本期不改动 picker UI（default workspace 下只有一个值）；

### 11.2 headless（`loom run`）

同 TUI，走 default workspace。`BUILD_WORKSPACE_DIRECTORY` 语义不变。

### 11.3 Web SPA（`internal/server/web/static`）

最小可用改动：

- sidebar 顶部加 workspace 选择器（`GET /v1/workspaces`），选中后 session 列表带 `?workspace_id=` 过滤；
- 新建 session 按钮在当前选中的 workspace 下创建（`POST /v1/sessions {workspace_id}`）；
- session 条目显示 workspace 名（`SessionSummary.WorkspaceID` → 名称映射）；
- "注册新 workspace"入口：输入绝对路径的极简表单（`POST /v1/workspaces`）。不做目录浏览 picker（server 端文件浏览是另一个安全话题，§16）。

### 11.4 CLI 子命令

新增管理命令（复用 client 包）。**两条命令的访问路径不同**：

```
loom workspace list                     # 只读，直接走 OpenSQLiteStoreReadOnly（sqlite_store.go:81 的 mode=ro 路径），无需 serve 运行
loom workspace add <path> [--name N]    # 写操作，需 serve 运行中，走 HTTP POST /v1/workspaces；serve 未运行时明确报错并提示启动
loom workspace rm <id>                  # 删除元数据（§16.1）；与 add 同路径：serve 运行中时走 Web UI 或 DELETE /v1/workspaces/{id}
```

`list` 不依赖 serve：只读路径本来就为 inspect 类场景存在（不含写），session/workspace 列表是典型只读查询。`add` 必须经 serve 的 `WorkspaceRegistry.Register`（需要 canonical 校验 + 并发 dedupe + 装配触发），不走第二条写路径——这与 SERVE_DESIGN §17 的单一写者原则一致。

## 12. 安全模型

- **路径边界不变**：`PathValidator` 原样复用，每 workspace 一份实例，逃逸防护、敏感组件拦截（`.git/.ssh/...`）、原子写全部继承；
- **跨 workspace 无隐式通道**：不同 workspace 的 session 不共享 registry/runner/book；文件工具只能拿到本 workspace 的 validator，session A 无法读写 workspace B 的 root 内部（除非 B 嵌套在 A 内——W3 已声明此为既有语义）。精确边界：**沙箱内**操作受 root 限制；经审批提权后无沙箱运行的命令（danger-listed / escalation，PERMISSION_DESIGN 的风险分级）不受任何 workspace root 限制——这与现状完全一致，多 workspace 不改变审批提权的语义，也不假装改变；
- **注册接口的校验**：`POST /v1/workspaces` 要求 root 已存在且为目录，canonical dedupe。server 绑定 loopback + bearer token 的既有前提下，注册接口不引入新攻击面（能调 API 的人本机已有 token）；
- **审批与沙箱语义不变**：policy 的 user 层规则共享、project 层规则按 workspace 加载——与今天"换目录重启 loom"的结果一致；
- **审计增强**：session 归属（workspace_id/root）进入审计日志与 trace 属性，回答"哪个 agent 会话改了哪个项目"。

## 13. 可观测性

- trace：root span 增加 `loom.workspace.id` / `loom.workspace.root` 属性（`internal/trace` 的属性常量区追加，additive）；`trace.UserID` 逻辑不变（user 体系归 §17）；
- 日志：`session_service` 与 `controller` 的结构化日志补 `workspace_id` 字段；
- 指标（如有）：session/turn 计数按 workspace 维度打标。

## 14. 测试计划

| 层 | 用例 |
|----|------|
| store | v4→v5 迁移（含 backfill 幂等、只读打开兼容）；root UNIQUE 冲突；`SessionWorkspace` 轻量查询 |
| registry | canonical dedupe（symlink 路径注册复用）；并发 `Register/GetOrBuild` 幂等（single-flight）；root 删除后 `Resolve` 报 `ErrWorkspaceUnavailable`；嵌套 workspace 各自装配成功 |
| app 隔离 | **跨 workspace 隔离测试**（对齐 `session_isolation_test.go` 的现有模式）：两个 workspace 各起 session 并发执行——edit 写各自 root 生效、越界路径被各自 validator 拒绝、project rules/LOOM.md 各自加载、FileStateBook 互不串、approval 记忆共享（D4 语义钉死） |
| service | `CreateSession(unknownID)` → not found；`ResumeSession` 在 root 丢失时的错误路径；`ListSessions` 过滤；idle sweep 不误回收他 workspace 的 handle；子代理 session 与父 session 的 workspace_id 一致（经 `run.created` 事件归属断言） |
| store 签名传播 | `SessionStore.CreateSession` 增加 `workspaceID` 参数后，全部直调点（`Controller.NewSession`、`main_test.go`×4、`subagent_v2_e2e_test.go`、`session_service_test.go`）编译与行为绿 |
| server | 新端点契约测试；旧客户端兼容（不带 workspace_id 建 session 落 default）；typed error → 状态码映射 |
| e2e | 升级场景：存量 v3 库启动 → 自动迁移 → 历史 session 归属 default workspace → resume 正常 |
| 回归 | 现有 `bazel test //go/pl/loom/...` 全绿（TUI/headless 路径行为不变由现有测试锁定） |

## 15. 里程碑

| # | 内容 | 验收 |
|---|------|------|
| M1 | domain 类型 + schema v5 + WorkspaceStore 实现 + 迁移/backfill 测试 | store 层测试绿；手工 SQL 验证迁移 |
| M2 | Bootstrap 拆分为 ProcessRuntime/WorkspaceRuntime + WorkspaceRegistry + SessionService/Controller 签名改造 | `bazel test //go/pl/loom/...` 全绿；行为零变化（所有现有测试不删只改构造） |
| M3 | server 端点 + client 包扩展 + 契约测试 | API 契约测试绿；旧 SPA 不升级仍可用 |
| M4 | Web workspace 选择器 + `loom workspace` 子命令 + trace/日志维度 | 手工验收：单 serve 双 workspace 并发跑 turn |

M2 是工作量主体，涉及 `bootstrap.go` 的拆分搬运与 `app` 包全部构造夹具的调整；不改动任何工具实现与 agent loop 逻辑。

## 16. 开放问题

1. **workspace 生命周期**（删除已落地，归档未做）：删除采用**纯元数据移除**——只删 `workspaces` 行，磁盘目录不动；其 session 保留为只读历史（`workspace_id` 悬空，transcript/inspect 可读，resume 拒绝），这正是 §7.1 不加外键的预设场景。两类拒绝（409 `workspace_in_use`）：default workspace（W5，旧客户端的回落目标）、有存活 session 的 workspace（先删除/关闭其 session）。实现：`WorkspaceRegistry.Delete`（buildMu 与 Resolve 互斥，防懒装配复活；返回被摘除的 runtime 由调用方在锁外 Close）、`SessionService.DeleteWorkspace`（存活会话检查与删除在同一 s.mu 临界区；CreateSession/ResumeSession 在 handle 插入点复核 workspace 仍在注册表，双向串行化关闭 TOCTOU）、`DELETE /v1/workspaces/{id}`、`loom workspace rm`；list 响应带 `is_default` 标记供前端隐藏删除入口。归档语义待真实需求出现；
2. **root 换绑（repair）**：目录移动是真实操作（磁盘迁移、worktree 调整）。换绑会使历史 `file_changes` 相对路径指向新内容，rewind 风险需要在设计层面解决（如换绑时冻结历史 session 的 rewind 能力）。本期 W4 禁止换绑；
3. **Web 端目录浏览 picker**：注册 workspace 需要输入绝对路径，体验欠佳；server 端文件浏览 API 涉及路径探测面，单独设计；
4. **per-workspace MCP**：MCP server 的 `cwd` 为空时继承进程 cwd 的语义在多 workspace 下可能不符合直觉（MCP 工具读的是 default workspace 的相对路径）。选项：a) 文档化现状；b) `srv.Cwd` 为空时填 default workspace root；c) workspace 级 MCP 装配。倾向 b)（一行改动），实施期定；
5. **remembered rules / memory 的 workspace 维度**：D4/D5 维持全局是保守选择，长期应演进为 user 层 + workspace 层两级（同 rules 的 user/project 分层），分属 PERMISSION_DESIGN 与 MEMORY_DESIGN 的修订；
6. **LRU 回收**：workspace 常驻数量级（几十）内无需回收；若未来 serve 承载上百 workspace，引入 refcount + idle 回收，结构已预留 `WorkspaceRuntime.Close`。

## 17. 演进方向：user 体系的接缝

本设计不实现 user，但以下决策已为其铺路，P2 引入 user 时**不需要返工**：

- `sessions` 表届时 `ALTER TABLE ... ADD COLUMN user_id TEXT NOT NULL DEFAULT ''` + backfill 为单例 `UserLocal`，与本期 `workspace_id` 的模式完全同构；
- `workspaces` 表届时加 `owner_id` 列；
- `WorkspaceRegistry` 的 `Resolve/Register` 届时增加调用方身份参数做归属校验（app 层授权，延续 SERVE_DESIGN "协议语义在应用层"原则）；
- server 的 `authorized()`（`auth.go:30`）届时从 `bool` 升级为返回 `Identity`；审批 actor（SERVE_DESIGN G6）随 user 一并落地；
- 配额（`MaxSessions/MaxActiveTurns`）届时从进程级单值改为 per-user 计数。

上述每一项都是 additive 演进，不在本期实现任何一处。

---

## 附：关联文档修订清单（实施时同步）

- `SERVE_DESIGN.md` §1.1 非目标：删除"多 workspace"一条，改为指向本文档；
- `DESIGN.md` §4 总体架构图：`workspace/` 注释从"路径安全、规则和快照"扩为"路径安全、规则和快照；workspace 实体与注册表见 WORKSPACE_DESIGN.md"；
- `CONFIG_DESIGN.md`：补 `workspaces` 段 schema（§10）；
- `PERMISSION_DESIGN.md`：标注 project rules 加载时机从"进程启动"变为"workspace 装配"（行为等价，时机变化）。
