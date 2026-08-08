# Loom Desktop 设计（M4）

- 状态：Implemented v5（M4.0~M4.5 已落地。v5：macOS 平台集成与发布工程——版本号单源化（`internal/version`）、squircle 图标、TCC usage descriptions、单实例 GUI 提示、Finder 启动 workspace 选择、窗口几何持久化、Notification Center 镜像、release 四产物（macOS 双架构 DMG + Linux CLI deb）与 strip（-s -w），见 §3.5/§6.5。v4：R-B2 实锤——AssetServer 通道 ContentLength 恒为 -1 且 responseWriter 无 http.Flusher（SSE 不可能），前后端通道从「AssetServer 进程内挂载」改为「常驻 loopback 监听 + bootstrap 跳转」（原 §2.3 降级方案转正）；token 注入主通道随之回到 URL fragment。v3：token 注入定稿 meta 标签（已随通道更换退役）；R-B1 三项 Bazel 适配；.app 打包签名验证通过。v2：前后端通道更正为 AssetServer 挂载（后被 v4 取代）；分享复制点更正为 main.js）
- 日期：2026-08-08
- 前置文档：`SERVE_DESIGN.md`（§5 协议、§10 客户端契约）、`WEB_DESIGN.md`（§3.4 api.js、§7 静态托管与安全头）、`WORKSPACE_DESIGN.md`
- 范围：基于 Wails 的桌面应用前端（`loom-desktop`），与 TUI、WebUI 三种 UI 并存；内网监听与会话分享；Bazel 直接产出 macOS `.app`

---

## 1. 背景与目标

M1'~M3 交付了 `client.Client` 协议无关契约（SERVE_DESIGN §10：「每个前端都是对等客户端」）和两个一等前端：TUI（inproc 实现）与浏览器 SPA（http 实现）。M4 在此之上交付第四个前端形态——桌面应用，兑现契约设计的最终论证：**新前端不触碰 agent 核心，只复用现有组装与协议**。

### 1.1 目标

1. **三 UI 并存平权**：`loom`（TUI）/ `loom serve`（WebUI）/ `loom-desktop`（桌面）共享 `assembleRuntime` + `app.SessionService` + `runtimeevent.Broker`，行为完全一致；TUI/WebUI 代码路径零改动、零回归。
2. **前端零 fork**：桌面窗口渲染的就是现有 SPA（`internal/server/web/static`），REST+SSE 协议原样消费；SPA 全部请求为相对路径、SSE 为 fetch+ReadableStream 实现（`sse.js`），对 origin 无假设，可原样运行于 webview。
3. **CGO 隔离**：Wails 引入 CGO（macOS 链 WebKit），不得污染主 `loom` binary 的纯 Go 默认构建——桌面是独立 cmd、独立 Bazel target。
4. **内网可达**：支持绑定非 loopback 地址，使会话分享链接（`/share/{token}`，token 即能力的只读公开路由）可被局域网同事打开。
5. **Bazel 直接产出 `.app`**：不依赖 Wails CLI 的打包流程，`.app` bundle 由 Bazel 组装。

### 1.2 非目标

- Windows/Linux 桌面打包（MVP 仅 macOS；Linux 需 webkit2gtk 开发包、Windows 的 AssetServer 不支持流式响应（§9 R-B2），均列为已知路径但不进 CI）。
- Wails 原生绑定（`Bind` + `runtime.EventsEmit`）——留作后续演进（§8），MVP 不做。
- 应用分发工程（Developer ID 签名、公证、自动更新）——ad-hoc 签名为止。
- 多窗口、系统托盘、全局快捷键等桌面增强——MVP 之后按需迭代。

---

## 2. 技术选型

### 2.1 决策总览

| 维度 | 决策 | 备选（放弃原因） |
|---|---|---|
| 桌面框架 | **Wails v2**（作为纯库 `wails.Run` 使用，不引入 CLI/代码生成/`wails.json`；最新稳定 v2.13.x） | webview/webview_go（Wails 底层库，更轻但菜单/窗口管理/后续原生绑定能力弱，见 §2.2）；Electron/Tauri（引入 JS/Rust 构建链，违反仓库约束） |
| 前后端通道 | **常驻 loopback 监听 + bootstrap 跳转**：server 始终绑 `127.0.0.1:0`，webview 起始页是 AssetServer 托管的 meta-refresh 跳转页，落地后全部 REST+SSE 走真实 HTTP | AssetServer 进程内挂载（**实锤不成立**：ContentLength 恒 -1、无 http.Flusher、无 30x，见 §2.3 R-B2）；Wails 原生绑定（需重写 api.js/sse.js 桥接层，§8 再评估） |
| 进程形态 | **独立 cmd `loom-desktop`**（独立 main、独立 Bazel target） | `loom` 加 `desktop` 子命令（wails 的 CGO 依赖会进入 `loom_lib`，污染默认构建矩阵，不可接受） |
| 静态资源 | 沿用 `internal/server/web` 的 `embed.FS` + 强 ETag 托管（经 `srv.Handler()` 统一出口） | Wails Assets 另挂一份 `fs.FS`（双出口、缓存语义分裂，漂移风险） |
| 鉴权 | 进程内随机 token（32 字节，不落盘不打印），经 URL fragment 注入 SPA 跳过 gate | 持久化 token 文件（桌面进程是短期会话所有者，无重连场景，持久化只增加泄漏面） |
| 打包 | **Bazel `pkg_zip` 组装 `.app`** + ad-hoc `codesign` | `wails build` CLI（引入 Node 依赖与外部工具链；且其模板工程结构与 monorepo 不符） |

### 2.2 Wails v2 vs v3 vs webview_go

- **webview_go**：最小依赖（单一 cgo 库），1~2 天可出原型。但窗口菜单、关闭钩子、macOS About 面板等都需手写 cgo/objc，且未来若演进到原生绑定（§8）等于重新引入 Wails。仅作为「Wails 依赖在 Bazel 下长期无法排障」时的降级方案保留。
- **Wails v2**：稳定线（v2.13.x），`assetserver.Options.Handler` 支持任意 `http.Handler` 进程内挂载，窗口/菜单/生命周期钩子完整，文档与社区成熟。**选此**。
- **Wails v3**：架构更好（多窗口一等公民、新事件系统），截至本稿处于 **beta**。不作为 MVP 基座；GA 后可在不改动 §3 组装逻辑的前提下升级（仅 `cmd/loom-desktop` 内变更）。

### 2.3 关键 API 事实：AssetServer.Handler（v2 无外部 URL 加载）

Wails v2 的 webview 启动页固定由内置 AssetServer 提供，**没有**「加载任意 URL」的选项（v1 草稿假设的外部 URL 加载不成立）。但 `assetserver.Options` 提供通用 `http.Handler` 出口：

- `Assets` 为 nil 时，**所有** GET 请求及全部非 GET 请求都进入 `Handler`；
- 官方能力矩阵（v2.13 文档）：macOS 上 GET/POST/PUT/PATCH/DELETE、请求头、请求体、响应状态码/头/体均支持，**响应体流式（Response Body Streaming）macOS ✅**（SSE 的前提）；Windows ❌、Linux ✅；
- WebSockets 全平台 ❌——loom 协议用 SSE 而非 WebSocket，无影响。

而 `server.Server` 恰好导出了为内嵌准备的完整 handler：

```go
// Handler exposes the fully-middlewared HTTP handler — for embedding the
// adapter in-process (tests, future mounts) without a listener.
func (s *Server) Handler() http.Handler { return s.http.Handler }
```

因此桌面端可以不经过任何 TCP 栈：`AssetServer{Handler: srv.Handler()}`。SPA 的相对路径 fetch（`/v1/...`）在 wails origin（macOS 为 `wails://localhost`）下全部落到该 handler，认证（Bearer）、安全头（CSP）、静态托管（ETag）由 server 中间件原样提供。

**R-B2 实锤（实机验收记录）**：AssetServer 通道实测三项协议缺陷，均命中 loom 的硬需求——

1. **ContentLength 恒为 -1**（`assetserver_webview.go` 对未知长度 body 一律 -1）：`handleCreateSession` 曾按 `r.ContentLength > 0` 判定有无 body，导致 resume 请求被误判为 create——表现为「每次启动自动创建空会话 + 打开最近会话报 session not found」。服务端已修为按实际字节判定（chunked/HTTP2 客户端同样受益），另有桌面端绕开该通道（见下）。
2. **responseWriter 无 `http.Flusher`**：SSE handler 的 `w.(http.Flusher)` 断言失败，流式事件推送在原理上不可能——表现为「对话无流式反馈、SSE 无限 reconnecting」。
3. **不支持 30x 重定向**（官方矩阵标注）：bootstrap 跳转因此用 `<meta http-equiv="refresh">` 而非 302。

结论：**通道改为常驻 loopback 监听**（`127.0.0.1:0` 随机端口）**+ bootstrap 跳转页**——webview 起始页由 AssetServer 托管（仅一个 GET 跳转页，无任何协议依赖），跳转后全部 REST+SSE 走真实 net/http，Flusher/ContentLength/流式语义完整。原「进程内挂载零监听」的优势经实测不成立（挂载点恰恰丢掉了 HTTP 语义），loopback 的性能损耗亚毫秒级，可忽略。

### 2.4 为什么 loopback 方案成立

webview 到 `127.0.0.1` 进程内监听的往返是亚毫秒级，SSE 高频帧（`model.text_delta`）无可感知损耗；且这是与 `loom serve` 完全相同的 net/http 通道，协议语义无分叉。关键收益是**前端与 WebUI 完全同一份代码**：

- SPA 的所有组件（transcript/composer/审批卡片/问答卡片/diff 视图）零改动工作；
- `server_test.go` 与 `client` contract 测试继续覆盖桌面端依赖的全部协议行为——桌面端的协议正确性由现有测试套件背书；
- 「桌面独有 bug」被压缩到窗口生命周期、URL 注入、AssetServer 行为三处，排障边界清晰。

---

## 3. 架构

### 3.1 进程内结构

```
loom-desktop (单一进程)
├── main.go                    # 参数解析、组装、窗口生命周期
├── assembleRuntime(...)       # 与 loom serve 完全相同的组装（app.ProcessRuntime + WorkspaceRegistry + Bootstrap）
├── server.AcquireDataDirLock  # 数据目录 flock——与 serve/chat 互斥语义一致
├── app.SessionService         # 应用层（唯一）
├── server.Server (UI)         # 传输适配器：常驻 127.0.0.1:0 loopback 监听，webview 直连
├── server.Server (share)      # 可选第二监听：--listen 指定时绑内网地址（§5）
└── wails.App                  # webview 窗口：AssetServer 只服务一页 bootstrap，
                               # meta-refresh 跳转到 http://127.0.0.1:<port>/#token=<tok>
```

关键性质：**一个进程、一个 SessionService、一个 broker；一个常驻 loopback 监听**（UI 专用，真实 HTTP 语义），外加可选的内网分享监听。

### 3.2 与现有入口的关系

| 入口 | 组装 | 锁 | 监听 | 前端 |
|---|---|---|---|---|
| `loom`（TUI） | `assembleRuntime` | 否 | 无 | inproc client |
| `loom serve` | `assembleRuntime` | 数据目录 flock | 可配置（默认 127.0.0.1:7680） | 任意 http client |
| `loom-desktop` | `assembleRuntime` | 数据目录 flock | 常驻 127.0.0.1:0（UI）+ 可选 `--listen`（分享） | 内嵌 webview（loopback HTTP） |

桌面端与 `loom serve` 共享「数据目录 flock → assembleRuntime → broker → SessionService → server.New」序列；差异在最后：serve 阻塞等信号，desktop 额外把 webview 经 bootstrap 页指向 loopback 地址并进入 GUI 事件循环。

### 3.3 启动序列（`cmd/loom-desktop/main.go`）

```
1. 解析参数：--listen（默认空 = 仅进程内，无 TCP）、--advertise（可选，§5）
2. workspace 解析：终端启动用 cwd（与 loom 语义一致）；Finder 启动（cwd=/）
   弹原生目录选择框，取消即干净退出（v5，§3.5）
3. loadConfig(true) / prepareStorage / newFileLogger        ← 与 runServe 相同
4. AcquireDataDirLock(dataDir)                               ← 与 runServe 相同；冲突时弹原生
                                                              警告框再退出（v5，§3.5）
5. assembleRuntime(ctx, resolved, root, logger)              ← 原样复用
6. broker := runtimeevent.NewBroker(WithDurableQueue(4096))
   app.WireSubagentObserver(...)
   service := app.NewSessionService(...)
   go watchNotifications(ctx, broker, logger)                ← v5，§3.5：事件镜像到通知中心
7. token := 32 字节随机（crypto/rand，进程内存，不落盘不打印）
8. uiSrv := server.New(Config{Listen: "127.0.0.1:0", Token: token, ...})
   uiSrv.Listen(); go uiSrv.Serve()                            ← UI 常驻 loopback
   if --listen 非空 {                                          ← 可选内网分享（第二监听）
       shareSrv := server.New(Config{Listen, PublicBaseURL: derive(...), ...})
       shareSrv.Listen(); go shareSrv.Serve()
   }
9. wails.Run(&options.App{Title: "Loom", Width/Height ← 持久化窗口几何（v5，§3.5）,
       AssetServer: &assetserver.Options{
           Handler: bootstrapHandler(uiBase + "/#token=" + token)},  ← §4.2
       Mac: &mac.Options{...},
       OnStartup: 恢复窗口位置 + 启动几何持久化轮询,
       OnShutdown: 优雅退出序列（§3.4）})
```

注（v4 定稿）：Wails v2 的启动 URL 不可配，且 AssetServer 通道丢失关键 HTTP 语义（§2.3 R-B2）。因此 webview 只从 AssetServer 拿一页 `<meta http-equiv="refresh">` bootstrap，立即跳转到常驻 loopback 地址，token 经 fragment 携带（不进请求行/日志）；落地后 SPA 与浏览器版行为完全一致。

### 3.4 优雅退出

`OnShutdown` 回调复用 `runServe` 的退出序列，不新设计：

```
srv.Shutdown(60s)      # 若起过 TCP 监听：停止接受 HTTP、SSE 发出 server.draining 并尽快返回
service.Shutdown(60s)  # drain：进行中的 turn 跑完或被取消
broker.Close()
lock.Release()
```

同时保留 `signal.NotifyContext`（SIGINT/SIGTERM 走同一路径），保证 `kill` 与点关闭按钮语义一致。`srv.Shutdown` 在未调用 `Listen` 时退化为标记 draining + 关闭空闲连接，安全幂等（实现时验证该路径，必要时在 desktop 侧跳过该调用）。

**wails 关闭时序（v5 排障记录）**：wails v2 的 `App.Run` 顺序是 `frontend.Run → RunMainLoop → WindowClose() → shutdownCallback`——`OnShutdown` 执行时原生窗口上下文已 `ReleaseContext` 释放，在其中调用 `WindowGetSize/Position` 是 use-after-free。因此窗口几何不在退出时采集，改为运行期轮询落盘（§3.5）。

### 3.5 macOS 平台集成（v5）

四项集成都经 AppleScript（osascript）而非直接 Cocoa 调用：这些弹窗运行在 wails GUI 循环启动之前/之外，NSAlert/NSOpenPanel 有主线程 + runloop 前置条件，AppleScript 无此约束，且 Finder/终端启动行为一致。

| 能力 | 实现 | 要点 |
|---|---|---|
| 单实例提示 | flock 冲突时 `display dialog` 警告（`dialogs_darwin.go`） | Finder 启动的第二实例无可见 stderr，弹窗替代静默退出；osascript 失败按 stderr 是否含 "User canceled" 区分「用户取消」与「基础设施失败」（后者回退 home 目录，SSH 无 window server 场景不误判） |
| workspace 选择 | Finder 启动（cwd=`/`）时 `choose folder` | 取消 = 干净退出；不再静默把 `$HOME` 当 workspace（agent 工具的文件操作范围过大）；终端启动行为不变 |
| 窗口几何持久化 | `windowstate.go`：运行期每 2s 轮询、变更即写 `desktop-window.json`；启动时恢复尺寸与位置 | 位置校验当前屏幕可达性（拔掉外接屏不会在屏幕外复活）；wails 坐标是**当前屏 visibleFrame 相对值**（左上原点），跨屏移动无需特判 |
| 通知中心镜像 | `notifications.go` 订阅 broker：`approval.requested`/`question.asked`/`turn.finished/failed` → `display notification` | v1 不做前台检测（`NSApp.isActive` 只能主线程安全读）；通知频率低（每 turn/审批一条），误报成本为一条自动消失的横幅 |

---

## 4. 鉴权与 token 注入

### 4.1 约束继承

协议安全模型（SERVE_DESIGN §5.2/§6）不变：token 只走 `Authorization: Bearer` header；`/v1/*` 全部鉴权；静态资源与 `/v1/shared/*`、`/share/{token}` 公开。桌面端新增的问题只有一个：**进程内随机 token 用户无从知晓，SPA 的 token gate 必须被程序化跳过**。

### 4.2 token 注入（SPA 唯一改动；实现定稿为 meta 标签）

实现定稿（v4）：桌面壳的 bootstrap 页把 token 编进跳转目标 `http://127.0.0.1:<port>/#token=<hex>`；`main.js` 启动处按 **meta 标签 → URL fragment** 的优先级读取并在 gate 运行前落入 `sessionStorage`（约 10 行；meta 通道为 v3 遗产，保留兼容）：

```javascript
// Desktop shell bootstrap: meta tag (AssetServer-injected) or URL fragment.
const embeddedToken =
  (document.querySelector('meta[name="loom-token"]')?.content || "") ||
  (new URLSearchParams(location.hash.slice(1)).get("token") || "");
if (embeddedToken) {
  sessionStorage.setItem(TOKEN_KEY, embeddedToken);
  if (location.hash) history.replaceState(null, "", location.pathname);
}
```

安全性质：fragment 不进入 HTTP 请求行（R4 泄漏面），读出后立即 `history.replaceState` 抹除；bootstrap 页只存在于进程内 AssetServer 通道，网络侧不可达。token 落 `sessionStorage` 后走现有全部路径（`api.js` 的 Bearer header、`sse.js` 的 fetch SSE），无第二条认证链路。浏览器模式无 meta/fragment，行为完全不变。桌面端发生 401 时不再展示 token 输入框（进程内 token 无凭证可贴），改为提示重启应用。

### 4.3 威胁模型核对

| 场景 | 分析 |
|---|---|
| 本机其他进程读取 token | 默认模式下**没有任何 TCP 监听**，token 仅存桌面进程内存与 webview 页面上下文（sessionStorage 按 wails origin 隔离）；攻击面小于现有 serve token 文件（0600 落盘） |
| 内网嗅探 | 默认模式流量不出进程；`--listen` 开启后见 §5.3——与 `loom serve --listen 0.0.0.0` 的威胁模型完全一致，非新增 |
| webview 打开外部链接 | 沿用 CSP `default-src 'self'` + `connect-src 'self'`；wails 默认将外部协议导航交给系统浏览器 |

---

## 5. 内网监听与会话分享

### 5.1 分享链路现状（无需改动部分）

`/share/{token}` 页面与 `/v1/shared/*` 只读接口是「token 即能力」的公开路由（`server.go` routes 注释），创建/撤销仍属 owner 操作（bearer-gated）。该设计对内网共享天然成立：同事浏览器打开链接即可读，无登录、无依赖。

### 5.2 缺口与方案

**缺口 A：桌面默认无 TCP 监听，分享链接出不了本机。**

`--listen` 参数（语义对齐 `loom serve --listen`）显式开启内网监听。**与 v1 草稿不同**：webview 自身不再依赖该监听（进程内挂载），因此绑定地址的选择不影响桌面 UI，推导逻辑大幅简化：

| `--listen` 值 | TCP 监听 | 分享可达性 |
|---|---|---|
| 空（默认） | 无 | 仅本机（无对外地址，不生成 PublicBaseURL） |
| `0.0.0.0:7680` | 全接口 | 内网经 `http://<lan-ip>:7680` |
| `192.168.1.5:7680`（具体 IP） | 指定接口 | 内网经同一地址 |

**缺口 B：SPA 复制的分享链接 host 是页面自身 origin。**

复制点在 `main.js`（非 v1 草稿所写的 share.js——share.js 是分享**消费侧**页面）：

```javascript
const { path } = await app.api.shareSession(app.sessionId);
const url = location.origin + path;
```

webview 中 origin 是 `wails://localhost`，拼出的链接既不可达也不是合法 HTTP URL。引入「对外地址」`PublicBaseURL`（服务端增量，WebUI 远程场景同样受益）：

| 文件 | 改动 |
|---|---|
| `internal/server/server.go` | `Config` 新增 `PublicBaseURL string`（可选，如 `http://192.168.1.5:7680`） |
| `internal/server/handlers_share.go` | `handleShareSession` 响应在 `PublicBaseURL` 非空时附带绝对 `url` 字段（`{token, path, url}`） |
| `internal/server/web/static/js/main.js` | 复制处优先用响应的 `url`，缺省退回 `location.origin + path`（现行为） |
| `cmd/loom-desktop/main.go` | `PublicBaseURL` 推导：`--advertise` 显式值优先；`--listen` 为 unspecified 时自动探测出口网卡 IP 拼端口；未开 `--listen` 时为空（分享仅本机，不误导） |

否决的备选：API 进程内 + 第二 TCP 监听只挂分享路由。`server.Server` 的 mux 是 API/分享/SPA 一体注册，拆监听要动路由结构，侵入大而收益小；单监听 + bearer 鉴权已满足威胁模型。

### 5.3 安全提示（沿用现有设计，无新增风险面）

- 内网监听后 REST API 仍受 bearer token 保护；桌面 token 为进程内 32 字节随机值，不可猜测；
- 分享链接的「知道即可读」是既有设计语义，内网监听仅扩大可达范围，不改变授权模型；
- macOS 首次绑非回环地址会触发防火墙授权弹窗；ad-hoc 签名（§6.3）使该授权身份稳定，避免每次构建重复弹窗。

---

## 6. Bazel 构建与 `.app` 打包

### 6.1 CGO binary target

rules_go 原生支持 cgo（含 wails darwin 依赖中的 Objective-C `.m` 源与 `-framework WebKit`/`Cocoa` 链接），macOS builder 上直接用系统 Xcode 工具链：

```python
# go/pl/loom/cmd/loom-desktop/BUILD
go_binary(
    name = "loom-desktop",
    srcs = ["main.go"],
    cgo = True,
    tags = ["manual"],   # 不进 //go/... 默认矩阵；CI 单独 job
    deps = [
        "//go/pl/loom/internal/app:app",
        "//go/pl/loom/internal/config:config_lib",
        "//go/pl/loom/internal/logging:logging",
        "//go/pl/loom/internal/runtimeevent:runtimeevent",
        "//go/pl/loom/internal/server:server",
        # ... 与 runServe 相同的依赖闭包
        "@com_github_wailsapp_wails_v2//:wails",
    ],
)
```

`MODULE.bazel` 的 `use_repo(go_deps, ...)` 补 `com_github_wailsapp_wails_v2`（`bazel mod tidy` 校正）。

**R-B1 实锤与三项适配（实现期排障记录）**：

1. **`production` 构建标签**：wails v2 的真实前端由 `//go:build production` 门控，否则 `wails.Run` 启动即报错。rules_go 有两层过滤——gazelle 生成仓库 BUILD 时按默认标签集求值（`production` 为假，`app_production.go` 被从 srcs 剔除），compilepkg 编译时再按上下文标签过滤。因此需要双管齐下：
   - `MODULE.bazel` 加 `go_deps.gazelle_override(path="github.com/wailsapp/wails/v2", directives=["gazelle:build_tags production"])`（让生成期选中 production 文件）；
   - `.bazelrc` 新增 `build:desktop --@rules_go//go/config:tags=production`（让编译期上下文标签生效；per-target 的 `gotags` 属性不会传导到外部依赖编译，已验证无效）。
2. **`UniformTypeIdentifiers` 链接缺口**：wails v2.13 darwin 前端引用 `UTType` 但未在自身 cgo LDFLAGS 声明该框架。双修：`frameworks_darwin.go` 的 in-source `#cgo LDFLAGS`（覆盖 `go build`，注意 cgo 前导注释会被当 C 解析，不能含撇号/破折号等散文）+ `go_library` 的 `clinkopts`（rules_go 只从规则属性收集链接参数，in-source 指令不传导，已验证）。
3. **`go_binary` 输出布局**：声明产物是 `loom-desktop_/loom-desktop` 目录结构，bundle 规则用 `pkg_files(renames={":loom-desktop": "loom-desktop"})` 扁平化。

退路仍然成立：普通 `go build -tags production`（in-source shim 使其自包含）产物与 Bazel 一致。

### 6.2 `.app` bundle 组装

`.app` 是固定布局目录，不依赖 Wails CLI：

```
Loom.app/
└── Contents/
    ├── Info.plist            # 由 macos/Info.plist.tmpl 经 genrule 渲染（v5，§6.5）：
    │                         # CFBundleExecutable/CFBundleIdentifier/CFBundleIconFile/
    │                         # TCC usage descriptions/LSApplicationCategoryType 等
    ├── MacOS/loom-desktop    # ← go_binary 产物
    └── Resources/AppIcon.icns  # ← macos/generate_icon.sh 生成并入库（v5）
```

用 `rules_pkg` 的 `pkg_files`（`prefix` + `renames` 摆好布局）+ `pkg_zip` 产出解压即 `.app` 的 zip（实现见 `cmd/loom-desktop/BUILD`）：

```python
pkg_files(name = "app_binary", srcs = [":loom-desktop"], prefix = "Contents/MacOS",
          renames = {":loom-desktop": "loom-desktop"},
          attributes = pkg_attributes(mode = "0755"))
pkg_files(name = "app_metadata", srcs = [":info_plist"], prefix = "Contents",
          renames = {":info_plist": "Info.plist"})
pkg_files(name = "app_resources", srcs = ["macos/AppIcon.icns"],
          prefix = "Contents/Resources", strip_prefix = strip_prefix.from_pkg("macos"))
pkg_zip(name = "loom_desktop_app",
        srcs = [":app_binary", ":app_metadata", ":app_resources"],
        package_dir = "Loom.app")
```

产物 `bazel-bin/go/pl/loom/cmd/loom-desktop/loom_desktop_app.zip`；`bazel run :package_app` 解包到 `dist/Loom.app` 并 ad-hoc 签名（已验证 `codesign -v` 通过、二进制可运行）。

**图标（v5）**：`macos/generate_icon.sh` 用一次性 Swift/CoreGraphics 程序把 favicon 菱形标渲染到 squircle 底板（深色渐变 + 青色菱形环，按 Apple 图标栅格占画布 ~66%），`iconutil` 打成全尺寸 `AppIcon.icns` 入库；换设计后重跑脚本提交即可。plist 挂 `CFBundleIconFile=AppIcon`。

### 6.3 签名

- 本机构建不签名即可运行（Gatekeeper 只管带 quarantine 的下载文件）；
- bundle 规则内附带 ad-hoc 签名一步（v5 起为 `codesign --force --sign -`；去掉了 `--deep`——单 Mach-O bundle 没有嵌套代码，Apple 已不推荐该旗标），换取防火墙授权、TCC 权限的身份稳定；
- Developer ID 签名 + 公证属分发工程，出范围（§1.2）。

### 6.4 CI

桌面 target 标记 `manual`，不进默认 `bazel build //go/...`（已验证通配符排除）；`.github/workflows/build_go.yml` 新增独立 `desktop` job（macOS runner）：`bazel build --config=ci --config=desktop //go/pl/loom/cmd/loom-desktop:loom_desktop_app`。Linux/Windows 桌面构建不进 CI（§1.2）。

### 6.5 版本单源与 release 打包（v5）

**版本单源**：`internal/version/VERSION` 是唯一权威版本字符串。Go 侧经 `//go:embed` 暴露 `version.Version`（完整串，如 `0.2.0-dev`）与 `version.Release`（去预发布后缀，如 `0.2.0`），`cmd/loom` 与 `cmd/loom-desktop` 全部改引用；bundle 侧由 genrule 把 `macos/Info.plist.tmpl` 的 `@VERSION@`/`@RELEASE@` 渲染成 Info.plist——二进制与 bundle 元数据不会漂移。改版本只动 VERSION 一个文件。

**release 四产物**：`bazel run --config=desktop //go/pl/loom/cmd/loom-desktop:package_release` 一次产出：

| 产物 | 架构 | 内容 |
|---|---|---|
| `dist/Loom-<ver>-macos-arm64.dmg` | darwin/arm64（thin） | `.app` + `/Applications` 软链 |
| `dist/Loom-<ver>-macos-x86_64.dmg` | darwin/amd64（thin） | 同上 |
| `dist/loom_<debver>_amd64.deb` | linux/amd64 | CLI → `/usr/bin/loom` |
| `dist/loom_<debver>_arm64.deb` | linux/arm64 | 同上 |

要点：

- **交叉编译走 Go 工具链而非 Bazel**：Bazel 自动检测的 cc toolchain 只覆盖宿主机 arch（交叉平台 cgo 会被禁用，wails 编不过）；Xcode clang 同 SDK 直接接受双 `-arch`。bundle 元数据/图标仍来自 Bazel zip，只替换 Mach-O。
- **release 一律 strip（`-s -w`）**：universal 时代 68M → per-arch 23M 级；DMG 体积约半减。dev 路径（`package_app`）保留符号。
- **deb 手工组装**（`ar` + `tar`，无 dpkg-deb 依赖）：Debian 版本规范化 `0.2.0-dev` → `0.2.0~dev-1`（无 revision 不得含连字符；`~` 使预发布排序先于正式版）。**坑**：macOS 的 `ar rc` 会隐式调 ranlib，非 Mach-O 成员被全部丢弃（产物只剩 `__.SYMDEF`），必须 `ar rcS`。
- **Linux GUI 不出包**：需 Linux 构建环境 + webkit2gtk + §2.3 通道适配，单独立项（§8 第 4 项）；deb 只装 CLI。

---

## 7. 改动清单

| 类别 | 文件 | 内容 | 状态 |
|---|---|---|---|
| 新增 | `cmd/loom-desktop/main.go` | §3.3 启动序列 + 优雅退出 + `--listen`/`--advertise` + `injectTokenMeta` | ✅ ~330 行（含与 cmd/loom 有意重复的 bootstrap helpers） |
| 新增 | `cmd/loom-desktop/frameworks_darwin.go` | UniformTypeIdentifiers cgo shim | ✅ |
| 新增 | `cmd/loom-desktop/main_test.go` | token 注入/advertise 推导/端口解析单测 | ✅ 全绿 |
| 新增 | `cmd/loom-desktop/BUILD`、`macos/Info.plist`、`package_app.sh` | cgo target + `.app` bundle + ad-hoc 签名 | ✅ 构建/签名/运行验证通过 |
| 修改 | `internal/server/server.go` | `Config.PublicBaseURL`（含规范化与 http(s) 校验） | ✅ |
| 修改 | `internal/server/handlers_share.go` | 分享响应附带绝对 `url` | ✅ |
| 修改 | `internal/server/web/static/js/main.js` | ① meta/fragment token 注入 ② 分享复制优先用绝对 `url` | ✅ |
| 修改 | `internal/server/handlers_share_test.go` | `TestShareAbsoluteURL` / `TestPublicBaseURLValidation` + 缺省不含 url 断言 | ✅ 全绿 |
| 修改 | `go/go.mod`、`MODULE.bazel` | wails v2.13 依赖 + `use_repo` + `gazelle_override` | ✅ |
| 修改 | `.bazelrc` | `build:desktop` 配置段（全局 production tag） | ✅ |
| 修改 | `.github/workflows/build_go.yml` | macOS desktop 打包 job | ✅ |
| 新增（v5） | `internal/version/` | VERSION 单源 + embed 包 + 形态测试 | ✅ |
| 新增（v5） | `cmd/loom-desktop/dialogs_darwin.go` | osascript 原生对话框（单实例提示 / workspace 选择） | ✅ |
| 新增（v5） | `cmd/loom-desktop/windowstate.go` | 窗口几何轮询持久化与恢复 | ✅ |
| 新增（v5） | `cmd/loom-desktop/notifications.go`、`notify_darwin.go` | broker 事件 → Notification Center | ✅ |
| 新增（v5） | `cmd/loom-desktop/macos/Info.plist.tmpl`、`AppIcon.icns`、`generate_icon.sh` | plist 模板化（TCC/元数据）+ squircle 图标 | ✅ |
| 新增（v5） | `cmd/loom-desktop/package_release.sh` | release 四产物打包 | ✅ |
| 修改（v5） | `cmd/loom/main.go`、`cmd/loom-desktop/main.go` | 版本切换 `version.Version`；平台集成接线 | ✅ |
| 修改（v5） | `cmd/loom-desktop/package_app.sh` | 去 `--deep`；bazel-bin 回退；图标缓存 touch | ✅ |
| 修改（v5） | `cmd/loom-desktop/main_test.go` | 转义/截断/窗口状态/通知过滤/回写回环 | ✅ 全绿 |

TUI（`internal/ui`）、WebUI 浏览器路径、`cmd/loom/main.go`、`internal/server/web/static/js/share.js`、agent/app/session 核心：**零改动**。

---

## 8. 后续演进（非本里程碑）

1. **Wails 原生绑定**：将 `client.Client` 方法 `Bind` 给前端（`window.go.main.Client.*`），事件经 `runtime.EventsEmit` 推送，进一步消掉 handler 调用开销。需重写 `api.js`/`sse.js` 桥接薄层，其余 UI 组件保留。前置条件：桌面端用户量证明体验瓶颈真实存在。
2. **桌面能力增强**：系统托盘、Dock 未读徽标、「复制分享地址」菜单项（原生通知已于 v5 落地，§3.5）；通知前台去重（`NSApp.isActive` 需主线程安全读取方案）。
3. **Wails v3 升级**：GA 后评估，变更收敛在 `cmd/loom-desktop` 内。
4. **Linux/Windows 桌面**：Linux 需 CI 镜像预装 webkit2gtk 开发包；Windows 除 WebView2 运行时外，还受 AssetServer 不支持流式响应限制（§2.3），届时走 §2.3 的 TCP loopback 降级路径。Linux **CLI** 的 deb 已于 v5 随 release 打包产出（§6.5）；GUI 版单独立项。
5. **分发工程**：Developer ID 签名 + 公证（hardened runtime + entitlements 为前置）；Sparkle 自动更新；`loom://` URL scheme 与深链（注册 plist 容易，handler 需配合分享链路设计）。

---

## 9. 风险与里程碑

### 9.1 风险登记

| 编号 | 风险 | 等级 | 状态 |
|---|---|---|---|
| R-B1 | wails 依赖树在 Bazel/Gazelle 下的 cgo 构建排障 | 中 | **已实锤并解决**（§6.1 三项适配；构建通过） |
| R-B2 | AssetServer 在 macOS 上的 SSE flush/POST 体行为与 net/http 存在偏差 | 中 | **已实锤并解决**：ContentLength 恒 -1（resume 误判为 create）+ 无 http.Flusher（SSE 不可能）——通道切换为常驻 loopback + bootstrap 跳转（§2.3），实机验证 resume/SSE/prompt 全通；服务端 ContentLength 判定同步修复（chunked 回归测试锁定） |
| R-B3 | wails origin 下 SPA 的浏览器假设（sessionStorage 隔离、CSP）偏差 | 低 | 已消解：SPA 实际运行在 http origin（loopback），与浏览器行为一致 |

### 9.2 里程碑拆解（实际执行）

| 步骤 | 内容 | 状态 |
|---|---|---|
| M4.0 | spike：窗口启动 + 服务端点冒烟 | ✅ 完成（webview 内流式 turn 留人工验收） |
| M4.1 | server 包 `PublicBaseURL` + main.js 复制点 + 测试 | ✅ 测试全绿 |
| M4.2 | `main.js` token 注入 | ✅（定稿 meta 注入，fragment 兼容） |
| M4.3 | `cmd/loom-desktop` cmd，`go build -tags production` 跑通 | ✅ 冒烟通过（healthz/SPA/401/分享页/token 不泄漏） |
| M4.4 | Bazel cgo target + `.app` 打包 + ad-hoc 签名 + CI job | ✅ `loom_desktop_app.zip` 产出、`codesign -v` 通过 |
| M4.5 | macOS 平台集成 + 版本单源 + release 四产物（v5） | ✅ 43/43 测试绿；DMG/deb 产物实机验证（架构、签名、deb 结构） |

人工验收清单（交付后）：窗口内完整流式对话一轮、审批卡片交互、`--listen 0.0.0.0:PORT` 下从内网另一台机器打开分享链接。
