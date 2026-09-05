# Loom Neovim UI 设计

> 状态：Draft（Phase 1 已实现：渲染分层 + 编辑器联动 + Nerd Font 图标体系）  
> 日期：2026-09-05  
> 前置文档：`SERVE_DESIGN.md`（REST+SSE 协议权威 spec）、`VIM_UI_DESIGN.md`（TUI vim 化交互语义，本文复用其决策，但两者是不同前端：VIM_UI_DESIGN 改造的是 Bubble Tea TUI 内部交互，本文是独立的 Neovim 插件客户端）

## 1. 背景与动机

`SERVE_DESIGN.md §3.1` 定案：loom 的所有前端（TUI / Web SPA / desktop / curl）都是 `/v1/*` REST+SSE 协议的**平权纯渲染客户端**，协议语义全部下沉在应用层（SessionService）。WebUI（`internal/server/webui`）走 HTTP+SSE；desktop（`cmd/loom-desktop`）把 `server.Handler()` 嵌进 webview。

本文交付第 4 个平权客户端：**Neovim 插件**（纯 Lua），直接面向 `loom serve` 的 REST+SSE 协议，不触碰任何 Go 代码。

**精准定位：Neovim UI 不是第二个 WebUI，它是代码开发前端。** WebUI 面向通用聊天，Neovim UI 面向「正在写代码的人」——核心价值是与编辑器的联动：随手把选区/文件上下文丢给 agent、transcript 中的路径一键跳回编辑窗口、在编辑器里完成审批，全程不离开编辑态。动机：

1. **编辑态原生融合**：vim 用户在编辑器内完成对话、审批、查看 diff，上下文切换成本最低；
2. **导航零成本**：`gg/G/Ctrl+D/Ctrl+U`、`/`、`n/N`、buffer 搜索全部是 vim 原生能力——`VIM_UI_DESIGN.md Phase 4/5` 想在 TUI 里模拟的浏览态，在真 Neovim 里是开箱即得；
3. **markdown 高亮零成本**：transcript buffer 设 `filetype=markdown` + `conceallevel=2` 即获得完整语法渲染，无需再维护 glamour 缓存。

## 2. 目标与非目标

### 2.1 目标

1. **纯 Lua 客户端**：实现 REST（curl 一次性调用）+ SSE（`curl -N` 流式 job）两个传输层，语义完全复刻 `webui/src/protocol/sse.ts` 的重连/watchdog/resync 语义；
2. **协议正确性优先**：首屏 "snapshot → SSE attach" 不重不漏；resync 只由 `server.resync` 帧 / `instance` 变化触发（客户端不查 sequence 连续性，见 §4.2）；`model.response_completed` 用 canonical 文本校正 lossy delta 草稿；
3. **serve 生命周期托管**：插件自动探测 `127.0.0.1:7680` 的 `healthz`，不活则以子进程拉起 `loom serve`（可选保持常驻）；
4. **vim 原生交互**：transcript/composer/approval/sessions 四个 UI 单元全部以 buffer+浮窗组织，复用 `vim.ui.select`，不引入大依赖；
5. **零外部依赖**：仅依赖 `curl` 与 `nvim >= 0.10`（`vim.system`、`vim.json`），不需要 plenary/telescope/nui；用户装 fzf-lua/telescope 时自动接管 `vim.ui.select`。

### 2.2 非目标

- **不实现 RPC 双向通道**：approval/question 决议走 REST POST（与 WebUI 一致），不引入标准 SSE 之外的回传通道；
- **不内嵌 serve 进程内 Go 逻辑**：与 desktop 的"宿主治 handler"模式不同，插件只走网络协议；单实例 flock（`loom.lock`）天然防止插件拉起的 serve 与既有 instance 冲突；
- **Composer 不做 modal 编辑**：延续 `VIM_UI_DESIGN.md §2.2` 的决策，输入域永远 insert-first，Esc 即退出回 transcript（normal 模式不需要再成为聊天输入的常规状态）；
- Phase 1 不做 workspace 文件树、`@`补全、git diff 面板、subagent 钻取、多 session 并存（见 §8 路线图）。

## 3. 总体架构

### 3.1 进程拓扑

```text
Neovim                                     loom serve（127.0.0.1:7680）
┌──────────────────────────────┐           ┌───────────────────────────┐
│ lua/loom/init.lua（setup）    │           │ internal/server（适配器#1） │
│  ├─ server.lua（探测/拉起）   │ ──job──▶  │  loom serve               │
│  ├─ http.lua（curl oneshot）  │ ──REST──▶ │  GET/POST /v1/*           │
│  ├─ sse.lua（curl -N 流）     │ ──SSE───▶ │  GET /v1/sessions/{id}/   │
│  ├─ events.lua（kind 分发）   │           │      events?after=<seq>   │
│  ├─ state.lua（会话状态机）   │           └───────────────────────────┘
│  └─ ui/*.lua（buffer/浮窗）   │
└──────────────────────────────┘
```

### 3.2 关键技术决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| HTTP 客户端 | `vim.system / jobstart + curl` | Neovim 无原生 streaming HTTP；`curl -N --no-buffer` + stdout 回调是社区事实标准（CopilotChat/avante 同构）；零 Lua 依赖 |
| SSE 解析 | 增量子字符串切分（`\n` 行缓冲 + 空行边界） | curl stdout 按 chunk 到达，必须容忍半包；按 `id/event/data` 字段组帧，注释行（`: connected`、`: hb`）单独通道 |
| JSON | `vim.json.decode`（nvim ≥ 0.9），容忍空 body | 无第三方 JSON 库 |
| 认证 | 自动读 `~/.loom/sessions/serve.token`（`LOOM_HOME` 定位），也可 `config.server.token` 显式覆盖 | 与 server 生成的 0600 文件语义一致，用户零配置 |
| 布局 | 右侧 vsplit 承载 transcript，composer 是其底部 split（prompt buffer） | 与 coder companion 类插件一致 |
| `vim.ui.select` 承载 sessions/model/reasoning picker | 用户未装插件时为原生 UI；装了 fzf-lua/telescope/dressing 自动表达为现代化 picker | 零依赖前提下 max flex |
| 渲染分层 | `blocks.lua`（纯函数：状态 → lines+deco）→ `transcript.lua`（block 区间管理） | 渲染可单测；样式与 buffer 操作解耦 |
| markdown 渲染 | 助手正文保留原始 markdown，靠 `filetype=markdown + conceallevel=2`；reasoning 默认折叠为一行提示 | 不做 ANSI 转义 |
| 工具输出 | `│ ` 前缀 + dim 行级高亮；run_cmd 风格 JSON 负载（stdout/stderr/exit_code）结构化拆分 | 避免 fence 孤儿高亮与 JSON 原文糊屏 |
| 图标体系 | Nerd Font Font Awesome 段（`hl.lua` 集中一张表 `M.icons`，可覆盖） | 角色/动作/状态语义清晰；不用 emoji（渲染宽度不稳定性在代码编辑器里是视觉噪音） |
| 事件分发 | `events.lua` 按 `kind` switch，未知 kind 忽略（对齐 wire 协议契约 clause 2） | 协议平滑演进 |
| 等待反馈 | `spinner.lua` uv timer 驱动帧动画 + 已耗时，tick 即重绘信号 | 卡死与推理可区分 |

### 3.3 与 avante.nvim 的差距与站位（诚实对照）

avante 是成熟的单端插件，Loom Neovim UI 是「平权多端客户端」——形态不同，但用户体感的可比项要诚实对齐：

| 能力 | avante | 本插件现状 | 计划 |
|------|--------|-----------|------|
| 选区上下文发送 | ✅ | ✅（`:'<,'>LoomSend`，含路径/行号/语言 fence） | - |
| 编辑器内 diff | ✅（inline suggestion） | ✅ 双入口：审批时刻 proposed 预览 + 自动批准后事后 before/after 铺屏（vim diffmode，任意端决议/手动 q 收敛） | 决议粒度细化到 hunk（Phase 3） |
| 代码块快速取用 | ✅ | ✅（`gy` 复制 / `gi` 插入编辑窗） | - |
| 历史消息渲染 | ✅ | ✅（`history.lua` 重放 snapshot.messages；idle 快照限定） | - |
| `@` 文件提及补全 | ✅ | ✅（search API 补全 + 提交时展开 fenced 内容，winbar `ctx` 用量） | - |
| 会话内 token / 模式切换 | ✅ | ✅ token（occupancy/context_window）；模式切换无 | Phase 2 剩余 |
| **多客户端平权 / 断线续挂 / 审批多先入** | ❌ 单端架构 | ✅ 协议级（与 TUI/WebUI 同会话观） | - |
| serve 托管与协议演进容忍 | N/A | ✅ auto_start/readyz/SSE reconnect/resync | - |

## 4. 协议接入契约

客户端严格按 `SERVE_DESIGN.md §5` 执行，语义以该文档为准，本节只列接入序列与必须遵守的坑。

### 4.1 启动与首屏协议

```text
1. GET /healthz（免认证）               # server.lua
   └─ 失败且 auto_start → jobstart("loom serve --listen 127.0.0.1:7680")
      轮询 /readyz 至 startup_timeout_ms（默认 10s）
2. GET /v1/meta/version                 # 协议协商；protocol ≠ 1 → report error 并中止
3. POST /v1/sessions（{resume?} 或 {}）  # LoomNew / LoomSessions 恢复
4. GET /v1/sessions/{id}/snapshot       # 拿 event_seq 作为 cursor，重建 pending_requests 卡片
5. GET /v1/sessions/{id}/events?after={event_seq}   # 打开 SSE
```

第 4→5 步"补拉-订阅原子衔接"由 server 的 ReplayLog 保证（`SERVE_DESIGN.md §4.5`），客户端只需保证两步顺序。

### 4.2 断线重连与 resync

- **重连**：curl job 终止（exit≠0 或 EOF）后按 1s→15s 指数退避重挂 SSE，cursor 用已应用的最大 sequence（`after = max(seen)`）；
- **客户端不做 sequence +1 连续性检查**：sequence 是全局 broker 序号（跨会话共享、天然稀疏），+1 检查会把其他会话推进的序号误判成本会话丢帧而刷屏 resync。gap 判定是服务端 `Replay.Since` 的职责，不可挽回时服务端发 `server.resync` 帧；
- **chunk 解析**：jobstart 每 chunk 的首个元素要接续上个 partial line（拼接后本 chunk 就要处理，丢它会丢行——尤其丢 blank 分隔行导致相邻帧合并）；最后一个元素是新 partial，不处理；
- **instance 变化**：首帧 `: connected, instance=<id>` 与上次不同 → resync（loomd 重启）；
- **`server.resync` / `server.draining`** 事件：关闭 SSE，走 resync；帧 `data` 携带 `reason`（`cursor_invalid` / `shutdown`），通知文案透传真实原因；
- **resync**：`GET snapshot` → 清空动态块（pending 卡片、steer 徽标、流式草稿）→ 以新 `event_seq` 重挂 SSE。数据终态以 snapshot 为准（幂等）。

### 4.3 必须遵守的协议坑

1. `model.text_delta` 是 lossy 的：可见流式草稿是**辅助渲染**，`model.response_completed.payload.text` 到达时必须用 canonical 文本**整体替换**（`SERVE_DESIGN.md §5.4`）；
2. `approval.requested` / `question.asked` 是 ephemeral：断开重连后必须从 `snapshot.pending_requests` 重建卡片；
3. broker 对慢订阅者直接断连：心跳注释（`: hb <unix>`，15s）之外 > 30s 无数据且连接仍存活 → 主动断开重连；
4. busy 时提交 prompt：server 返回 `{steered:true, queue_len}`（SSE 随后会看到 `steer.queued`/`steer.injected`）；UI 不得误报"已发送"。Phase 1 以 notify 提示；
5. `turn.started.payload.prompt` 是用户消息唯一权威来源（steer 注入也被包含）；**用户消息块在 `turn.started` 时渲染，而不是在提交回执时本地 echo**——后者会在 steer/followup 场景下产生重复块。

### 4.4 API 最小面（Phase 1）

| 用途 | 端点 |
|------|------|
| 探测 / 协商 | `GET /healthz`、`GET /readyz`、`GET /v1/meta/version` |
| 会话 | `GET /v1/sessions`、`POST /v1/sessions`（新建/resume）、`GET /v1/sessions/{id}/snapshot` |
| turn | `POST /v1/sessions/{id}/prompts`、`POST /v1/sessions/{id}/cancel` |
| 审批 / 问答 | `POST /v1/sessions/{id}/approvals/{approval_id}`、`POST /v1/sessions/{id}/questions/{question_id}` |
| 事件 | `GET /v1/sessions/{id}/events?after=`（SSE） |

Phase 2 追加：`/v1/meta/models`、`POST /v1/sessions/{id}/model`、`/reasoning`、`/compact`。Phase 3 追加：`/v1/workspaces/*`（文件树、`@` 补全、git diff）、`/v1/artifacts`、`/maze`、`/feedback`。

## 5. 插件结构

```text
go/pl/loom/neovim/
├── plugin/
│   └── loom.lua                # :Loom* 命令注册（guard vim.g.loaded_loom）
├── lua/loom/
│   ├── init.lua                # setup / 命令注册 / 客户端主入口
│   ├── config.lua              # 默认值合并（vim.tbl_deep_extend）
│   ├── server.lua              # healthz 探测、serve job 生命周期、token 解析
│   ├── http.lua                # curl 一次性请求 helper
│   ├── api.lua                 # typed REST 方法（§4.4 最小面）
│   ├── sse.lua                 # SSE job：帧解析、心跳、重连、resync 上报
│   ├── events.lua              # RuntimeEvent kind → state/ui 动作（分发层）
│   ├── state.lua               # 会话状态机：run 草稿、pending、事件水位
│   ├── util.lua                # notify / json / time helpers
│   ├── health.lua              # :checkhealth loom
│   └── ui/
│       ├── init.lua            # 布局编排（transcript + composer）
│       ├── hl.lua              # 高亮组统一定义（全 default，主题可覆盖）
│       ├── blocks.lua          # 纯函数渲染器：状态 → lines+deco（不触 buffer，可单测）
│       ├── transcript.lua      # 通用 block 管理（buffer/deco/follow/winbar/欢迎页）
│       ├── spinner.lua         # uv timer 驱动的等待动画（帧 + 已耗时）
│       ├── composer.lua        # prompt buffer 输入条（❯ 提示，<CR> 发送）
│       ├── approval.lua        # 审批卡 + 问答决议（vim.ui.select）
│       └── sessions.lua        # 会话列表 picker（vim.ui.select）
└── specs/                      # 单元测试（plenary.nvim，Phase 1 骨架后续补）
```

配置默认（`config.lua`，用户 `require("loom").setup({...})` 深合并）：

```lua
{
  server = {
    url = "http://127.0.0.1:7680",
    auto_start = true,           -- 探测失败时自动 jobstart loom serve
    startup_timeout_ms = 10000,  -- 轮询 /readyz 上限
    loom_bin = "loom",           -- PATH 内可执行；可为绝对路径
    token = nil,                 -- 显式覆盖；nil 则从 token file 读取
    token_file = nil,            -- 默认 <LOOM_HOME|~/.loom>/sessions/serve.token
  },
  ui = {
    position = "right",          -- right | left（vsplit）
    width_ratio = 0.40,          -- transcript 占编辑器宽度比
    composer_height = 3,         -- prompt buffer 输入条
    max_tool_preview_lines = 20,
    max_tool_diff_lines = 40,
    reasoning_style = "hint",    -- hint（一行折叠提示）| full | hide
  },
  keymaps = {
    -- composer 是 prompt buffer：Insert/Normal 下 <CR> 发送，无需 submit 键位
    cancel = "<C-c>",            -- composer normal：取消当前 turn
    approve = "y",               -- 审批卡（transcript buffer-local）
    deny = "n",
  },
}
```

## 6. UI 设计

### 6.1 布局

```text
┌────────────────────────────────────┬──────────────────────────┐
│                                    │ ◐ loom · running · model │  winbar（pill 状态）
│   用户编辑区（原 buffer）           │ 🧑 你（pill bar）         │
│                                    │ ……                       │
│                                    │ 🤖 loom · model   ⠹ 3.2s │  run 头部（spinner）
│                                    │ 助手流式 markdown…       │
│                                    │ ● run_cmd  ls · 0.4s     │  工具条（区间级着色）
│                                    │ │ stdout 行（dim）       │
│                                    │ ⚠ 需要批准 — write_file  │  审批卡
│                                    ├──────────────────────────┤
│                                    │ loom · <CR> 发送 · <C-c> │  composer winbar（dim）
│                                    │ ❯ 输入（prompt buffer）   │
└────────────────────────────────────┴──────────────────────────┘
```

- `:Loom` 打开/聚焦；`:LoomHide` 收起（状态保留）；`:LoomClose` 关停会话 UI（serve 根据配置决定是否保持）。
- Transcript buffer：`buftype=nofile filetype=markdown conceallevel=2`（语法标记符隐藏）；`modifiable` 只在渲染时短暂置真。首启显示欢迎页，首个 `turn.started` 到来时经 `clear_kind("welcome")` 清除。
- Composer buffer：`buftype=prompt`，`❯` 提示符；Insert/Normal 下 `<CR>` 发送，`<Esc>`（normal）回 transcript。

### 6.2 Transcript 渲染分层（`blocks.lua` / `transcript.lua` / `spinner.lua`）

- **blocks.lua（纯函数渲染器）**：状态 → `lines + deco`，不触 buffer、可单测。deco 两种形态：
  - 行级 `{row, group, eol}`：pill bar（`hl_eol=true` 整行填充）/ dim 整行；
  - 区间级 `{row, col0, col1, group}`：字节区间着色（工具图标/工具名/target/耗时各自上色）。
- **transcript.lua（通用 block 管理）**：只管 buffer 生命周期、block 行区间（extmark，`right_gravity=false`）、deco 区间铺/清、跟随滚动、winbar；内容细节一律委托 blocks。所有写操作经 `with_modifiable` 单点收口。
- **spinner.lua（动画）**：uv timer（100ms）驱动 braille 帧 + 已耗时；`current()` 对渲染层只读采样，tick 回调即重绘信号。起停收敛在 `events.lua` 的 `finish()`（turn.finished / run.cancelled / runtime.fatal / request_failed 共用）。
- **`run` block 重渲染策略**：每只 turn 一个 run block，内容 = 头部（spinner）+ reasoning（hint/full）+ canonical/draft 文本 + 该 turn 全部 tool 条目（call_id 索引）。任何事件（delta/tool.*/spinner tick/response_completed）触发 **run range 整段 set_lines 重渲染**：
  - 正确性简单：canonical 文本替换、tool 交错都被垂直覆盖；
  - 性能可接受：只改 run range，nvim 增量重绘；preview 行数上限兜住极端尺寸。
- 工具正文不用 markdown fence（反复重渲会留下孤儿 ``` 高亮）；`run_cmd` 风格 JSON preview（`{stdout,stderr,exit_code}`）被结构化拆分呈现。
- Approval 卡片是**独立 block**（不参与 run range），决议后改为收敛行；`approval.resolved` payload 不带 tool_name，从请求阶段回放。
- `turn.started`：清欢迎页 → 追加 `user` block（payload.prompt）+ 新 `run` block + 启动 spinner；`turn.finished`：停 spinner、error 尾注。

### 6.3 审批与问答（`approval.lua`）

- `approval.requested` → 追加 approval block：warn pill 头 + target/risk/description/arguments（dim）；同时 `vim.notify` 提示，winbar 转 `awaiting_approval`。
- 决议：transcript buffer-local 的 `y`/`n` → `POST /v1/sessions/{id}/approvals/{approval_id}` `{call_id, args_hash, decision, client:"nvim"}`；server 广播 `approval.resolved` 后所有端卡片收敛（多客户端先到先得，语义见 `SERVE_DESIGN.md §4.6`）。
- 快照恢复：`snapshot.pending_requests` 中 `kind=="approval"` 重建卡片；`kind=="question"` 以 `vim.ui.select(options)` 答复，选定 → `POST .../questions/{id}` `{selected}`，Esc → `{skipped=true}`。
- Question 无独立 card（其互动性强），直接 `vim.ui.select`；Phase 2 再落专用卡。

### 6.4 Composer（`composer.lua`）

- prompt buffer（`prompt_setprompt("❯ ")` + `prompt_setcallback`）；Insert/Normal 下 `<CR>` 发送；`<Esc>`（normal）切回 transcript；`<C-c>`（normal）→ `POST /cancel`。
- **@ 文件提及**：`@` 后输入时 TextChangedI 驱动 `GET /v1/workspaces/{id}/files/search?q=`，`vim.fn.complete` 浮窗（代际号防乱序）；`@` 紧前是路径/字母字符（`pwd@host` 形）不触发。提交时扫描 `@path` token、逐个 `GET /file` 读取，以 ```ft fence 附在 prompt 末尾 `引用文件：` 段；原文 `@path` 原样保留。无 workspace_id 时静默降级。
- 提交动作：`trim(全文)` → 展开提及 → `POST /prompts`；buffer 由 prompt 语义自动清空。steered 则 notify `已入队`；失败按 `error.code` notify 文案。
- 用户消息渲染由 `turn.started` 事件权威完成（§4.3-5）。

### 6.5 Sessions picker（`sessions.lua`）

`:LoomSessions` → `GET /v1/sessions?limit=50` → `vim.ui.select({id, title, state, updated_at, turn_count})` → `POST /v1/sessions {resume:id}` → 回到 §4.1 第 4 步挂接。`vim.ui.select` 是 hook 点，装 fzf-lua/telescope 自动升级。

### 6.6 编辑器联动（`context.lua` / `diffview.lua`）

定位落地（§1）：

- **`:'<,'>LoomSend [说明]`**（usercommand `range=true`）：visual 选区 → prompt = `说明 + 上下文 \`path:l1-l2\` + \`\`\`lang fence`；没有会话时暂存并在 attach 完成后自动发送（`pending_send`）。不带范围时等同普通提问。
- **编辑器内 diff（`diffview.lua`）**：真实 buffer + `loom-diff://` scratch 成对铺开 vim 原生 diffmode（同步滚动/折叠由 vim 自带），scratch 侧 `q` 关闭、任意时刻 resync 全清（易失态）。两个入口共享同一 `open_pair` 引擎：
  - **审批时刻**：`approval.requested` 且工具为 `write`/`edit` 且带 arguments → 左 `:edit` 现状 / 右 `proposed` scratch（新文本按 arguments 客户端构造：write=content，edit=old_string→new_string 替换）。任意端 `approval.resolved` 自动收敛。
  - **事后时刻（自动批准路径的主反馈）**：`tool.prepared` 为 write/edit 时快照旧文本（执行前盘上未动），`tool.completed` 成功后 → 左 `before` 快照 scratch / 右 fresh 文件（`:edit` 触发 reload），同一 path 只留最新一组视图。受 `ui.diff_after_edit`（默认 `true`）开关。
  - 旧文本优先读已加载 buffer（所见即用户当前状态）；相对路径以 `snapshot.workspace_root` 为准（`getcwd()` 兜底）；目标 buffer 有未保存修改时跳过展示并告警（E37 前置检查，绝不动用户数据）。
- **transcript `<CR>`**：打开光标下的文件——优先 `expand("<cfile>")`（绝对/相对路径），退化解析行内 `path:line`；目标窗口是 tab 中第一个 `buftype==""` 的编辑窗口。
- **`gy` / `gi`（代码块取回）**：光标在 markdown fence 内时，`gy` 复制块体到无名寄存器，`gi` 沿行插入编辑窗口光标下方；fence 判定用「上方最近 fence 之前 fence 成对闭合」奇偶法，嵌套 fence 不误剖。
- 方向已定（Phase 2/3 深化）：`@` 文件补全走 workspace API、diagnostics 上下文、code action 类快捷指令（explain/fix/test）、hunk 粒度审批。

### 6.7 历史重放与用量（`history.lua`）

- attach/resync 拿 snapshot 后：`state == "idle"` 时把 `snap.messages` 全量重放为 transcript 块（user → 普通 user 块；assistant → `blocks.history_assistant`：pill 头标注「· 历史」、reasoning 折叠 hint、正文、tool_call/result 按 call_id 配对的状态行、`interrupted` 尾注）。
- **渲染纪律**：idle 快照的水位是 turn 边界，SSE `after=event_seq` 续传不会重播这些事件，故历史块与后续 live 块严格衔接；running 态不重放（交给 live 事件流补齐），避免双写。
- winbar 追加 `ctx {occupancy}/{context_window}`（attach/resync 时刷新）。

## 7. serve 进程管理与健康检查

### 7.1 生命周期（`server.lua` / `init.lua`）

- `M.attach()` 是主入口：`healthz` 通过 → 直接连；未通过且 `auto_start` → `vim.fn.jobstart("loom serve --listen <host:port>")`，每 300ms 轮询 `readyz` 直至超时；超时即 notify error。
- `VimLeavePre` autocmd：由插件拉起的 serve 在退出时 `jobstop()`，除非 `config.server.keep_alive = true`（desktop / 其他客户端可能共用）。
- token 解析优先级：`config.server.token` > `<loom_home>/sessions/serve.token`（0600 文件，readfile 第一行 trim）。

### 7.2 `:checkhealth loom`（`health.lua`）

- nvim 版本 ≥ 0.10（`vim.system`、`vim.json`）；
- `curl` 可执行（`-sS -N` 支持）；
- server 可达（`healthz`）、token 可读、协议版本 = 1；
- `loom` 二进制可执行（仅当 `auto_start`）。检测结果以 `vim.health.ok/warn/error` 结构输出。

## 8. 阶段路线图

| Phase | 目标 | 依赖 | 状态 |
|-------|------|------|------|
| 1（MVP） | 骨架：§4 全协议接入；渲染分层（blocks/spinner/transcript）；prompt composer；approval/question；sessions picker；checkhealth；欢迎页；**编辑器联动（选区发送、`<CR>` 文件跳转、diffmode 审批、代码块取回）**；Nerd Font 图标体系 | 无 | 已实现 |
| 2 | ~~transcript 历史渲染~~（已提前：history.lua）；~~token 用量~~（已提前：winbar ctx）；~~`@` 文件提及~~（已提前至 Phase 3 项：composer search 补全 + 提交展开）；模型/推理档切换；question 专用卡；ask_user / plan.updated 渲染；steer/followup 徽标队列；UDS 支持 | Phase 1 | 部分已实现 |
| 3 | workspace 文件树、`@` 文件补全、git diff 面板；subagent 钻取视图；artifact 图片查看（`vim.ui.open` 拉流）；多 session 并存（每 session 一个 tab） | Phase 2 | 方向已定 |

非路线图：主题化（对齐 colorscheme 即可）、鼠标交互专属设计、移动端等。

## 9. 测试策略

- **lua 单元测试**（`specs/`，busted 风格，依赖 plenary.nvim 运行）：
  - `sse.lua` 帧解析：半包、多帧、`id/event/data`、注释行、心跳；Gap 检测、instance 变化；
  - `api.lua`： curl 参数构造、错误模型（401/409 typed error）映射；**空 body 必须编码为 `{}` 而非 `[]`**（`vim.json.encode({})` 陷阱，用 `vim.empty_dict()`）；
  - `blocks.lua`：各构造器的 lines/deco 快照断言；run_cmd JSON preview 拆分；截断逻辑；区间级 deco 的 byte 偏移；
  - `state.lua`：事件序列应用（text_delta → response_completed 校正、tool.* 配对、pending 卡片生命周期）；
  - `transcript.lua`：block 增删、run range 重渲染、follow 滚动、`clear_kind("welcome")`。
- **headless 冒烟**：`nvim --headless -S <script>` 驱动一轮完整事件流（已验证：欢迎页/spinner/tool JSON 拆分/审批卡/winbar/deco 结构）。注意：headless 下大量 `print` 到 TTY 会触发内置分页器挂起——**测试输出一律落盘读回**。
- **e2e 集成测试**（Go 侧，与 `e2e/harness` 同构）：起一个 go 实现的 fake SSE/REST server（或真实 `loom serve` + `internal/fakes` provider），`nvim --headless` 驱动：连接 → submit → streaming → approval → resync。Bazel `sh_test` 封装。
- 协议正确性以单测锁死：instance 变化 / `server.resync` 两条 resync 路径、稀疏序号不误报、chunk 跨断行完整重组，每次重刷后 transcript 与 snapshot 严格一致。

## 10. 开放问题

1. **Composer 多行 / 图片粘贴**：prompt buffer 天然单行提交（Enter 即提交）；多行需换键位或回到普通 buffer 方案；图片 via `POST /prompts {images:[artifact refs]}` 需要先有 artifact 上传通道。均不在本期。
2. **多 session 并存**（Phase 3）：骨架现在是单 session 状态机；需要 messages/blocks 按 session 隔离 + tab 宿主。期间如需并行，用多个 Neovim 实例。
3. **~~审批卡 diff 预览~~（已实现）**：`diffview.lua` 用 vim diffmode 在编辑窗口分 proposed/before scratch（审批+事后双入口），见 §6.6。下一个开放点是 hunk 粒度决议。
4. **serve 不在 Local**（UDS、远程）：`curl` 对 UDS 支持良好（`--unix-socket`），Phase 2 顺支持；远程（TLS 真鉴权）引用 `SERVE_DESIGN.md §1.1` 的遗留决策，不在本期。
5. **nvim 版本底线**：锁定 ≥ 0.10（vim.system）；如要向下兼容 0.9 需回退到 jobstart 逐块拼接 stdout 的 REST 封装，Phase 1 不投入。
