# BROWSER_DESIGN — loom 浏览器能力：GUI 打开通道与 agent 自主网页浏览

> 状态：**已实现**（`browser` 工具与 `run_cmd`/`exec_session` 的 `needs_gui_open` 通道已落地；原 Draft v3 待 review——v3 起移除 `open_in_browser` 工具，通道收敛为两条）
> 关联：PERMISSION_DESIGN.md（三层信任梯度、grant 机制、域名规则）、DESKTOP_DESIGN.md、SERVE_DESIGN.md
> 变更：v2 修复独立审查发现的 2 个 Blocker 与 8 个 Major；v3 按 review 决策砍掉 `open_in_browser`（原 M2），模型展示页面的诉求由 M1 的 `needs_gui_open` 通道覆盖。详见 §9 审查记录。

## 1. 背景与问题

两个真实痛点，对应两类不同的能力缺口：

1. **沙箱内 `open` 不可用（隐式调用）**。大量 CLI 在内部调用 `open` 打开 URL（`gh browse`、`npm docs`、dev server 的 "open in browser"、OAuth 回调引导等）。这些命令经 `run_cmd`/`exec_session` 在 Seatbelt 沙箱内执行，profile 是 `(deny default)` 且未放行任何 mach-lookup / appleevent，`open` 必然失败。实测（macOS，2026-08-10，对照实验见 §4.1）：基线 profile 下 `open -g https://example.com` 报 `kLSExecutableIncorrectFormat`。当前的绕行是 `require_escalated` 整条命令出沙箱（R3 逐次审批），爆炸半径远大于"打开一个 URL"。
2. **agent 无法"看"网页（显式浏览）**。`web_fetch` 只拿静态 HTML，`web_search` 只做检索；对 JS 渲染的页面、需要感知页面样式/布局的场景（前端联调、"看看这个页面样式对不对"、对本地 dev server 做视觉验证）完全失明。agent 需要一条能截图（视觉）+ 结构快照（可操作）的浏览通道。

两个问题的共同点是：**浏览器是用户机器上最重要的外部能力之一，而 loom 目前对它既无展示通道也无感知通道**。

## 2. 设计目标 / 非目标

### 2.1 目标

- **G1 两条通道各管一段，语义互补不重叠**：
  - 沙箱 GUI 开口：命令**隐式**内部调用 `open` 时能工作（compat）；模型想**显式**给用户展示一个页面时，也走这条通道（`run_cmd open <url>` + `needs_gui_open`），不单设展示工具；
  - `browser` 工具：agent **自主**浏览网页（automation），双通道感知（截图 + 结构快照）。
- **G2 复用权限体系的机制与语义**：能力授予走 `ExecGrant`/`RuleGrant` 扩展（与 `NetworkFull`/`WritablePaths` 同构），URL 门控复用域名规则语义（PERMISSION_DESIGN M3-lite）。**（v2 修正）**"复用"不是零改动：域名评估目前按工具名 `web_fetch` 硬编码分发，需要一次小重构把 URL 评估泛化（§5.3），如实计入工作量。
- **G3 能力最小化**：GUI 开口只在命令声明或规则授予时开启，不进默认 profile；browser 工具用临时 profile，与用户浏览器登录态完全隔离；browser 的 URL 入口强制 http/https（§5.2）。
- **G4 平台现实**：GUI 开口仅 darwin 落地（`open` 是 macOS 机制）；browser 工具跨平台（依赖外部 Chrome）；Linux 沙箱维持 fail-closed 现状。

### 2.2 非目标

- **不单设"打开 URL 给用户看"的内置工具**（v3 决策）：`open_in_browser` 类工具的展示诉求由 M1 的声明式开口覆盖（模型调 `run_cmd` 执行 `open`，带 `needs_gui_open` 声明），少维护一个工具面。
- 不内置/下发 Chromium 二进制（bazel external 体积与缓存成本），M3 依赖系统 Chrome 探测；自带的可行性留给后续评估。
- browser 工具初版**不含** `evaluate`（页面上下文执行任意 JS），不含多 tab 管理，不含文件下载/上传。
- 不改动 `web_fetch`/`web_search` 的既有语义；readability 正文提取不属于本设计（若只做内容获取那是更轻的路径，本设计解决的是"感知与操作"）。
- Linux/Windows 的 GUI 打开（`xdg-open` 等）只保留接口形状，不承诺实现。

## 3. 核心概念：两条通道的分工

| 通道 | 触发方 | 执行位置 | 感知能力 | 权限语义 |
|---|---|---|---|---|
| 沙箱 GUI 开口（M1） | 命令内部隐式调用 `open`；或模型显式 `run_cmd open <url>` + `needs_gui_open` | 沙箱内，profile 按需放行 | 无——内容对 agent 不可见 | grant `gui_open`（L1），声明式，任何模式都需审批（§4.2） |
| `browser`（M3） | 模型显式调用 | 主进程管理 headless Chrome 子进程（不经 Runner） | 截图（视觉）+ AX 快照（结构） | 按 action 分级风险 + 域名规则（导航入口，§5.2/§5.3） |

分工即语义边界：M1 驱动**用户的**浏览器（展示/兼容，agent 看不到内容），`browser` 是 **agent 的**浏览器（操作与感知）。工具 description 与 run_cmd 引导语各自强化这一区分。

## 4. M1：沙箱 GUI 开口（darwin）

### 4.1 实测规则集（2026-08-10 对照实验）

在 loom 现行 profile 基础上逐步放宽，`open -g https://example.com` 的表现：

| 实验 | 增量规则 | 结果 |
|---|---|---|
| 基线 | 无 | ❌ `kLSExecutableIncorrectFormat`（LaunchServices 查询不到 URL 绑定） |
| +LS 三件套 | `mach-lookup`：`com.apple.coreservices.launchservicesd`、`com.apple.lsd.mapdb`、`com.apple.lsd.modifydb` | ❌ `procNotFound`（能解析目标应用，Apple Event 发不出） |
| +AE 两件套 | 再加 `mach-lookup`：`com.apple.coreservices.appleevents` + `(allow appleevent-send)` | ✅ 成功打开 |
| 收窄验证 | 去掉 `com.apple.dock.server` | ✅ 不需要 |

**最小规则集 5 条**：

```
(allow mach-lookup (global-name "com.apple.coreservices.launchservicesd"))
(allow mach-lookup (global-name "com.apple.lsd.mapdb"))
(allow mach-lookup (global-name "com.apple.lsd.modifydb"))
(allow mach-lookup (global-name "com.apple.coreservices.appleevents"))
(allow appleevent-send)
```

### 4.2 授予模型：声明式，但比 needs_network 更保守

不默认进 profile（`appleevent-send` 的攻击面见 §6.1），走既有能力授予链路，**但在审批模式语义上刻意与 `needs_network` 不同**（v2 修正，审查 M5）：

- **`ExecGrant` 扩展**：domain 层 `ExecGrant` 新增 `GUIOpen bool`；permission 层 `RuleGrant`（`rules.go`）新增 `gui_open`。**（v2 修正命名，审查 M12：permission 层实际类型名是 `RuleGrant`，代码库中无 `CapabilityGrant`。）**
- **规则 schema**：`grant` 新增 `"gui_open": true`，校验/层级约束与 `network`/`write` 完全一致（仅用户层可携带；项目层剥离；builtin 层视为构建期 bug）。
- **run_cmd 参数**：新增 `needs_gui_open: bool`（可选）。模型在沙箱内 `open` 失败重试时**声明能力**而非直接提权；模型主动给用户展示页面时同样走此通道（`run_cmd` + `open` + `needs_gui_open`）。
- **审批模式语义（关键差异）**：`needs_network` 在 danger-only/never 模式下静默放网，其理由是"凭证路径仍不可读、无 exfiltration 增量"；该论证对 `appleevent-send` **不成立**（§6.1 的 TCC 归因分析）。因此 `gui_open` 声明在 **on-request 下产生 ask**（never 模式 deny 并附绕行指引；danger-only 按声明放行——该模式只拦明确危险操作），用户层规则文件是其余模式的免审批途径。模型声明本身绝不直接换到能力，§6.3 的"模型无法自授"不变量由此在所有模式下成立。
- **exec_session 同步覆盖（v2 新增，审查 M7）**：dev server（vite/webpack/react-scripts 启动时自动 `open`）是 M1 最有价值的场景，而它跑在 `exec_session` 里——`exec_session` 的 schema 目前连 `needs_network` 都没有。M1 范围显式包含给 exec_session 增加 `needs_gui_open` 参数（其 grant 映射点已有先例，改动小），否则长驻进程场景只能靠用户手写规则文件。
- **执行层**：`SeatbeltSandbox` 新增 `allowGUIOpen` 选项，`widenSandbox` 克隆时并入；`grant.GUIOpen` 为真时 profile 追加 §4.1 五条规则。Linux：`widenSandbox` 对非 Seatbelt 沙箱本就走"加宽为空操作"路径，fail-closed 保持。
- **失败引导**：`run_cmd` 的沙箱失败 Note 增加 GUI 签名识别——stderr 含 `_LS`/`NSOSStatusErrorDomain`/`AppleEvent` 字样时提示"GUI 打开类失败 → `needs_gui_open` 重试"。**（v2 注意，审查 N14）**新签名必须排在既有 `"operation not permitted"` 网络签名**之前**，否则 AE 发送被拒的 `errAEEventNotPermitted` 会被误分类为 network 引导。

### 4.3 grant 管道完整 touch-point 清单（v2 新增，审查 M8a）

`GUIOpen` 字段不是加进 struct 就完事——grant 的联合/比较/摘要点散落各处，漏掉任何一处 union 点，组合命令的 gui_open 就会被静默丢弃。实现 checklist：

- `ExecGrant.IsZero` / `ExecGrant.Summary`（domain/permission.go——审批 UI 的 grant 展示消费 Summary）
- `RuleGrant` 加载校验（rules.go：仅用户层、与 unsandboxed 互斥等）
- `execGrantsEqual`（规则匹配相等性）、`MatchAll` 的 grant 并集、shell 脚本逐子命令 grant union（decider.go）
- `AllowGrantCovers`（声明能力 vs 规则 grant 的覆盖判定）
- `DeriveRememberGrant`（"allow always" 记忆的 grant 推导，rule_approver.go）
- `widenSandbox` + `SeatbeltSandbox.profile`（执行层开口）
- **（v2 新增，审查 M9）DangerDecider 的 ask 盖章**：现有实现对命中危险清单的调用只在 `Escalated` 时盖 `Unsandboxed` grant——`needs_network` 命令被危险屏拦下、用户批准 ask 后仍在无网络的沙箱里跑，形成"批了但没给能力"的失败循环。这是既有缝隙，M1 顺手修复：DangerDecider 对声明能力（network/gui_open）统一盖章，needs_gui_open 不同构复制这个 bug。

### 4.4 已知边界（明示）

- **TCC 自动化授权**：沙箱内进程首次向某应用发 Apple Event，macOS 会弹"允许 X 控制 Y"系统授权框，这是系统行为，无法绕过也不应绕过；用户拒绝后 `open` 报错原样回传。**（v2 补充，审查 M5）**TCC 按 responsible process 归因：沙箱子进程的 Apple Event 大概率归到 **loom 本体**——用户给 loom 授权一次"控制 Safari"后，后续拿到 gui_open 的命令驱动该 App 不再有系统级提示。这正是 §4.2 要求所有模式都审批的原因：loom 层面的审批成为 TCC 之后的第二道闸。
- **global-name 是私有接口**：四个 mach 服务名无 ABI 承诺，macOS 大版本升级可能改名/拆分。失败兜底链不变（提示 escalate），并在 `runner_seatbelt_test.go` 增加实测用例（`open` 一个 URL 探测，失败即报），升级 macOS 后跑测试即可发现。
- **不做自动检测**：M1 不在 `shell_analyze` 里自动识别"命令会调 open"（第三方 CLI 把 open 藏在二进制内部，静态分析看不到）。声明式 + 失败引导已闭环；自动检测列为 M4 评估项。

## 5. M3：`browser` 工具（headless Chrome）

### 5.0 前置决策：为什么自研而不是挂 playwright-mcp（v2 新增，审查意见）

loom 已有完整的 MCP 管理面，playwright-mcp 现成提供 browser 能力，且 §8 把它列为 ref 交互范式的参照实现——必须正面回答为什么不直接挂它。自研的增量价值在三点：

1. **域名级门控**：MCP 工具的审批粒度是工具名整体（allow 一次 `browser_navigate` 即全网放行）；自研能把导航入口接进域名规则集，与 `web_fetch` 同一审批/记忆语义。这是安全模型的实质差异，不是工程洁癖。
2. **artifact/inline 截图集成**：截图走 `ArtifactStore` + 内联图像部分（§5.4），用户可见、模型可复审；MCP 通道回传图像无此约定。
3. **生命周期与回收可控**：浏览器进程、临时 profile、超时治理在自有代码内闭环（§5.5），MCP server 的进程管理不受 loom 控制。

代价是 snapshot ref 交互层需要自研（chromedp 只给裸 CDP Accessibility API，playwright-mcp 该层是数千行）——M3 因此拆成两个子里程碑（§7）。**若评审认为域名门控价值不足以抵回自研成本，M3 整体可降级为"文档化推荐 playwright-mcp MCP 配置"，本设计其余部分不受影响。**

### 5.1 选型

**chromedp**（纯 Go、无 cgo、CDP 直连、bazel/gazelle 友好）+ **系统 Chrome 探测**。探测序列：darwin `/Applications/Google Chrome.app` → Chromium → Edge；linux `google-chrome` → `chromium` → `chromium-browser`。找不到时工具返回明确指引（"未检测到 Chrome，安装后重试"），不静默降级。playwright-go 需 node 驱动，rod 与 chromedp 等价但社区生态略薄——chromedp 胜出。

**（v3 修正，2026-08）CDP 客户端迁移 chromedp → go-rod**：chromedp 的 context 即执行器模型（首次 `Run` 的 ctx 绑定进程生死、超时需手工包裹）被证明是持续维护负担；go-rod 的显式 `Browser`/`Page` 对象、链式 `Timeout`、`launcher` 内建浏览器探测与 leakless 进程回收在同等 CDP 能力下显著降低生命周期代码复杂度。对外行为（工具 schema、ref 交互、stealth 硬化、远程 `cdp_url` 模式）不变，e2e 套件原样通过。

浏览器**不进 Runner/Seatbelt**：Chrome 的 helper 进程群（GPU/renderer/network service）在 `(deny default)` 下无法启动，且 CDP 本身需要 loopback WebSocket。由主进程直接管理子进程——与 M1 的 Runner 路径无关。启动参数：`--headless=new`、`--disable-gpu`、`--user-data-dir=<os.MkdirTemp>`（**临时 profile，用户 cookie/登录态天然隔离**）、`--no-first-run`、窗口 1280×800（可配）。

### 5.2 工具形态

单工具 `browser`，`action` 枚举驱动：

| action | 参数 | 风险级 | 返回 | 说明 |
|---|---|---|---|---|
| `navigate` | `url` | **R3** | 最终 URL（含 redirect 后 host）、标题、状态码 | http/https 强制 + host 过域名规则 |
| `snapshot` | — | R2 | AX 树文本序列化，交互元素带 `ref` 编号 | token 友好的结构视图，截断策略见 §5.4 |
| `screenshot` | `full_page?` | R2 | artifact ref + inline 图像（复用 `maxInlineImageBytes` 约定） | 视觉/样式感知主通道 |
| `click` | `ref` | R2 | 新页面摘要 | ref 来自最近一次 snapshot |
| `type` | `ref`, `text`, `submit?` | **R3** | 新页面摘要 | 向页面注入数据，与 navigate 同级 |
| `scroll` | `direction`, `amount?` | R2 | 新页面摘要 | |
| `close` | — | R2 | — | 显式回收；idle 超时自动回收（§5.5） |

设计决策：

- **（v2 新增，审查 M4）按 action 分级风险**：`Prepare` 阶段按 action 覆盖 risk，先例现成（`run_cmd` 的 `riskForArgs` 对 escalated 调用提级）。不分级的后果：browser 若为 R3，`click`/`scroll`/`snapshot` 每次调用都在 on-request 模式弹审批；域名记忆只覆盖 navigate，工具级 "allow always" 又不满足 tool-name 记忆的固定爆炸半径门槛——审批风暴会让工具不可用。分级后：读操作（snapshot/screenshot/scroll/click）R2 免审批，写/导航操作（navigate/type）R3 走域名规则与审批。
- **URL/scheme 门控（v2 修正，审查 B1，安全红线）**：`navigate` 强制 http/https 白名单。`file://`、`chrome://`、`data:`、`view-source:` 一律拒绝——headless Chrome 以用户身份运行，`navigate("file:///Users/x/.aws/credentials")` + `snapshot` 会把沙箱 `sensitiveReadDenies` 拼命守护的凭证文件直接读进模型上下文，这是整个权限体系的旁路。本地静态页面预览需求（`file://` workspace 内页面）如需支持，列为独立能力后续评估，不进初版。
- **交互目标是 snapshot 分配的 `ref` 序号，不是 CSS selector**（playwright-mcp 风格）：模型写 selector 脆弱且易注入，ref 绑定"最近一次快照看到的页面"，陈旧 ref（页面已变）直接报错要求重新 snapshot。
- **`evaluate` 不进初版**：在页面上下文执行模型生成的 JS 等价于把页面数据/同源请求能力完全交给模型，风险收益不成比例；列入 M4 评估。
- **`ConcurrentSafe() = false`**：单个 browser 实例有状态；跨 session 并发的处理见 §5.5。
- **注册策略**：主 agent 独占（子代理注册表不含，同 `generate_image`）；配置节 `browser.enabled`（默认 false——引入外部进程依赖，opt-in；未启用不注册，模型不可见）。**（v2 补充，审查 M11）**配置热重载按 image 节先例是 restart 级，`classifyConfigChanges` 需新增 browser 节条目，否则热改配置静默不生效。

### 5.3 域名评估泛化重构（v2 新增，审查 B2；v3 起归入 M3a）

v1 假设"URL 门控零新增机制"，审查证伪：域名评估目前按**硬编码工具名 `web_fetch` + 顶层 `{"url":...}` 参数形状**分发——RuleDecider/SessionDecider/BaselineDecider 的 web_fetch 特例、`RuleApprover.RememberCall`/`ApprovalRulePreview`、`ParseWebFetchHost` 只解顶层 `url`。`browser` 的参数是 `{"action":"navigate","url":...}`，现状下根本不会进入域名评估。

修法（仿 `ExecRequest` 的 M17 先例——该注释正是为解决工具名耦合）：

- `domain.PreparedCall` 增加 typed **`URLRequest{Host string}`** 字段，由工具在 `Prepare` 期填充并纳入签名覆盖（与 Grant 刻意留在签名外相反——URL 是模型输入，必须防篡改）。
- web_fetch（顶层 `url`）与 browser（`action=navigate` 时的 `url`）两个工具各自填充；decider 链与 approver 改判 `URLRequest` 字段而非工具名 + ad-hoc 参数解析。
- 无 `URLRequest` 的调用行为逐字节不变（G5 兼容）。

这是一次小重构（permission + app 审批层 + 两处工具），工作量计入 M3a。

### 5.4 输出预算与降级

- snapshot：AX 树序列化后按 rune 上限截断（初版 8K runes，超出部分省略并标注"已截断，用 scroll/缩小范围"）；巨型页面退化为可视视口部分。
- screenshot：PNG 超 `maxInlineImageBytes`（3.5MB）时只落 artifact 不内联（与 `generate_image` 同约定）。
- 每个 action 独立超时（navigate 30s 默认，其余 10s），超时返回部分状态 + 明确错误，不留悬挂 goroutine。
- 无头渲染差异：少数站点对 headless 有反爬差异（返回不同 DOM），如实回传，不伪装 UA（初版）；反爬对抗非目标。

### 5.5 生命周期与回收（v2 重写，审查 M6/M10）

v1 假设"per session 懒启动、session 关闭 kill"，审查证伪：loom session 没有 close 事件（`DeleteSession` 只删持久化数据，TUI 里 session 只是被切换/搁置），且 serve 模式下同一 workspace 的多个 session 共享同一工具实例。实际机制：

- **实例键控**：browser 实例按 `LOOM_SESSION_ID`（turn ctx 已注入，工具可经 `SessionEnvFromContext` 读取）键控管理；每实例一把互斥锁，跨 session 并发驱动不同实例、同 session 内串行。`ConcurrentSafe()=false` 管 turn 内批处理，实例锁管跨 session。
- **回收**：仿 `exsession.Manager` 的 idle-TTL reaper 模式——browser 实例 30 分钟无活动自动 kill + 删临时 profile；workspace 级 `Bootstrap.Close` 兜底全量回收。
- **（v2 修正，审查 M10）"父亡子随"修正**：macOS 无 `PR_SET_PDEATHSIG`，loom 被 SIGKILL 时 chromedp 的 cancel 链不生效，Chrome 孤儿 + 临时 profile 残留。缓解：临时目录名带 loom 标记 + 时间戳，browser 工具初始化时清扫自己创建的陈旧目录（进程已死的）；§6.3 不变量表相应改为"孤儿有界回收"，不承诺强父亡子随。**（v3 补充）**迁移 go-rod 后，其 launcher 的 leakless 守卫进程在父进程 SIGKILL 时也能回收 Chrome，该问题在本地启动模式下结构性消除；远程 `cdp_url` 模式不涉及本机进程。

## 6. 安全分析

### 6.1 M1 GUI 开口的攻击面（明示）

`(allow appleevent-send)` 授予的不只是"打开 URL"：沙箱内进程可向**任意正在运行的应用**发送 Apple Event（如 `osascript -e 'tell app "Safari" to ...'`）。缓解现实：macOS 对自动化事件有 TCC 逐应用授权（首次必弹窗）；浏览器的 `do JavaScript` 默认关闭。**TCC 归因（v2 补充）**：事件按 responsible process 大概率归到 loom 本体——用户授权一次"loom 控制 Safari"后，系统层面不再逐次提示，因此 loom 自己的审批（§4.2 要求所有模式 ask）是必须保留的第二道闸，不能像 needs_network 那样静默放行。残余风险接受依据：grant 仅来自用户规则文件或逐次交互批准，模型声明只触发审批、不直接获得能力。

### 6.2 browser 工具的威胁模型（明示）

1. **提示注入（最主要）**：网页内容是不可信输入，snapshot/截图进入上下文后可能含针对模型的注入指令。这是 Lethal Trifecta 的经典构型（不可信内容 + 工具能力）。缓解：域名规则约束入口；工具输出 header 固定标注"以下为不可信网页内容"；`evaluate` 缺席 + http/https 白名单收窄了页面→宿主的能力面。无银弹，明示。
2. **redirect / 点击逃逸**：入口域名 allow 后，服务端 redirect 或模型 `click` 页面上任意外链（v2 补充，审查 M13：click 驱动的跨域导航与 redirect 效果等同，且完全在"已批准页面"语义内）都会把外站内容引入上下文。缓解：navigate/click 后输出标注最终 host 供模型/用户察觉；完全收敛需本地代理（PERMISSION_DESIGN M3），不在本期。
3. **localhost 浏览**：访问 loopback dev server 是核心场景，予以放行；副作用是恶意页面可经 browser 探测内网/本机服务（CSRF 风格）——临时 profile 无 cookie，爆炸半径限于无鉴权服务，接受并明示。
4. **凭证隔离**：临时 profile 保证用户登录态永不进 agent 视野；这是与"驱动用户日常浏览器"方案的本质安全差异。

### 6.3 不变量汇总

| 不变量 | 机制 |
|---|---|
| 模型无法自授 GUI/浏览能力 | gui_open 所有模式均审批（§4.2）；规则仅用户层；`browser.enabled` 是部署配置 |
| 沙箱凭证防护不被 browser 旁路 | navigate 强制 http/https，file:// 拒绝（§5.2） |
| 用户浏览器登录态不进 agent | browser 工具独立临时 profile；M1 通道只展示不读回 |
| 域名审批语义单一来源 | 两条 URL 通道共用 `URLRequest` + 域名规则集 + remembered.json domains 节（§5.3） |
| 浏览器进程回收有界 | idle-TTL reaper + `Bootstrap.Close` + 陈旧临时目录清扫（§5.5；macOS 无强父亡子随，不承诺） |

## 7. 里程碑

### M1（darwin）
Seatbelt GUI 开口：`ExecGrant.GUIOpen` + `RuleGrant.gui_open` + run_cmd/exec_session `needs_gui_open` + profile 五规则 + 失败引导 Note（签名排序）+ §4.3 全部 touch-point + DangerDecider 能力盖章修复 + seatbelt 实测用例。

### M3a
`browser` 工具最小闭环：chromedp + 系统 Chrome 探测、`browser.enabled` 配置（含热重载分类）、生命周期管理（§5.5）、**URL 评估泛化重构**（`URLRequest`，§5.3，web_fetch 同步迁移）、`navigate`（scheme 白名单 + 域名门控）+ `screenshot`（artifact/inline）+ `scroll` + `close`。此里程碑已具备"视觉感知"核心价值。

### M3b
`snapshot`（AX 树序列化 + ref 分配 + 陈旧检测）+ `click`/`type` ref 交互。工作量主体（自研 ref 交互层），独立交付。

### M4（评估项）
`evaluate` action；多 tab；`file://` workspace 内预览能力；自动检测命令内 open 调用；自带 Chromium 分发；Linux `xdg-open` 落地；反爬差异应对。

## 8. 参照实现索引

| 主题 | 位置 |
|---|---|
| seatbelt GUI 规则实测记录 | 本文 §4.1（2026-08-10 对照实验） |
| grant/widen 机制 | PERMISSION_DESIGN.md §3.2/§6.3；`internal/process/sandbox_darwin.go` |
| 域名规则 | PERMISSION_DESIGN.md M3-lite；`internal/permission/domain_rules.go` |
| 图像内联/artifact 约定 | `internal/tool/imagegen/generate_image.go`（`maxInlineImageBytes`、header+part 布局） |
| 按参数分级 risk 先例 | `internal/tool/command/run_cmd.go`（`riskForArgs`） |
| idle-TTL 回收先例 | `internal/tool/exsession/manager.go` |
| snapshot ref 交互范式 | playwright-mcp（Microsoft playwright MCP server） |
| CDP Go 客户端 | github.com/go-rod/rod（v3 起，原 chromedp） |

## 9. 审查记录

v1 草案经独立审查（对照代码逐项核验），结论"骨架正确、不可原样实施"。v2 合入：

- **Blocker**：B1 navigate scheme 白名单（file:// 旁路沙箱凭证防护）→ §5.2；B2 域名规则复用假设证伪（web_fetch 硬编码分发）→ §5.3 URLRequest 重构 + G2 措辞修正。
- **Major**：M3 展示工具 R2→R3（审批 UX 矛盾）→ v2 §5（v3 已随工具移除）；M4 per-action 风险分级 → §5.2；M5 gui_open 打破 needs_network 同构（所有模式审批 + TCC 归因）→ §4.2/§6.1；M6 session 生命周期虚构 → §5.5 重写；M7 exec_session 缺口 → §4.2；M8 工作量重估（touch-point 清单 + M3 拆分）→ §4.3/§7。
- **Minor**：M9 DangerDecider 盖章缝隙 → §4.3；M10 父亡子随过强 → §5.5/§6.3；M11 热重载分类 → §5.2；M12 RuleGrant 命名 → §4.2；M13 click 逃逸 → §6.2；N14 失败签名排序 → §4.2。
- **设计批评**：playwright-mcp 替代方案正面论证 → §5.0。

v3（review 决策）：移除 `open_in_browser` 工具（原 M2）——模型展示页面的诉求由 M1 的 `run_cmd open` + `needs_gui_open` 通道覆盖，通道收敛为两条；URLRequest 泛化重构随之归入 M3a（§5.3）；原 M2 的 R3 定级结论（审查 M3）不再适用，M1 通道的审批语义由 §4.2 独立规定。
