# Loom 记忆系统设计

> Codex 长期记忆架构的 1:1 复刻（两阶段提取/整合、分层存储、
> 基于 git 的增量 diff 追踪、通过记忆工具实现渐进式披露）。

---

## 1. 概述

记忆系统为 Loom 提供跨会话的持久化回忆能力。没有它，每个会话都从零开始；
有了它，agent 能记住用户偏好、项目约定、调试工作流以及历史决策——
正如人类同事随时间逐渐内化的一切。

本设计镜像了 Codex 的记忆流水线：

```
  ┌──────────┐     阶段 1        ┌──────────────┐     阶段 2        ┌──────────────┐
  │  会话     │ ──── 提取 ─────→ │ raw_memories  │ ──── 整合 ──────→│  MEMORY.md   │
  │  消息     │    (LLM 调用)    │   （暂存区）   │    (LLM 调用)    │  （手册）     │
  └──────────┘                    └──────────────┘                    └──────┬───────┘
                                                                            │
                                                                   ┌────────▼────────┐
                                                                   │ memory_summary   │
                                                                   │   （热层）        │
                                                                   └─────────────────┘
```

### 设计目标

| # | 目标 | 理由 |
|---|------|-----------|
| G1 | **两阶段流水线** | 提取（按会话、低成本）不得阻塞用户；整合（跨会话、高成本）在退出时运行。 |
| G2 | **分层存储** | 热层（摘要 → 系统提示词）、温层（MEMORY.md → 可搜索）、冷层（rollout 与笔记 → 按需读取）。 |
| G3 | **增量合并** | 基于 git 的 diff 检测避免重复处理未变更的记忆。 |
| G4 | **渐进式披露** | 摘要始终加载；完整的 MEMORY.md 和原始文件仅在 agent 使用记忆工具时读取。 |
| G5 | **配置开关** | `memory.enabled: false` 可干净地禁用整个子系统。 |
| G6 | **数据完整性** | 空模型输出绝不抹掉已有记忆；diff 解析保留水平分隔线与各类分隔符。 |

---

## 2. 存储布局

所有记忆文件位于 `~/.loom/memories/` 之下：

```
~/.loom/memories/
├── .git/                          # 用于增量 diff 的 Git 仓库
├── memory_summary.md              # 热层：≤ 2500 token，注入系统提示词
├── MEMORY.md                      # 温层：整合后的手册（≤ 32K 字符）
├── raw_memories.md                # 冷层：提取记忆的暂存区
├── rollout_summaries/             # 冷层：按会话的摘要
│   ├── 2026-08-02T14-30-00-abcd1234.md
│   └── fix-bazel-build.md
├── extensions/
│   └── ad_hoc/
│       └── notes/                 # 冷层：用户标记的笔记
│           └── 2026-08-02T14-30-00-prefer-go.md
└── skills/                        # 冷层：可复用的流程定义
```

### 分层语义

| 层级 | 文件 | 加载时机 | token 预算 |
|------|---------|-------------|--------------|
| 热层 | `memory_summary.md` | 每轮（注入系统提示词） | ~2500 token |
| 温层 | `MEMORY.md` | 按需，通过 `memory_read` 工具 | ~8000 token |
| 冷层 | `raw_memories.md`、rollout 摘要、笔记 | 按需，通过 `memory_read` / `memory_search` | 无限制 |

---

## 3. 两阶段流水线

### 阶段 1：提取（Extraction）

**触发时机**：会话关闭（`Controller.handleShutdown`）。

**流程**：
1. 序列化会话转录（user + assistant 消息、工具调用名称）。
2. 将转录截断至 200 KB（保留尾部——最近的轮次信号最强）。
   - UTF-8 安全：字节级截断后，跳过不完整的 rune 前缀以避免乱码。
3. 以 `temperature=0` 发送给提取模型。
4. 解析 JSON 输出：`{rollout_summary, rollout_slug, raw_memory}`。
5. 清洗模型给出的 slug（仅允许字母数字与连字符；防止路径穿越）。
6. 将 `raw_memory` 追加到 `raw_memories.md`（暂存区）。
   - 每个 rollout 以 `---` 分隔线开头，并带有包含 slug、会话 ID、工作区和提取时间戳的头部。
7. 将 `rollout_summary` 写入 `rollout_summaries/<slug>.md`。

**超时**：45 秒（可在 `Controller` 中配置）。

**兜底**：如果模型调用失败或返回为空，则不写入任何内容；该会话的记忆就此丢失。

### 阶段 2：整合（Consolidation）

**触发时机**：应用退出（`Bootstrap.Close`），在阶段 1 之后。

**流程**：
1. 执行 `git add -A && git diff --cached`，检测自上次整合以来的新内容。
2. 仅从 diff 中提取新增行。
   - **关键点**：只跳过 `"+++ "` 和 `"--- "`（带尾随空格，它们是 diff 的文件头，如 `--- a/file` 和 `+++ b/file`）。像 `+---` 这样的内容行（水平分隔线、YAML 分隔符）必须保留。
   - 还要跳过 `"++"` 行（即被空格规则捕获的 `+++` 头，额外的 `!strings.HasPrefix(line, "++")` 保护确保不会把 `+++` 误当作内容行）。
3. 如果模型可用：
   - 将现有的 `MEMORY.md` 加上新增行发送给整合模型。
   - **空输出保护**：如果模型返回空/纯空白输出，则退回追加策略，而不是写入空文件（那会抹掉所有已有记忆）。
   - 剥离模型可能在输出外层包裹的 markdown 代码围栏。
   - 将合并结果写入 `MEMORY.md`。
   - 将更新后的 `MEMORY.md` 发送给摘要模型。
   - 摘要同样适用空输出保护。
   - 将摘要写入 `memory_summary.md`。
4. 如果模型不可用：
   - 将新内容以带时间戳的头部追加到 `MEMORY.md`。
   - 取 `MEMORY.md` 前 100 行作为摘要。
5. 以整合时间戳为提交信息执行 `git commit -A`。

**超时**：90 秒（两次串行的 LLM 调用）。

**兜底**：模型出错或输出为空时，退回追加策略（步骤 4）。git 出错时，整个整合跳过。

---

## 4. 系统提示词集成

`memoryPromptWrapper`（位于 `bootstrap.go`）装饰了基础的
`PromptBuilder`：

```go
type memoryPromptWrapper struct {
    inner  prompt.PromptBuilder
    store  *memory.Store
    logger *slog.Logger
}
```

### 构建流程

1. 调用 `inner.Build()` → `(basePrompt, refs, err)`。
2. 从 store 读取 `memory_summary.md`。
3. 组装注入段落：
   - `# Memory\n\n` + 摘要（若非空）
   - `MemoryInstructions`（工具使用说明）
4. 对注入段落计算 `sha256` → `ContextRuleRef.Hash`。
5. 追加该段落与规则引用。

这保证了：
- 摘要始终是最新的（每轮重新读取）。
- 审计规则哈希是内容寻址的（与 manifest 追踪的一致）。
- 摘要不存在时，仅注入工具使用说明。

### 截断

摘要被截断至 `SummaryTokenLimit × 4` 字符（约 10,000 字符）。发生截断时会附加提示：

```
(Memory summary truncated; use memory_search and memory_read for full content)
```

所有面向用户的提示词均为英文，以与系统提示词保持一致。

---

## 5. 记忆工具

四个读写工具将记忆存储暴露给 agent：

| 工具 | 风险等级 | 描述 |
|------|------|-------------|
| `memory_list` | R1 | 列出存储中某相对路径下的文件/目录 |
| `memory_read` | R1 | 读取记忆文件（支持可选的行偏移/行数限制） |
| `memory_search` | R1 | 在所有 `.md` 文件中进行子串搜索 |
| `memory_add_note` | R2 | 创建带时间戳的临时笔记（仅追加） |

### 路径安全

所有接受相对路径的工具都采用多层防护：

1. `isWithinRoot()` —— 防止目录穿越（例如 `relPath="../memories-evil"`）：
   ```go
   func isWithinRoot(absPath, root string) bool {
       if absPath == root { return true }
       return strings.HasPrefix(absPath, root+string(os.PathSeparator))
   }
   ```

2. **`.git` 拒绝** —— `ReadFile` 拒绝任何包含 `.git` 组成部分的路径，防止通过工具 API 读取 git 内部数据。

3. **笔记专属的 `isWithinRoot()`** —— `AddNote` 和 `ReadNote` 校验解析后的路径始终位于 `NotesDir` 之内，防止通过文件名参数逃逸路径（这是在工具层正则校验之外的纵深防御）。

### 空查询 / maxResults 防御

- `Store.Search("")` 返回零匹配（防止 `strings.Contains(line, "")` 恒为真的边界情况）。
- `Store.Search(query, 0)` 将 `maxResults` 默认为 `DefaultSearchMaxResults`（200），防止返回零结果。
- `Store.List(relPath, 0)` —— 工具层在调用 `List` 之前应用 `DefaultListMaxResults`。

### ReadFile 偏移超出文件长度

当 `offset > total_lines` 时，`ReadFile` 返回空内容和总行数，而不是静默地从开头返回整个文件。这防止了模型在请求超出文件末尾的行时读到错误的内容。

### List 跳过 .git

`Store.List` 使用 `len(out) >= maxResults`（而非迭代索引）来跳过 `.git` 条目，因此 `.git` 条目不会占用结果名额。这确保调用方恰好得到 `maxResults` 个非 git 条目。

### 笔记文件名验证

临时笔记必须匹配：`YYYY-MM-DDTHH-MM-SS-slug.md`（在 JSON schema 和
Go 校验中都用正则强制）。这防止了通过文件名进行路径穿越。

---

## 6. Git 集成

### 目的

Git 追踪记忆存储的增量变更，使整合只处理新内容（通过 `git diff`），
而不是整个历史。

### 初始化

`Store.InitGit()` 在首次使用时执行 `git init` 加初始提交。
git 提交命令附带 `-c user.name=loom -c user.email=loom@localhost`，
因此即使没有全局 git 身份配置也能正常工作。

### 工作流程

```
提取写入   → raw_memories.md（已暂存，未提交）
                   ↓
整合       → git add -A
                   → git diff --cached（检测新内容）
                   → 模型合并
                   → git commit -A（快照所有变更）
```

### git 不可用时

如果 git 初始化失败（无 git 二进制、权限问题），整合将被**禁用**——
而非静默降级。日志会明确说明整合在 git 可用之前将保持禁用，
而不是声称存在一个并不存在的“追加策略”兜底。

### 并发考量

当前基于 git 的 diff/commit 循环在单进程使用下是安全的。
多进程并发访问同一记忆存储目录目前**不受支持**——两个同时运行的
loom 实例可能在 git index 状态上发生竞争。这是记录在案的已知限制。
未来可能通过文件锁或基于 HEAD 的冲突检测来解决。

---

## 7. 配置

```yaml
# ~/.loom/config.yaml
memory:
  enabled: true   # 默认值：true；设为 false 可禁用
```

当 `memory.enabled` 为 false 时（或省略时——默认为 true）：
- 不打开记忆存储。
- 不注册记忆工具。
- 不运行提取或整合。
- 系统提示词中不注入记忆段落。

### 解析后的默认值

```go
type ResolvedMemory struct {
    Enabled bool
}
```

解析器应用 `Enabled: f.Memory.Enabled == nil || *f.Memory.Enabled`，
因此当没有 `memory` 配置段时，该子系统默认启用。

---

## 8. 生命周期钩子

| 事件 | 钩子 | 超时 |
|-------|------|---------|
| 会话结束 | `Controller.handleShutdown` → `Extractor.ExtractFromSession` | 45s |
| 应用退出 | `Bootstrap.Close` → `Consolidator.Consolidate` | 90s |

两个钩子都是**尽力而为（best-effort）**：失败会被记录但绝不阻塞关闭。
如果超时到期，context 会被取消，操作被放弃。

### 顺序保证

阶段 1（提取）必须先于阶段 2（整合）完成。
这由调用顺序保证：`handleShutdown` 同步运行提取，
然后 `Bootstrap.Close` 运行整合。其他退出路径
（如无头 runner）必须保持这一顺序。

---

## 9. 错误处理与健壮性

| 场景 | 处理方式 |
|----------|----------|
| 模型返回非 JSON | `extractJSON()` 剥离 markdown 围栏，找到第一个 `{…}`；解析失败返回错误（会话丢失） |
| 模型将输出包裹在代码围栏中 | `stripMarkdownFences()` 移除 ` ```markdown ... ``` ` 包装 |
| 模型返回空输出 | **保护**：空/纯空白模型输出退回追加策略，绝不抹掉已有的 MEMORY.md 或摘要 |
| 响应中途流出错 | `io.EOF` → 正常结束；其他错误 → 立即返回（丢弃部分响应） |
| 模型给出恶意 slug | `sanitizeSlug()` 剔除 `[^a-z0-9-]`、折叠连字符，退回生成式 slug |
| git 未安装 / 初始化失败 | 整合被禁用并记录明确日志；提取仍会写入原始文件 |
| 转录过长 | `capTranscript()` 截断至 200 KB 并保留尾部；边界处不完整的 UTF-8 rune 会被跳过 |
| 工具参数中的路径穿越 | 多层防御：`isWithinRoot()` + `.git` 拒绝 + 笔记专属根目录检查 |
| 模型传入 `maxResults=0` | `Execute()` 应用 `DefaultListMaxResults` / `DefaultSearchMaxResults`；`Store.Search()` 内部也有默认值 |
| 空搜索查询 | `Store.Search("")` 返回零匹配（避免 `strings.Contains(line, "")` 恒为真） |
| ReadFile 偏移超出文件 | 返回空内容 + 总行数（而非整个文件） |
| diff 中的 `---` 内容行 | `extractAddedLines()` 只跳过 `"+++ "` / `"--- "`（带空格），保留 `+---` 水平分隔线 |
| Search 中的 Walk 错误 | 传播给调用方（此前被静默吞掉） |

---

## 10. 源码地图

| 文件 | 职责 |
|------|---------------|
| `internal/memory/store.go` | 基于文件的存储、git 集成、搜索、路径安全 |
| `internal/memory/store_test.go` | Store 单元测试（路径逃逸、偏移、.git 拒绝、搜索默认值） |
| `internal/memory/tools.go` | `memory_list/read/search/add_note` 工具实现 |
| `internal/memory/tools_test.go` | 工具单元测试 |
| `internal/memory/prompt.go` | 摘要注入、`MemoryInstructions`、`RuleRef` |
| `internal/memory/prompt_test.go` | Prompt provider 单元测试 |
| `internal/memory/extract.go` | 阶段 1 提取（模型调用、转录序列化、slug 清洗、JSON 提取） |
| `internal/memory/extract_test.go` | Extractor 单元测试（sanitizeSlug、capTranscript、extractJSON） |
| `internal/memory/consolidate.go` | 阶段 2 整合（合并 + 摘要、diff 解析、围栏剥离） |
| `internal/memory/consolidate_test.go` | 整合单元测试（extractAddedLines、stripMarkdownFences、truncateToSummary） |
| `internal/app/bootstrap.go` | 装配：打开 store、注册工具、包装 PromptBuilder、关闭时整合 |
| `internal/app/controller.go` | 关闭钩子：提取触发 |
| `internal/config/schema.go` | `Memory.Enabled` 配置字段 |
| `internal/config/resolve.go` | 默认启用语义的 `ResolvedMemory` |

---

## 11. 与 Codex 的对比

| 方面 | Codex | Loom |
|--------|-------|------|
| 存储根目录 | `~/.codex/memories/` | `~/.loom/memories/` |
| 热层 | `memory_summary.md` | 相同 |
| 温层 | `MEMORY.md` | 相同 |
| 冷层 | `raw_memories.md` + rollouts | 相同 + `extensions/ad_hoc/notes/` |
| Git 追踪 | 按文件 diff | 相同（有记录在案的单进程限制） |
| 提取提示词 | `stage_one_system.md` | `stageOneSystemPrompt`（1:1 改写） |
| 整合提示词 | `stage_two_system.md` | `stageTwoSystemPrompt`（1:1 改写） |
| 摘要模型 | 独立的低成本模型 | 同一模型（未来可配置） |
| 工具面 | `memory_search`、`memory_read` | + `memory_list`、`memory_add_note` |
| 配置开关 | — | `memory.enabled`（默认 true） |
| 空输出保护 | — | 有：空模型输出时退回追加策略 |
| 路径安全 | 基础 | 多层：isWithinRoot + .git 拒绝 + 笔记根目录检查 |
| UTF-8 安全 | — | 有：capTranscript 跳过不完整的 rune 前缀 |

---

## 12. 已知限制与未来工作

| 事项 | 状态 | 说明 |
|------|--------|-------|
| 多进程并发 | 已知限制 | 两个共享 `~/.loom/memories/` 的 loom 实例可能在 git index 上竞争；仅支持单进程 |
| 轻量提取模型 | 计划中 | 目前使用主对话模型；`memory.model` 配置字段将允许使用更低成本的模型 |
| `Cleanup()` 调度 | 计划中 | `Store.Cleanup()` 存在但生产环境从未调用；rollout 摘要会无限增长 |
| `skills/` 目录 | 计划中 | 目录会被创建并在 `MemoryInstructions` 中宣传，但没有写入路径 |
| `DefaultReadMaxTokens` | 计划中 | 常量已定义但未使用；MEMORY.md 可能超出预期大小 |
| token 级截断 | 计划中 | 当前截断按字符数（约 4 字符/token）；正经的 tokenizer 会更精确 |
