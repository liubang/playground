# Loom Desktop 设计（M4）

- 状态：Draft v2（v1 经一轮对照代码与 Wails v2 API 事实的自审修订：前后端通道由「loopback TCP + 外部 URL 加载」更正为「AssetServer.Handler 进程内挂载」（Wails v2 无 StartURL 类选项，v1 方案不成立）；分享链接复制点由 share.js 更正为 main.js；新增风险 R-B2（AssetServer 流式响应行为）及其降级路径）
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
| 前后端通道 | **AssetServer 进程内挂载**：`assetserver.Options{Handler: srv.Handler()}`，webview 全部请求（静态资源、REST、SSE）由进程内 server handler 直接服务，默认零 TCP 监听 | TCP loopback + webview 加载外部 URL（**不成立**：Wails v2 `options.App` 没有 StartURL 类字段，见 §2.3）；Wails 原生绑定（需重写 api.js/sse.js 桥接层，MVP 工作量 3 倍，§8 再评估） |
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

**该方案成立的前提是 SSE 流式帧经 AssetServer 的 flush 行为与 net/http 一致**——矩阵标注 macOS 支持流式响应体，但 flush 延迟、POST 体边界等细节需在 M4.0 spike 中首个验证（§9 R-B2）。验证不通过的降级路径：常驻 `127.0.0.1:0` TCP 监听 + 微型 bootstrap 页（`Assets` 内嵌单个 `index.html`，内容仅 `location.replace("http://127.0.0.1:<port>/#token=...")`）跳转——代价是丢失 wails origin 下运行时注入（MVP 不用 Bindings，无实际损失）。

### 2.4 为什么进程内挂载方案成立

webview 到进程内 handler 的调用无序列化、无网络栈，SSE 高频帧（`model.text_delta`）零可感知延迟。关键收益是**前端与 WebUI 完全同一份代码**：

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
├── server.Server              # 传输适配器：Handler() 进程内挂载到 wails AssetServer
│                              # 仅当 --listen 指定非默认时追加 TCP Listen（§5，供内网分享）
└── wails.App                  # webview 窗口：wails://localhost/#token=<tok>（macOS）
```

关键性质：**一个进程、一个 SessionService、一个 broker；默认零 TCP 监听**（UI 全走进程内 handler），仅在用户显式开启内网分享时绑定 TCP 地址。

### 3.2 与现有入口的关系

| 入口 | 组装 | 锁 | 监听 | 前端 |
|---|---|---|---|---|
| `loom`（TUI） | `assembleRuntime` | 否 | 无 | inproc client |
| `loom serve` | `assembleRuntime` | 数据目录 flock | 可配置（默认 127.0.0.1:7680） | 任意 http client |
| `loom-desktop` | `assembleRuntime` | 数据目录 flock | 默认无；`--listen` 开启（§5） | 内嵌 webview（进程内 handler） |

桌面端与 `loom serve` 共享「数据目录 flock → assembleRuntime → broker → SessionService → server.New」序列；差异在最后：serve 一定 `Listen()` 并阻塞等信号，desktop 将 `srv.Handler()` 挂进 wails AssetServer 并进入 GUI 事件循环。

### 3.3 启动序列（`cmd/loom-desktop/main.go`）

```
1. 解析参数：--listen（默认空 = 仅进程内，无 TCP）、--advertise（可选，§5）
2. loadConfig(true) / prepareStorage / newFileLogger        ← 与 runServe 相同
3. AcquireDataDirLock(dataDir)                               ← 与 runServe 相同；冲突即报错退出
4. assembleRuntime(ctx, resolved, root, logger)              ← 原样复用
5. broker := runtimeevent.NewBroker(WithDurableQueue(4096))
   app.WireSubagentObserver(...)
   service := app.NewSessionService(...)
6. token := 32 字节随机（crypto/rand，进程内存，不落盘不打印）
7. srv := server.New(Config{Listen: <--listen 或占位>,
       Token: token, PublicBaseURL: deriveAdvertise(...),    ← §5 新增字段
       Version, Service, Service, Logger})
   if --listen 非空 { srv.Listen(); go srv.Serve() }          ← 仅内网分享时
8. wails.Run(&options.App{Title: "Loom", Width, Height,
       AssetServer: &assetserver.Options{Handler: srv.Handler()},
       Mac: &mac.Options{...},
       OnShutdown: 优雅退出序列（§3.4）})
   webview 起始 URL 由 main.js 的 fragment 注入逻辑补 token（§4.2）
```

注：fragment 注入不依赖启动 URL 可配——AssetServer 服务的 `index.html` 内 `main.js` 读取自身 `location.hash`，桌面壳只需在创建窗口后导航至 `/#token=<tok>`（或将 token 渲染进 bootstrap，二选一，实现时定）。

### 3.4 优雅退出

`OnShutdown` 回调复用 `runServe` 的退出序列，不新设计：

```
srv.Shutdown(60s)      # 若起过 TCP 监听：停止接受 HTTP、SSE 发出 server.draining 并尽快返回
service.Shutdown(60s)  # drain：进行中的 turn 跑完或被取消
broker.Close()
lock.Release()
```

同时保留 `signal.NotifyContext`（SIGINT/SIGTERM 走同一路径），保证 `kill` 与点关闭按钮语义一致。`srv.Shutdown` 在未调用 `Listen` 时退化为标记 draining + 关闭空闲连接，安全幂等（实现时验证该路径，必要时在 desktop 侧跳过该调用）。

---

## 4. 鉴权与 token 注入

### 4.1 约束继承

协议安全模型（SERVE_DESIGN §5.2/§6）不变：token 只走 `Authorization: Bearer` header；`/v1/*` 全部鉴权；静态资源与 `/v1/shared/*`、`/share/{token}` 公开。桌面端新增的问题只有一个：**进程内随机 token 用户无从知晓，SPA 的 token gate 必须被程序化跳过**。

### 4.2 URL fragment 注入（SPA 唯一改动）

入口 URL 采用 fragment 携带 token：`/#token=<hex>`（wails origin 下即 `wails://localhost/#token=<hex>`）。选 fragment 而非 query 的理由与协议禁止 URL 参数传 token 的理由一致（R4 泄漏面）：**fragment 不进入 HTTP 请求行**，不会出现在 access log、Referer、代理日志中；它只在页面 JS 上下文中可读，读出后立即抹除。

`internal/server/web/static/js/main.js` 启动处新增（gate 逻辑之前，约 10 行）：

```javascript
// Desktop bootstrap: the embedding shell passes the in-process token via
// URL fragment (never sent over the wire). Persist to sessionStorage and
// scrub the address bar before the gate runs.
const hashToken = new URLSearchParams(location.hash.slice(1)).get("token");
if (hashToken) {
  sessionStorage.setItem(TOKEN_KEY, hashToken);
  history.replaceState(null, "", location.pathname);
}
```

token 落 `sessionStorage` 后走现有全部路径（`api.js` 的 Bearer header、`sse.js` 的 fetch SSE），无第二条认证链路。浏览器模式不带 fragment，行为完全不变。

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

`MODULE.bazel` 的 `use_repo(go_deps, ...)` 需补 wails 及其直接依赖的 repo 名（`go mod tidy` 后由 `bazel mod tidy`/Gazelle 辅助生成）。

**已知风险 R-B1**：wails v2 依赖树中个别包（embed + cgo LDFLAGS 混用）在 Gazelle 生成的 BUILD 下可能需手工补 `cdeps`/`clinkopts`。预留约半天构建排障；退路是普通 `go build` 出二进制后交给 §6.2 的 bundle 规则（两者产物一致，Bazel 主矩阵不受影响）。

### 6.2 `.app` bundle 组装

`.app` 是固定布局目录，不依赖 Wails CLI：

```
Loom.app/
└── Contents/
    ├── Info.plist            # CFBundleExecutable/CFBundleIdentifier/
    │                         # NSHighResolutionCapable/LSMinimumSystemVersion
    ├── MacOS/loom-desktop    # ← go_binary 产物
    └── Resources/icon.icns
```

用 `rules_pkg` 的 `pkg_filegroup`（重映射路径）+ `pkg_zip` 产出解压即 `.app` 的 zip：

```python
pkg_zip(
    name = "loom_desktop_app",
    srcs = [":app_contents"],   # pkg_filegroup 摆好 Contents/... 布局
    package_dir = "Loom.app",
)
```

产物 `bazel-bin/go/pl/loom/cmd/loom-desktop/loom_desktop_app.zip`，解压拖入 `/Applications` 即用。新增静态文件：`cmd/loom-desktop/macos/Info.plist`、`cmd/loom-desktop/macos/Resources/icon.icns`。

### 6.3 签名

- 本机构建不签名即可运行（Gatekeeper 只管带 quarantine 的下载文件）；
- bundle 规则内附带 ad-hoc 签名一步（`codesign --force --deep --sign -`），换取防火墙授权、TCC 权限的身份稳定；
- Developer ID 签名 + 公证属分发工程，出范围（§1.2）。

### 6.4 CI

桌面 target 标记 `manual`，不进默认 `bazel build //go/...`；新增独立 CI job（仅 macOS runner）：`bazel build //go/pl/loom/cmd/loom-desktop:loom_desktop_app`。Linux/Windows 桌面构建不进 CI（§1.2）。

---

## 7. 改动清单

| 类别 | 文件 | 内容 | 规模 |
|---|---|---|---|
| 新增 | `cmd/loom-desktop/main.go` | §3.3 启动序列 + 优雅退出 + `--listen`/`--advertise` + fragment 导航 | ~180 行 |
| 新增 | `cmd/loom-desktop/BUILD` | cgo `go_binary`（manual）+ `pkg_zip` `.app` + ad-hoc codesign | ~50 行 |
| 新增 | `cmd/loom-desktop/macos/Info.plist`、`Resources/icon.icns` | bundle 元数据 | 静态 |
| 修改 | `internal/server/server.go` | `Config.PublicBaseURL` 字段 | ~3 行 |
| 修改 | `internal/server/handlers_share.go` | 分享响应附带绝对 `url` | ~5 行 |
| 修改 | `internal/server/web/static/js/main.js` | ① fragment token 注入（~10 行）② 分享复制优先用绝对 `url`（~3 行） | ~13 行 |
| 修改 | `go/go.mod`、`MODULE.bazel` | wails 依赖 + `use_repo` 补充 | 若干行 |
| 修改 | `internal/server/handlers_share_test.go` | `PublicBaseURL` 分享响应用例 | 1~2 个用例 |

TUI（`internal/ui`）、WebUI 浏览器路径、`cmd/loom/main.go`、`internal/server/web/static/js/share.js`、agent/app/session 核心：**零改动**。

---

## 8. 后续演进（非本里程碑）

1. **Wails 原生绑定**：将 `client.Client` 方法 `Bind` 给前端（`window.go.main.Client.*`），事件经 `runtime.EventsEmit` 推送，进一步消掉 handler 调用开销。需重写 `api.js`/`sse.js` 桥接薄层，其余 UI 组件保留。前置条件：桌面端用户量证明体验瓶颈真实存在。
2. **桌面能力增强**：系统托盘、Dock 未读徽标、原生通知（turn 完成/审批待办）、「复制分享地址」菜单项。
3. **Wails v3 升级**：GA 后评估，变更收敛在 `cmd/loom-desktop` 内。
4. **Linux/Windows 桌面**：Linux 需 CI 镜像预装 webkit2gtk 开发包；Windows 除 WebView2 运行时外，还受 AssetServer 不支持流式响应限制（§2.3），届时走 §2.3 的 TCP loopback 降级路径。

---

## 9. 风险与里程碑

### 9.1 风险登记

| 编号 | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R-B1 | wails 依赖树在 Bazel/Gazelle 下的 cgo 构建排障 | 中 | 预留半天；退路 `go build` + bundle 规则，主矩阵不受影响 |
| R-B2 | AssetServer 在 macOS 上的 SSE flush/POST 体行为与 net/http 存在偏差（官方矩阵标注支持，但未承诺 flush 语义） | 中 | **M4.0 spike 首个验证**：起 wails 窗口挂 `srv.Handler()`，跑通一次流式 turn；失败则降级 §2.3 bootstrap 跳转方案（设计其余部分不变） |
| R-B3 | wails origin 下 SPA 隐含的浏览器假设（sessionStorage 隔离、CSP、`history.replaceState`）行为偏差 | 低 | spike 中一并验证；`srv.Handler()` 已含全部安全头，CSP 对自定义 scheme 无额外假设 |

### 9.2 里程碑拆解

| 步骤 | 内容 | 验收 |
|---|---|---|
| M4.0 | **spike（先于一切）**：最小 wails 程序挂载 `srv.Handler()`，验证 SSE 流式 turn + POST prompt + fragment 注入（R-B2/R-B3） | 流式对话在窗口内完整跑通 |
| M4.1 | server 包分享 URL 增强（`PublicBaseURL` + main.js 复制点）+ 测试 | 独立可测，`loom serve` 行为不变 |
| M4.2 | `main.js` fragment token 注入 | 浏览器模式无回归（无 fragment 时走原 gate） |
| M4.3 | `cmd/loom-desktop` cmd，`go build` 跑通窗口 + 对话 + 审批 + 分享闭环 | 手动验收三 UI 行为一致 |
| M4.4 | Bazel cgo target + `.app` 打包 + ad-hoc 签名 + CI job | `bazel build //go/pl/loom/cmd/loom-desktop:loom_desktop_app` 产出可用 bundle |

预估总工作量：M4.0 约半天；M4.1~M4.3 约 1~2 天；M4.4 约 0.5~1 天（含 R-B1 排障余量）。
