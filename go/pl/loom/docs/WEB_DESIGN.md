# Loom Web SPA 设计（M3）

- 状态：Draft v2（v1 经一轮对照代码实现的自审修订：diff 来源更正为 tool.prepared、补齐 SessionSummary 富化（§7.7）、prompt 幂等键、全局 401、SSE 活性看门狗、审批 actor 自报、allow_multiple、transcript 去重、X-Frame-Options 入列、首入落地态）
- 日期：2026-08-04
- 前置文档：`SERVE_DESIGN.md`（§5 协议、§9 客户端契约、§12 里程碑）、`TUI_DESIGN.md`（设计语言来源）
- 范围：`loom serve` 内嵌 Web 客户端（`internal/server/web/`），浏览器完整跑通对话/审批/问答/取消/恢复/重连

---

## 1. 背景与目标

M1'+M2 交付了「curl 可驱动的 headless server」：REST+SSE 协议 v1、双实现契约测试、`loom serve` 守护进程。M3 在此之上交付协议的第三个一等公民前端——浏览器 SPA，达成 SERVE_DESIGN.md 的最终形态：「类似 Codex App 的网页版」。

### 1.1 目标

1. **零构建链交付**：SPA 以静态文件经 `embed.FS` 内嵌进 `loom` 二进制，`GET /` 直接可用；不引入 npm/node 工具链（本仓库为 Bazel monorepo，无 JS 基建）。
2. **协议公民**：SPA 只消费 §5 协议（snapshot + SSE + REST），不链接任何 Go 内部包，不本地推导 agent 状态机——与 TUI、curl 平权。
3. **完整交互闭环**：token 引导、会话列表/新建/恢复、流式对话、工具块、diff 视图、审批卡片、问答卡片、取消、断线重连、resync。
4. **安全底线**：XSS 白名单 sanitize + 负例验收；token 仅存 `sessionStorage`；严格 CSP。
5. **设计语言对齐 TUI**：Everforest 双色系（dark/light），块语义与 TUI 一一对应（见 §6）。

### 1.2 非目标

- 多用户/远程部署（TLS/SSO，SERVE_DESIGN §1.1/O5 已明确出范围）。
- checkpoint/rewind、rules 管理、skills/MCP 查看页（协议端点属 M3+，SPA 后续迭代）。
- 移动端深度适配（桌面优先，窄屏仅保证可用）。
- 国际化（文案随 TUI 用英文；i18n 留开放问题）。
- 消息历史虚拟滚动（MVP 用「加载更早」分页；虚拟化待性能基线）。

---

## 2. 技术选型

### 2.1 决策总览

| 维度 | 决策 | 备选（放弃原因） |
|---|---|---|
| 应用框架 | **Vanilla ES Modules + 微内核 store**（零框架） | React+Vite（引入 npm 构建链，供应链面大）；Preact+htm（保留为迁移路径，见 §2.3） |
| Markdown 渲染 | **marked**（vendored ESM，~40KB） | markdown-it（体积 2 倍+）；自写（不完整， XSS 面更大） |
| XSS 净化 | **DOMPurify**（vendored ESM，~20KB，事实标准） | 自写白名单（边角 case 无穷尽，R7 为高风险不接受） |
| 代码高亮 | **highlight.js**（vendored ESM common 包，~125KB）：markdown 代码块 + diff 行内高亮；diff 自写解析器（~80 行，服务端已给 unified diff 字符串） | Prism（生态分裂）；自写 tokenizer（质量不可比） |
| CSS | **原生 CSS + custom properties**（双主题一套变量） | Tailwind（要构建链）；CSS-in-JS（要框架） |
| SSE 传输 | **fetch + ReadableStream 手写 SSE 解析**（~100 行） | EventSource（**无法设置 Authorization header**，见 §2.2）；polyfill（cookie 依赖，违反协议设计） |
| 图标 | 内联 SVG（~10 个，手绘）+ Unicode 兜底 | icon font/npm 包（体积与供应链） |

### 2.2 关键技术约束：SSE 与认证

`EventSource` 不支持自定义请求头，而协议规定 token 只能走 `Authorization: Bearer` header（禁止 URL 参数——R4 token 泄漏面；禁止 cookie——CSRF 与隐式携带问题，SERVE_DESIGN §5.2/§6）。因此浏览器端必须用 `fetch` 发起 SSE 请求并手写解析器：

```
fetch(url, {headers: {Authorization}}) → res.body.getReader() →
  增量 decode UTF-8 → 按 \n 切行 → 累积 id:/event:/data: 字段 → 空行 dispatch
```

这与 Go 侧 `client/http.go` 的 `pumpSSE` 是同构实现（多行 data、注释行、`server.resync`/`server.draining` 帧），行为契约一致。连接生命周期（重连、退避、instance 检测）完全自控，不受 EventSource 自动重连策略干扰。

### 2.3 为什么零框架成立（以及何时不成立）

本应用的热路径是**追加式流式渲染**：`model.text_delta` 高频到达（数十次/秒），正确做法是向流式块末尾 appendChild/append text node——VDOM 框架在此路径上反而是负担（每次 delta 触发组件重渲染与 diff）。状态复杂度集中在「会话切换 + pending 请求 + 重连状态机」，一个 ~100 行的 store（状态 + subscribe + reducer 式更新）足够承载。

**迁移触发条件**（届时迁 Preact+htm，仍零构建链：vendor 两个 ESM 文件即可）：组件数 >25、出现双向数据流（设置页/表单页）、或多人协作维护前端。组件以「纯函数返回 DOM 子树」编写，迁移成本低。

### 2.4 Vendored 依赖清单

| 包 | 版本 | 文件 | 用途 |
|---|---|---|---|
| marked | 待 vendoring 时锁最新稳定版 | `vendor/marked.esm.js` | assistant 文本 markdown → HTML |
| DOMPurify | 同上 | `vendor/purify.es.mjs` | HTML sanitize 白名单 |
| highlight.js | 同上（cdn-release ESM build） | `vendor/highlight.es.min.js` | 代码块 / diff 行内语法高亮 |

 vendoring 时记录精确版本号与 SHA-256（写进 `internal/server/web/VENDORED.md`），更新走显式 PR。除此三包外无任何第三方 JS。

---

## 3. 前端架构

### 3.1 目录与模块

```
internal/server/web/
├── web.go                  # //go:embed static；静态文件 handler（§7.1）
├── VENDORED.md             # vendored 包版本 + SHA-256
└── static/
    ├── index.html          # 唯一 HTML（token 引导页 + 主壳同一文档）
    ├── app.css             # 全部样式（主题变量 + 组件，§6）
    ├── favicon.svg
    ├── vendor/
    │   ├── marked.esm.js
    │   └── purify.es.mjs
    └── js/
        ├── main.js         # 启动编排：token gate → boot → 主界面
        ├── api.js          # REST 封装（fetch + Bearer + 错误模型解析）
        ├── sse.js          # fetch SSE 解析器 + 连接管理（重连/退避/实例检测）
        ├── store.js        # 状态容器：state + subscribe + actions
        ├── markdown.js     # marked + highlight.js + DOMPurify 管线（含 sanitizeHtml 供 diff 复用）
        ├── diff.js         # unified diff 解析与渲染
        ├── format.js       # 时间/数字/usage 格式化
        └── components/
            ├── sidebar.js      # 会话列表 + 实时徽标
            ├── transcript.js   # 消息流容器 + 块渲染调度
            ├── blocks.js       # user/assistant/tool/notice/stream 块
            ├── composer.js     # 输入框（IME 安全、自适应高度）
            ├── approval.js     # 审批卡片
            ├── question.js     # 问答卡片
            ├── statusbar.js    # ctx 占用 / usage / turn / 连接状态
            └── subagent.js     # 子 agent 钻取视图（M3c）
```

无构建步骤：`web.go` 直接 `//go:embed static`；浏览器以 `<script type="module" src="/js/main.js">` 原生加载模块图。

### 3.2 状态模型（store）

单一 store，state 形状：

```js
{
  conn:    "connecting" | "live" | "reconnecting" | "draining" | "dead",
  token:   string | null,            // 仅存 sessionStorage
  session: {                         // 当前会话（snapshot 驱动）
    id, state, modelName, turnCount, usage, contextWindow,
    eventSeq,                        // SSE 重连 cursor
  },
  blocks:  [...],                    // transcript 渲染模型（§3.3）
  pending: { approvals: Map, questions: Map },  // 卡片，按 id 存取
  sessions: [...],                   // 侧栏列表 + 徽标
}
```

**铁律**（SERVE_DESIGN §9 客户端契约，逐条落到实现）：

1. 状态来源仅 `GET snapshot`（首屏/会话切换/resync）+ SSE（此后全部）；store 的任何字段不得由其他途径推导。
2. 未知事件 kind / 未知 payload 字段一律忽略（协议向前兼容）。
3. 流式文本：delta 拼草稿；`model.response_completed` 到达即整段替换（canonical 校正），替换时才跑 markdown 管线——**流式期间不渲染 markdown**，纯文本追加，完成时一次性渲染。这同时解决增量 markdown 解析的正确性难题与 XSS 面（sanitize 只跑一次）。
4. 审批卡片由 `approval.requested` payload 完整渲染；收到 `approval.resolved` 即撤卡折叠为一行 notice（含 actor）。首屏由 `snapshot.pending_requests` 重建。
5. `server.resync` 或 SSE 首帧 instance 变化 → 清空 blocks/pending，重走 snapshot → 以新 `event_seq` 重订阅。`server.draining` → 置 `conn=draining`，停止重连，顶部横幅提示。
6. 一切模型/工具文本输出：DOM 操作只许 `textContent`；允许 `innerHTML` 的位置只有两处，且都必须经 DOMPurify 白名单过滤（入口统一在 markdown.js）：marked 输出、hljs 高亮片段（diff.js 经 `sanitizeHtml` 复用）。
7. token 存 `sessionStorage`（关页即清），不落 `localStorage`。

### 3.3 渲染模型

transcript 不是消息列表，而是**块序列**（与 TUI 的 transcript 块同构）：

| 块类型 | 来源 | 说明 |
|---|---|---|
| `user` | `turn.started.payload.prompt` | 用户投稿 |
| `assistant` | `model.response_completed.payload.text` | 完成态，markdown 渲染 |
| `stream` | `model.text_delta` 累积 | 进行中草稿，纯文本 + 光标符 |
| `reasoning` | `model.reasoning_delta` / completed | 折叠块（默认收起，点击展开） |
| `tool` | `tool.prepared/started/completed` | 工具卡片：名称、状态徽标、耗时、参数摘要、输出（截断+展开） |
| `diff` | tool.completed（edit 类）payload.diff | 语法着色 diff 视图 |
| `approval` | `approval.requested` | 交互卡片（§4.3），resolve 后折叠为 notice |
| `question` | `question.asked` | 交互卡片（§4.4），answer 后折叠为 notice |
| `notice` | steer/budget/compact/subagent/warning 等 | 单行弱化提示 |
| `fatal` | `runtime.fatal` | 错误块 |

首屏从 `snapshot.messages`（canonical 历史）建块；`pending_requests` 在对应位置后补交互卡片。会话历史分页：MVP 渲染 snapshot 全量 + 「加载更早」按钮走 `GET /transcript?after=&limit=`——**两个投影有交集**（R8：snapshot=实时权威、transcript=历史权威），拼接时必须按 message id 去重，以 snapshot 已有内容为基准只 prepend 更早的页。虚拟滚动留开放问题。

### 3.4 连接管理（sse.js）

```
connect(afterSeq):
  fetch SSE → 首帧读 instance（与上一连接不同 → 触发 resync）
  正常帧 → dispatch 到 store
  活性看门狗：服务端每 15s 发心跳注释帧；>45s 无任何帧 → 判死，主动断开重连
    （后台标签页中 fetch 流可能静默 stall，不能依赖 onerror）
  连接断开（非 draining）→ 指数退避重连：1s, 2s, 4s … 封顶 15s，jitter ±25%
  重连时用 Last-Event-ID 语义：URL after=<最后收到的 seq>
  收到 409 cursor_invalid / server.resync → 全量 resync（snapshot → 新 cursor）
  收到 server.draining → 停止重连，conn=draining
  429 rate_limited（单会话 >8 条 SSE 流）→ 提示「该会话已在太多标签页中打开」，不重连
  页面隐藏（visibilitychange）不断连；仅侧栏轮询暂停（§4.6）
```

**REST 通用行为**（api.js 统一处理）：

- 任何响应 401 → 清 `sessionStorage` token、断开 SSE、回 token 引导页（token 可能被 rotate）。
- 每次 prompt 投稿生成 UUID 作 `Idempotency-Key` header：网络重试/双击/重发共享同键，服务端去重（M2 已支持单飞幂等），绝不允许一次用户意图产生两个 turn。
- 409/503 → toast 错误条，输入框内容恢复。

---

## 4. 页面布局

桌面优先，三档断点：≥1200px（完整侧栏）、768–1199px（侧栏可折叠）、<768px（侧栏抽屉式，仅保证可用）。

### 4.1 Token 引导页（首访）

`index.html` 加载后先查 `sessionStorage.loom_token`：无 → 渲染引导页（不占主壳）；有 → 先 `GET /v1/meta/version` 验活，401 则回引导页并提示「token 无效」。

```
┌──────────────────────────────────────────┐
│                                          │
│   ◆ loom                                 │
│                                          │
│   Enter the serve token to connect.      │
│   ┌──────────────────────────────────┐   │
│   │ ••••••••••••••••                 │   │
│   └──────────────────────────────────┘   │
│   [ Connect ]                            │
│                                          │
│   Stored in sessionStorage only.         │
│   token 见首次 `loom serve` 启动输出或    │
│   <数据目录>/serve.token                  │
│                                          │
└──────────────────────────────────────────┘
```

### 4.2 主界面（桌面）

```
┌────────────────────────────────────────────────────────────────────┐
│ ◆ loom   sess_241c…   glm-5.2 · running        ● live        ⚙ ☀ │  header (48px)
├─────────────────┬──────────────────────────────────────────────────┤
│ + New session   │  You                                             │
│                 │  ┌────────────────────────────────────────────┐  │
│ ● sess_241cf…   │  │ fix the flaky test in sstv2                │  │
│   glm-5.2       │  └────────────────────────────────────────────┘  │
│   turn 3 · ●    │                                                  │
│                 │  Loom                                            │
│ ○ sess_9ab31…   │  The flaky test is in `block_test.go`…          │  │
│   idle          │  ▸ reasoning (1.2k tokens)                       │  │
│                 │  ┌─ run_cmd ─────────────────── ✓ ok · 1.2s ──┐  │
│ ○ sess_77ee0…   │  │ $ bazel test //cpp/pl/sstv2:block_test     │  │
│   idle          │  │ │ output (42 lines, click to expand)       │  │
│                 │  └────────────────────────────────────────────┘  │
│                 │  ┌─ edit ─────────────────────── ✓ ok ────────┐  │
│                 │  │ ▼ diff                                     │  │
│                 │  │  - expect_eq(n, 3)                         │  │
│                 │  │  + expect_eq(n, 4)                         │  │
│                 │  └────────────────────────────────────────────┘  │
│                 │                                                  │
│                 │  ⚠ Approval required · R2                        │
│                 │  ┌────────────────────────────────────────────┐  │
│                 │  │ run_cmd: rm -rf /tmp/build-cache           │  │
│                 │  │ [ Allow ]  [ Allow always ]  [ Deny ]      │  │
│                 │  └────────────────────────────────────────────┘  │
│                 │                                                  │
│                 │  ▍streaming answer…                              │
├─────────────────┴──────────────────────────────────────────────────┤
│ ┌────────────────────────────────────────────────┐                 │
│ │ Message loom…                       (⇧⏎ 换行)  │  [Send ⏎] [■]   │  composer
│ └────────────────────────────────────────────────┘                 │
│ ctx 12% · 1.2k in / 340 out · turn 3 · reconnecting in 2s…        │  statusbar (28px)
└────────────────────────────────────────────────────────────────────┘
```

区域职责：

- **Header**：会话 id（点击复制）、模型名、会话状态徽标（idle/running/awaiting_approval/cancelling/fatal）、连接状态灯（live/reconnecting/draining/dead）、主题切换、（M3c 后）设置入口。
- **Sidebar**：`+ New session` 置顶；会话条目 = 截断 id + 模型 + turn 数 + 实时状态点（running 绿色脉冲 / awaiting 黄色 / idle 灰）。点击切换会话（→ snapshot 重载 + SSE 重订阅）。`ESC` 或点击遮罩在窄屏收起。
- **Transcript**：块序列（§3.3），自动跟随滚动——用户上翻则暂停跟随并显示「↓ 回到底部」浮钮；交互卡片（审批/问答）到达时若不在视口内，状态栏提示 + 自动滚动。
- **Composer**：自适应高度（1–8 行），`Enter` 发送、`Shift+Enter` 换行；**IME 保护**（`compositionstart` 期间 Enter 不发送）；running 时发送 = steer 投稿（输入框提示文案变化：`Steer this turn…`），`■` 按钮 = `POST /cancel`。
- **Statusbar**：ctx 占用百分比（`usage`/`context_window`，>80% 变黄 >90% 变红）、token usage、turn 数、连接/重连信息。

### 4.3 审批卡片

内联于 transcript（不弹模态——多轮审批流与消息流时间线一致；同 SERVE_DESIGN §9 契约第 4 条）。未决卡片在视口滚动时吸底（`position: sticky`）保证可见。

```
┌ ⚠ Approval required · R2 · run_cmd ──────────────────────────────┐
│ rm -rf /tmp/build-cache && bazel build //...                     │
│ ┌──────────────────────────────────────────────────────────────┐ │
│ │ arguments (JSON, 折叠)                                       │ │
│ └──────────────────────────────────────────────────────────────┘ │
│ diff（edit/write 类工具时展示，§4.5）                             │
│ [ Allow ]   [ Allow always ]   [ Deny ]                          │
└───────────────────────────────────────────────────────────────────┘
```

- 提交 binding 三元组：`POST /v1/sessions/{id}/approvals/{approval_id}`，body `{call_id, args_hash, decision, client}`；`Allow always` 附加 `rule_hint`。`client` 填 `"web"`（或 `"web:<tab 短 id>"`）——`approval.resolved` 的 actor 字段来自客户端自报，不传则多客户端场景无法区分决议者。
- 决议返回 → 卡片原地折叠为一行 notice：`✓ Allowed (you) · run_cmd rm -rf …` / `✗ Denied (you)`；随后 `approval.resolved` 事件到达时幂等确认（actor 字段区分决议者：本 tab 提交记 `(you)`，其他客户端/actor 字符串原样展示；server 侧超时落地后为 `system:timeout`，卡片自动收编，无需额外处理）。
- **先到先得竞态**：点击后若返回 409 `binding_mismatch`（已被其他客户端决议）→ 立即撤卡，notice 显示实际决议方。
- `Allow always` 的提示样式需与单次授权明确区分（次要按钮 + 下划线说明文字：remembers for this workspace）。

### 4.4 问答卡片（ask_user）

```
┌ ? Loom asks ─────────────────────────────────────────────────────┐
│ Which migration strategy should I use?                           │
│  ○ expand-contract    ○ big-bang (faster, riskier)               │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ custom answer… (optional)                                  │  │
│  └────────────────────────────────────────────────────────────┘  │
│ [ Submit ]                                        [ Skip ]       │
└───────────────────────────────────────────────────────────────────┘
```

- `POST /v1/sessions/{id}/questions/{question_id}`，body `{selected, custom_text, skipped}`。
- `question.asked.payload.allow_multiple=true` 时选项渲染为 checkbox（多选数组进 `selected`），否则 radio 单选。
- 409 `binding_mismatch`（未知/已答）→ 与审批同处理：撤卡 + notice。
- 首屏恢复：`snapshot.pending_requests` 中 `kind=question` 条目重建卡片。

### 4.5 Diff 视图

unified diff 字符串有两个来源：**`tool.prepared.payload.diff`**（edit 类工具建块时挂载，展开可见；审批卡片直接移用工具块上已渲染的 diff 节点、收编后移回，`approval.requested` 不再重复传输；`tool.completed` 只携带 `preview` 有界输出摘要，不含 diff），以及 **snapshot 历史重建时由 `diff.js` 的 `diffForToolCall` 从 tool_call 参数本地重算**（diff 不落盘；write=纯新增免 LCS，edit 双侧 LCS 上限 400 行/侧，与 render/diff.go 同算法）。web 端 diff 不截断（`domain.ToolDiffUnbounded`；TUI 仍按 `ToolDiffMaxLines` 收敛）：超过 30 行的 diff 折叠为 details（summary 显示文件名与 `N lines · +a −d` 统计），展开即完整内容。`diff.js` 解析为 `{file?, hunks: [{lines: [{type: add|del|ctx, text}]}]}`，渲染为表格行：`+` 行绿底（`--success` 10% 透明度）、`-` 行红底、上下文行默认；行首显示 +/- 符号；行内代码按文件扩展名做 highlight.js 语法高亮（经 `sanitizeHtml` 白名单）。MVP 不渲染行内字符级 diff 高亮（留后续）。

### 4.6 会话列表与实时徽标

- 进入主界面 `GET /v1/sessions?limit=30` 建列表；滚动近底时带 `cursor` 拉下一页（keyset 分页，响应含 `next_cursor`）；周期刷新按「已加载数量」重拉首页大小并整列替换。
- 条目为紧凑单行（标题 + 相对时间）；`parent_session_id` 非空的子 agent 会话缩进展示在父会话下方（父不在已加载页内时按顶层兜底渲染）。
- 子 agent 会话只读：`snapshot.delegated=true` 时 composer 与模型/reasoning 切换禁用、header 显示 read-only 徽标；服务端同步拒绝 `POST /prompts`（`invalid_input`）。审批/提问不受限（当前子 agent 策略为只读白名单、不产生 Ask 升级，该通道为前向兼容保留）。
- 条目管理：hover 显示归档/删除按钮；删除走 `DELETE /v1/sessions/{id}`（store 级联清理 events/checkpoints/artifact_refs，显式清 file_changes；live handle 先 shutdown），归档走 `POST /v1/sessions/{id}/archive`（schema v4 `archived_at_unix_nano` 列，默认列表过滤）；侧栏底部「归档」按钮切换归档视图（含取消归档），删除当前打开的会话回空态。
- 徽标更新两条路（按 M3c 落地情况二选一，设计预置）：
  - **首选**：多会话合并流 `GET /v1/events?sessions=a,b`（SERVE_DESIGN §5.3，M2 已裁切到本里程碑），单连接驱动全部徽标——规避浏览器每域 6 连接限制（R9）。
  - **兜底**：标签页可见时每 5s 轮询列表接口（`visibilitychange` 暂停）。
- 条目展示所需的模型/状态/turn 数/标题来自富化后的 `SessionSummary`（§7.7）；在此之前侧栏只显示 id + 更新时间。
- 会话切换：`Cancel` 当前 SSE → `GET snapshot` → 重建 → `after=event_seq` 新订阅。
- **首入落地态**：进入主界面默认选中最近更新的会话（列表第一项）并加载；无任何会话则主区渲染空态引导（「New session 开始」），不自动创建。

---

## 5. 事件 → UI 映射

未知 kind 一律忽略（契约第 2 条）。✱ = MVP 必需；○ = 可降级为 notice 或忽略（后续迭代增强）。

| 事件 kind | UI 动作 | MVP |
|---|---|---|
| `turn.started` | 追加 user 块；状态徽标 → running；清 composer | ✱ |
| `turn.finished` | 状态 → idle；stream 草稿 finalize（若有残余） | ✱ |
| `model.text_delta` | 追加到 stream 块（rAF 合帧批量 DOM 更新） | ✱ |
| `model.reasoning_delta` | 追加到 reasoning 折叠块 | ✱ |
| `model.response_completed` | stream 块 → assistant 块（markdown 渲染替换） | ✱ |
| `model.tool_call_delta` | 工具块参数区增量 | ○ |
| `model.request_started/failed` | notice / 错误条 | ○ |
| `tool.prepared` | 建工具块（名称+参数摘要；edit 类挂载 diff，展开可见） | ✱ |
| `tool.started` | 工具块状态 → running（spinner） | ✱ |
| `tool.progress` | 工具块进度行 | ○ |
| `tool.completed` | 状态徽标（✓/✗/耗时）；`preview` 输出默认折叠（details），展开显示有界摘要，copy 按钮经 snapshot 按 call_id 取完整输出（无 diff，diff 在 prepared 阶段已挂载） | ✱ |
| `approval.requested` | 建审批卡片；pending.approvals 登记；非视口时提示 | ✱ |
| `approval.resolved` | 撤卡 → notice（含 actor） | ✱ |
| `question.asked` / `question.answered` | 问答卡片生命周期 | ✱ |
| `steer.queued` / `steer.injected` | pending steer notice 生命周期：queued 入队（FIFO），injected 移除队首并转正式 user block；`turn.started` 命中接力 prompt 的 notice 一并移除；snapshot 由 `pending_steers` 重建 | ✱ |
| `run.cancel_requested` / `run.cancelled` | 状态 → cancelling → idle + notice「cancelled」 | ✱ |
| `run.completed` / `run.phase_changed` | 状态徽标同步 | ○ |
| `context.compacted` | notice + ctx 条刷新 | ✱ |
| `context.usage` / `usage.updated` / `budget.updated` | 状态栏数值 | ✱ |
| `budget.notice` | notice（黄色） | ○ |
| `plan.updated` | 钉在 composer 上方的 plan 面板（可折叠清单，就地更新；snapshot 经 `plan` 字段重建） | ✱ |
| `subagent.started/progress/finished` | 子 agent 嵌套块（M3c 钻取视图；MVP 折叠块） | ○ |
| `runtime.warning` / `runtime.fatal` | 黄色 notice / 红色错误块 | ✱ |
| `session.opened` / `session.closed` | 侧栏徽标（合并流时） | ○ |

服务端事件（非 runtime event）：`server.resync` → 全量重建；`server.draining` → 停止重连 + 横幅；instance 首帧变化 → 等同 resync。

---

## 6. 样式设计

### 6.1 主题：Everforest 双色系（与 TUI 同源）

直接移植 `internal/ui/theme.go` 的两个 palette 为 CSS custom properties，双主题一套结构：

```css
:root, [data-theme="dark"] {          /* Everforest Dark Hard（默认） */
  --bg0: #1e2326;  --bg1: #272e33;  --bg2: #2e383c;
  --fg:  #d3c6aa;  --muted: #859289;
  --primary: #7fbbb3;  --success: #a7c080;  --info: #83c092;
  --warning: #dbbc7f;  --error: #e67e80;    --highlight: #e69875;
  --purple: #d699b6;
  --on-accent: #1e2326;
}
[data-theme="light"] {                /* Everforest Light Hard */
  --bg0: #fffbef;  --bg1: #efebd4;  --bg2: #f2efdf;
  --fg:  #5c6a72;  --muted: #a6b0a0;
  --primary: #3a94c5;  --success: #8da101;  --info: #35a77c;
  --warning: #dfa000;  --error: #f85552;    --highlight: #f57d26;
  --purple: #df69ba;
  --on-accent: #fffbef;
}
```

主题选择：默认跟随 `prefers-color-scheme`（`matchMedia` 监听系统切换）；header 的 ☀/☾ 按钮手动覆盖，选择写 `sessionStorage.loom_theme`（与 token 同生命周期，不落 localStorage）。

### 6.2 字体与排版

```css
--font-ui:   -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", sans-serif;
--font-mono: ui-monospace, "SF Mono", SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
```

- 基准 14px；transcript 正文 14.5px / line-height 1.65；工具块/状态栏 12.5px；标题 15px/600。
- 间距刻度 4px 基：`--sp1:4px … --sp6:24px`；块间距 16px；卡片内边距 12–16px。
- 圆角：块/卡片 8px；按钮 6px；徽标 999px。
- 最大内容宽：transcript 列 `min(920px, 100%)` 居中（超宽屏不拉满，长行可读性）。

### 6.3 组件样式语义（与 TUI 块一一对应）

| 组件 | 样式 | TUI 对应 |
|---|---|---|
| user 块 | 左侧 3px `--primary` 边条 + 左 padding；标签 `You` 加粗 primary | `UserBlock`/`UserLabel` |
| assistant 块 | 无装饰，markdown 正文 | `AssistantBlock` |
| stream 草稿 | 同 assistant + 末尾呼吸光标 `▍`（CSS animation） | `StreamBlock` |
| reasoning 折叠块 | `--bg1` 底 + 左侧 `--primary` 细边 + `--muted` 文字；`<details>` 原生折叠 | `ReasoningBlock` |
| tool 块 | 左侧 3px `--muted` 边条 + `--bg1` 面板；头部：名称（mono）+ 状态徽标 + 耗时 | `ToolBlock` |
| 工具状态徽标 | running=`--success` 脉冲点；ok=`--success` ✓；error=`--error` ✗；canceled=`--muted` ⊘ | `ToolRunning/Success/Error/Canceled` |
| diff 行 | add：10% `--success` 底 + 行首 `+`；del：10% `--error` 底 + `-`；ctx 默认 | — |
| approval 卡片 | 2px `--warning` 边框 + 8px 圆角 + 内边距 16px；标题 warning 加粗 | `ApprovalBorder/Title` |
| question 卡片 | 2px `--primary` 边框；选项为原生 radio + 自定义 focus ring | — |
| notice | `--muted` 斜体单行 | `NoticeBlock` |
| fatal 块 | `--error` 文字 + 10% error 底 | — |
| 按钮 | 主按钮：`--primary` 底 `--on-accent` 字；次按钮：透明底 1px `--muted` 边；danger：`--error` 边；hover 8% 提亮，focus-visible 2px outline | `ApprovalSelected` 等 |
| 状态徽标（header/侧栏） | 圆点 + 文案；running 绿色脉冲（`@keyframes pulse` 透明度 1→0.4） | StatusBar* |
| composer | 1px `--muted` 边 + 8px 圆角；focus 时边色 `--primary` | `Composer` |

### 6.4 动效与可访问性

- 动效仅三处：stream 光标呼吸、running 徽标脉冲、块入场 fade-in（120ms）。`prefers-reduced-motion` 时全部关闭。
- 交互元素 keyboard 可达（Tab 序 + `focus-visible` 环）；审批/问答卡片到达时 `aria-live="polite"` 通告；色彩不作唯一信息载体（徽标均带文字/符号）。
- 对比度目标：正文/背景 ≥ 7:1（AAA），弱化文字（muted）与徽标 ≥ 4.5:1（AA）——vendoring 落地时对最终色板做一轮实测抽检，不达标就微调色值而不是降目标。

---

## 7. 服务端配套改动（M3 范围内）

| # | 改动 | 说明 |
|---|---|---|
| 7.1 | `internal/server/web/web.go`：`//go:embed static` + `GET /` 静态服务 | `index.html` `Cache-Control: no-store`；其余资产 `?v=<version>` 查询串缓存；`--no-web` 时 404（纯 API 模式）；静态资源**不要求 token**（引导页必须匿名可达，敏感数据全在 /v1/*）。Bazel 侧：静态文件走 `embedsrcs`（gazelle 识别 `//go:embed` 指令自动生成） |
| 7.2 | 安全响应头（静态 + API 统一） | `Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'`（`unsafe-inline` 仅限 style：组件内联样式少量使用；script 严格 self，内联脚本零容忍；`img-src` 的 `blob:` 供 artifact 图片经 `URL.createObjectURL` 渲染——`<img>` 无法携带 Authorization 头）；`X-Content-Type-Options: nosniff`；`Referrer-Policy: no-referrer`；`X-Frame-Options: DENY`（防点击劫持，见 §8） |
| 7.3 | CORS preflight（`OPTIONS`）处理 | M2 审查遗留：`--allow-origin` 设置时响应 preflight（`Allow-Headers: Authorization, Content-Type`，`Allow-Methods` 按路由）；未设置时维持全拒。**仅远程前端开发场景需要**；内嵌 SPA 同源无需 CORS |
| 7.4 | `GET /v1/events?sessions=a,b` 多会话合并流（M3c） | SERVE_DESIGN §5.3/F8：侧栏徽标单连接驱动，规避浏览器每域 6 连接限制（R9）；M3a 以轮询兜底先行 |
| 7.5 | `GET /v1/sessions/{id}/subagents/{child}` 钻取端点（M3c） | §5.3 已列（可裁切项）；MVP 子 agent 以折叠块降级呈现 |
| 7.6 | `SessionSummary` 补 snake_case JSON tag | 现状无 tag，wire 输出 `{"ID":…,"Version":…}` 与协议其余部分的 snake_case 不一致；M2 刚发布、尚无外部消费者，趁 M3 前修正（协议 v1 内修补，不升版本） |
| 7.7 | `SessionSummary` 富化（侧栏数据缺口） | 侧栏条目要展示模型/状态/turn 数/标题，但现状只有 ID/Version/时间戳。增加：`model_name`、`state`（live 会话的控制器态，非 live 为 `"closed"`）、`turn_count`、`title`（首条 user prompt 截断 50 字符）。纯增量字段，协议 v1 内兼容 |

无 SessionService 语义改动——M3 的全部服务端工作仍是传输适配层，这本身即是对 M1' 架构的验证。

---

## 8. 安全设计

1. **XSS（R7，高风险）**：`innerHTML` 仅两个入口，统一收敛在 markdown.js 过 DOMPurify 白名单（`USE_PROFILES: {html: true}, FORBID_TAGS: ['style'], FORBID_ATTR: ['style']`）：marked 输出与 hljs 高亮片段；其余一切 DOM 写入 `textContent`。工具输出/推理文本全部纯文本节点。
2. **负例验收**（进 M3 验收清单）：构造模型输出含 `<script>alert(1)</script>`、`<img src=x onerror=…>`、`<a href="javascript:…">`、`markdown 链接 javascript: URI` 的 fixture 事件，断言渲染后 DOM 中无可执行载荷。
3. **token**：`sessionStorage` 仅存；页面任何位置不回显完整 token；不以任何形式进 URL（含 hash）。
4. **CSP**：§7.2 的策略使内联脚本与第三方源在浏览器层直接不可执行，构成 sanitize 之外的第二道防线。
5. **CSRF**：协议无 cookie，token 走 header，浏览器无法隐式携带——天然免疫（SERVE_DESIGN §6 已论证）。
6. **点击劫持**：`X-Frame-Options: DENY`（loopback 场景防恶意页面嵌套诱导点击审批按钮）。

---

## 9. 测试策略

| 层 | 内容 | 工具 |
|---|---|---|
| Go 单测 | 静态服务：路由/Content-Type/缓存头/CSP 头/`--no-web` 404；合并流端点（7.4） | `httptest`（现有 server_test 模式） |
| JS 纯逻辑 | sse.js 解析器（多行 data/注释/resync 帧）、diff.js、退避计算、store reducer——全部零依赖纯模块 | `node --test`（开发机本地跑，不进 CI 门禁；CI 无 node 基建） |
| 协议回归 | SPA 上线后既有契约测试（inproc/http 双跑）不受影响即为通过 | bazel（已有） |
| 浏览器验收 | 手工清单（§10 验收标准逐条过）+ XSS 负例 fixture | 手工 |
| 性能 | 长会话（500+ 块）滚动/流式帧率；delta 高频（>50/s）下 rAF 合帧无掉帧 | 手工 + Performance 面板 |

明确不做：JS 单元测试进 CI 门禁（无 node 工具链，引入成本远超收益）；Playwright/Puppeteer E2E（同上，留 M4 评估）。

---

## 10. 里程碑拆分与验收

### M3a 骨架与核心对话（~2 人日）

范围：静态服务（7.1/7.2/7.6）+ token 引导 + 主布局 + snapshot/SSE/reconnect + 发送/steer/取消 + user/assistant/stream/reasoning/tool/notice 块 + 状态栏 + 双主题。

验收：建会话 → 多轮流式对话 → 取消 → 恢复 → 刷新重连 → resync（kill 重启 server 后自动恢复）全通；`--no-web` 404；CSP 头正确。

### M3b 交互卡片与 diff（~1.5 人日）

范围：审批卡片（含 Allow always/binding 竞态/actor 展示）+ 问答卡片 + diff 视图 + XSS 负例。

验收：双浏览器标签页竞决审批恰好一个成功（败方 409 撤卡）；首屏 `pending_requests` 重建；负例 fixture 无一执行。

### M3c 会话管理与打磨（~1 人日）

范围：侧栏会话列表 + 合并流徽标（7.4）+ 子 agent 钻取（7.5）+ CORS preflight（7.3）+ 窄屏适配 + 动效/可访问性收口。

验收：SERVE_DESIGN §12 M3 行全项：「浏览器完整跑通 chat/流式/工具块/diff/审批/问答/取消/恢复/刷新重连/resync；XSS 负例不执行」。

---

## 11. 风险登记

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| W1 | XSS 经模型输出注入（R7 前移） | 高 | §8 双防线（DOMPurify + CSP）；负例进 M3b 验收硬门槛 |
| W2 | 零框架下组件失控（回调地狱/选择器漂移） | 中 | §2.3 迁移触发条件前置声明；组件纯函数化 + store 单一事实源 |
| W3 | fetch SSE 在个别代理/浏览器组合下的缓冲行为差异 | 低 | server 已有 15s 心跳；`X-Accel-Buffering: no` 已设置；断连即重连兜底 |
| W4 | 长会话 DOM 节点膨胀卡顿 | 中 | rAF 合帧 + 块数软上限（超出折叠为「N 个早期块」占位）+ 「加载更早」分页；虚拟化留开放问题 |
| W5 | vendored 包供应链 | 低 | 仅 2 包；版本+SHA-256 锁定；CSP self 限制外联 |

---

## 12. 开放问题

| # | 问题 | 默认倾向 |
|---|---|---|
| Q1 | 框架迁移时机 | §2.3 触发条件；届时 Preact+htm（仍零构建） |
| Q2 | 代码高亮 | MVP 不做；后续 vendored highlight.js 子集或自写常见语言 tokenizer |
| Q3 | transcript 虚拟滚动 | 待 M4 性能基线；先块数软上限 + 分页 |
| Q4 | 移动端 | 桌面优先；窄屏抽屉式侧栏保可用，不优化触控交互 |
| Q5 | 图片附件（协议已支持 `images`） | M3 不做上传 UI（TUI 侧刚支持）；下个迭代加粘贴/拖拽 |
| Q6 | i18n | 英文文案随 TUI；中文需求出现后再评估 |
| Q7 | 会话重命名/删除 | 协议无端点（M3+）；MVP 只读列表 |
| Q8 | token 每标签页隔离的摩擦 | `sessionStorage` 是 per-tab 语义：新标签页要重输 token。这是 SERVE_DESIGN §9 定的安全取舍（关页即清），暂不改变；若真实使用摩擦大，可加 opt-in 的「remember this browser」（落 localStorage + 引导页明示风险），不作为默认 |
