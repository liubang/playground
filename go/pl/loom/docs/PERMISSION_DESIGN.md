# PERMISSION_DESIGN — loom 权限体系：三层信任梯度与可插拔策略

> 状态：v2（M1 + 复合命令 AST 分析已落地）
> 关联：DESIGN.md §12（安全基座）、CONFIG_DESIGN.md、本文件取代其中权限章节的语义描述

## 1. 背景与问题

loom 现行权限模型是**二值信任**：一次工具调用要么逐次审批（ask），要么凭规则/会话记忆永久免审批（allow），且：

1. **allow 不免沙箱**：规则命中的命令仍在 Seatbelt 沙箱内执行（禁外网、写仅限 workspace+tmp）。需要外网/凭证/家目录写入的工作流（如 bi-query-sql skill）在沙箱内必然失败，模型被工具引导语推向 `require_escalated` 提权重试。
2. **提权永不豁免**：`RunCmdArgv` 对 `require_escalated` 调用硬性返回不可入规则，每次提权（R3）必弹审批。前述 skill 一个会话 13 次 `run_cmd` 弹了 9 次审批，全部发生在提权重试上。
3. **审批轰炸即安全降级**：用户被高频审批训练成"无脑点允许"，审批失去意义。这是二值模型的系统性后果，不是用户习惯问题。

业界两种参照（2026-07 调研，见 §9）：

- **Codex CLI**：默认沙箱（三平台），execpolicy `allow` 规则 = 免审批 + **直接绕过沙箱**（`bypass_sandbox`），提权命令可经 `prefix_rule` 一次批准永久豁免。体验最好，但"常驻的满权限信任"被提示注入利用的代价最高：allow 的前缀若指向可变内容（脚本/Makefile/package.json），workspace 可写 + 沙箱外执行构成 write-then-execute 提权链。
- **Claude Code**：沙箱内 auto-allow（沙箱即边界，不问人）；allow 规则只免审批、**不放沙箱边界**；网络按**域名**逐个授权（本地代理强制）；出沙箱（`dangerouslyDisableSandbox`）逐次审批，且提供焊死逃生舱的严格模式。

**结论**：loom 取 Claude Code 的信任边界 + codex 的工程形态，**不取** codex 的"allow 默认 bypass 沙箱"。

## 2. 设计目标 / 非目标

### 2.1 目标

- **G1 三层信任梯度**替代二值模型：沙箱内默认放行（L0）、规则携带能力授予（L1）、显式出沙箱信任（L2）。
- **G2 策略可插拔**：权限判定抽象为一组可独立替换的 `Decider`，基线策略（审批模式）可配置切换，后续可换实现（如引入分类器、远程策略服务）而不动调用方。
- **G3 能力授予最小化**：持久授权以"能力包"（网络、可写路径）表达，而非"摘下沙箱"。
- **G4 修复现存漏洞**：workspace 可写根下的 `.git`/`.loom`（含 hooks）保持只读；敏感读路径同时禁止 unlink/rename 探测。
- **G5 兼容**：旧规则文件（无 grant）语义不变；现有配置键行为不变或显式迁移。

### 2.2 非目标

- 域名级网络授权（本地代理 + 按域名记忆）：M3 单独立项，本设计只预留接口形状（§8.3）。
- Linux/Windows 沙箱实现：维持 Linux fail-closed 现状；能力模型按平台无关设计，不阻塞后续补实现。
- 规则 DSL 更换：JSON 规则文件 + 加载时自检（match/not_match）保留，不换 execpolicy 式 DSL。

## 3. 核心概念

### 3.1 三层信任梯度

| 层 | 名称 | 审批 | 执行方式 | 产生途径 |
|---|---|---|---|---|
| L0 | sandboxed | 免审批 | Seatbelt 沙箱（禁外网、写 workspace+tmp、凭证路径不可读） | 默认状态（on-request 基线下所有非危险 `run_cmd`） |
| L1 | granted | 免审批 | 沙箱"按需开口"：按规则 `grant` 放宽网络/可写路径，其余边界不变 | 用户层规则文件，或 "allow always" 记忆 |
| L2 | trusted | 免审批 | 完全出沙箱（完整环境/网络/凭证） | 仅用户显式 opt-in，UI 标红警示 |
| — | denied | — | 不执行 | deny 规则、危险命令清单（never 模式直接拒绝） |

判定优先级（即链序）：**规则层（deny/ask/allow，严格者胜）> 危险清单 > 会话记忆 > 基线**。显式用户意图（文件规则）可覆盖危险启发式；会话记忆只能升级"无意见"，永不覆盖前两层（§7.0 对此语义梯度有明示）。

### 3.2 能力授予（CapabilityGrant）

平台无关的执行能力描述，是"策略层"与"执行层"之间的唯一契约：

```go
// CapabilityGrant describes the execution capabilities a verdict grants.
// The zero value is the default sandbox (loopback-only network,
// workspace+tmp writes) — grants only ever widen it, never narrow.
type CapabilityGrant struct {
    // Unsandboxed runs outside the sandbox entirely (L2). When false the
    // call executes under the platform sandbox with the widenings below.
    Unsandboxed bool
    // NetworkFull allows outbound network/DNS inside the sandbox.
    NetworkFull bool
    // WritablePaths are additional absolute paths writable inside the
    // sandbox (cleaned + deduped; protected subpaths still excluded, §6.3).
    WritablePaths []string
}
```

- L0：`CapabilityGrant{}`（零值）
- L1：`{NetworkFull: true, WritablePaths: ["~/.myapp"]}` 等
- L2：`{Unsandboxed: true}`（其余字段无意义，加载时校验互斥）

`process.SeatbeltSandbox` 现有 `AllowNetwork`/`WritablePaths` 选项天然可消费该结构，执行层改动很小（§6.4）。

### 3.3 为什么 L1 的安全代价可控

沙箱内 `network: full` 的直觉风险是数据外泄，但 loom 已有 `sensitiveReadDenies`（`~/.ssh`、`~/.aws`、`~/.kube`、Keychain 等凭证路径沙箱内不可读，M1 起再补 unlink 保护）。沙箱内放网后：能读到的本不敏感，敏感的读不到——外泄价值有限。这正是"能力授予"优于"摘沙箱"的杠杆点：grant 只开放该项能力，其余边界（凭证不可读、家目录不可写）全部保留。

## 4. 架构：可插拔策略链

### 4.1 核心接口

```go
// package permission

// Verdict is one decider's judgment on a prepared call.
type Verdict struct {
    Decision domain.Decision  // allow | ask | deny
    Grant    CapabilityGrant  // meaningful only when Decision == allow
    Rule     *Rule            // the rule that produced this verdict (audit/UI)
    Reason   string           // human-readable provenance
}

// Decider judges a prepared call. Returning nil means "no opinion" —
// the chain consults the next decider.
type Decider interface {
    Evaluate(call domain.PreparedCall) *Verdict
}

// Chain returns the first non-nil verdict (deciders ordered strictest-first).
type Chain []Decider
func (c Chain) Evaluate(call domain.PreparedCall) *Verdict
```

- `nil` 表示"无意见"，**非 nil 即终局**——链按"最严格优先"排序，与规则层 strictest-wins 语义自洽。
- 调用方（agent 主循环、controller）只依赖 `Decider`，不知道具体策略组合——**换策略 = 换链的装配**，满足 G2。
- 现行 `Policy.Evaluate(call) domain.Decision` 签名升级为返回 `*Verdict`；agent run.go 的 switch 点改为消费 `Verdict.Decision`，`Verdict.Grant` 随 `prepared` 一起传给执行层（§6.4）。

### 4.2 内置 Decider（M1）

按链序（先命中先生效）：

| # | Decider | 语义 | 可产生 |
|---|---|---|---|
| 1 | `RuleDecider` | 文件层规则（builtin+user+project）argv 前缀匹配，strictest wins；deny/ask 永远先返回 | deny / ask / allow(L0|L1|L2) |
| 2 | `DangerDecider` | 危险启发式双层筛查：argv 级（`rm -rf /`、`dd`、`sudo`、凭证外泄形状）+ 脚本级 AST 筛查（`danger_script.go`：管道入 shell/解释器、重定向入敏感路径、嵌套在替换/子 shell 中的危险字面量）；仅作用于无规则命中时 | ask / deny |
| 3 | `SessionDecider` | 会话记忆前缀（"allow always" 产物），只能把"无意见"升级为 allow，永不覆盖文件层 | allow(L1) |
| 4 | `BaselineDecider` | 审批模式基线（§4.3），兜底永不返回 nil | allow(L0) / ask / deny |

`RuleDecider` 内部维持现行语义：trusted 目录 basename 归一化（`/bin/ls` 命中 `[ls]`）、项目层 `allow` 需 `project_allow` 显式开启、**项目层与 builtin 的规则禁止携带 grant 与 `unsandboxed`**（不可信层只能收紧，grant 也是一种放宽）。

### 4.3 BaselineDecider 与审批模式

```yaml
# ~/.loom/config.yaml
approval:
  mode: on-request   # on-request | unless-dangerous | never（默认 on-request）
```

| 模式 | 沙箱内 run_cmd | 工作区内建工具（R0–R2） | needs_network | 提权 | 危险清单 |
|---|---|---|---|---|---|
| `on-request` | **allow（沙箱即边界）** | **allow（path validator 即边界）** | ask（可记忆） | ask | ask |
| `unless-dangerous` | allow | allow | allow（沙箱内放网） | ask | ask |
| `never`（无人值守） | allow | allow | allow | deny（带绕行指引） | deny |

- 危险清单命中时交互模式升级为 ask（never 模式为 deny，denial 原因直达模型以便绕行）。
- 复合 shell 命令（管道/`&&`/重定向）不再是风险本身：`sh -c` 脚本经 AST 解析（mvdan.cc/sh）逐条子命令过规则层与危险清单；可证明安全的复合命令可按子命令前缀记忆。只有出沙箱、出网络、危险清单三类事件弹审批。
- 非 exec 的内建工具（edit/write 等）由 path validator 限制在工作区内（.git/.loom 受保护），爆炸半径与沙箱内命令等价，R0–R2 在任何模式下免审批；MCP 工具是第三方代码，保持逐次审批（可按工具名记忆），never 模式下拒绝。
- **unless-dangerous 下 builtin `web_fetch` 免审批**：它是无凭证的匿名 GET（不携带用户浏览器身份/cookie），SSRF 防护默认拦截私有/回环/链路本地目标（`allow_private=true` 显式开启），能力严格弱于该模式已静默放行的沙箱内 `needs_network` 命令。**deny/ask 域名规则仍优先**（RuleDecider 在链首，用户显式黑名单永远生效），on-request 仍逐次审批（可记忆域名），never 仍拒绝。`browser` 工具不受此豁免——它驱动真实用户浏览器（真实身份/cookie），即使目标域名已记住也保持逐次审批。
- `never` 模式绝不产生 ask：长程任务不会死挂在无人应答的提示上。
- 审批弹窗触发桌面通知（macOS osascript / Linux notify-send）：真正需要人时人要知道。

### 4.4 装配

`internal/app/bootstrap.go` 按配置构造链：

```go
// Policy.Decider(mode) 的实现（decider 构造即装配）：
chain := permission.Chain{
    permission.RuleDecider{Rules: ruleSet},        // nil-safe
    permission.DangerDecider{Mode: mode},          // 危险清单，never 模式 deny
    permission.SessionDecider{Session: session},   // nil-safe
    permission.BaselineDecider{Mode: mode},        // 审批模式基线，兜底
}
```

每个 Decider 独立可测；新策略（如未来的"分类器 Decider"、"域名网络 Decider"）只需实现接口并插入链的合适位置。

## 5. 规则 Schema v2

```json
{
  "rules": [{
    "argv_prefix": ["/Users/liubang/.mycli/bin/mycli"],
    "decision": "allow",
    "justification": "mycli CLI, needs network + its own config dir",
    "grant": {
      "network": "full",
      "write": ["/Users/liubang/.mycli"]
    },
    "match": ["/Users/liubang/.mycli/bin/mycli query submit --scene REPORT"],
    "not_match": ["mycli"]
  },
  {
    "argv_prefix": ["make", "deploy"],
    "decision": "allow",
    "grant": {"unsandboxed": true},
    "justification": "needs SSH + kube credentials (user-layer only)"
  }]
}
```

加载时校验（沿用"自检失败整文件拒绝"）：

1. `grant.unsandboxed: true` 与 `network`/`write` 互斥；`decision != "allow"` 时禁止携带 grant。
2. **仅用户层**（`~/.loom/rules`）规则可携带 `grant`；项目层携带则被**剥离**（grant 置空、decision 保留，记 warning）——不可信层只能收紧。builtin 层携带 grant 视为构建期 bug，加载即报错。
3. 空 `argv_prefix` + grant 拒绝（空前缀只可用于 deny/ask，现行注释已有此约束，升级为硬校验）。
4. `write` 路径展开 `~`、clean、去重；落在 workspace 内的条目视为无操作（workspace 本就可写）。
5. 旧文件无 `grant` 字段：语义 = L0 allow，行为与今天逐字节一致（G5）。

`loom rules list` 输出 grant 标注；`loom rules check <argv...>` 输出最终 Verdict（decision + grant + 命中规则来源）。

## 6. 组件改动详述（M1）

### 6.1 `internal/permission`

- 新增 `verdict.go`：`Verdict`、`ApprovalMode`（on-request / unless-dangerous / never）、`Decider`、`Chain`（含 contextDecider 单次解析快路径）。
- 内置 Decider 集中于 `decider.go`（Rule/Danger/Session/Baseline 四实现）。`Policy` 结构简化为装配参数包（Rules + Session），不再携带风险基线开关——基线行为完全由审批模式决定（§4.3）。
- `rules.go`：Rule 增加 `Grant *RuleGrant`；`validateRule` 增加 §5 校验；`LoadRuleSets` 按层剥离非法 grant。
- `danger.go`：argv 级危险命令清单（程序级：`dd`/`mkfs`/`shred`/`sudo`；子命令级：`git push --force`、`git reset --hard`、`git clean -f`；目标级：`rm -r` 指向 `/`、`~`、系统目录或 `..` 逃逸；凭证外泄：`curl -d @~/.ssh/...` 等网络工具 argv 含凭证路径；包装器剥离：env/nice/nohup/timeout/command 前缀后递归筛查）。
- `danger_script.go`：脚本级 AST 筛查（**v2 新增**，见 §6.4.1）：`sh -c` 脚本经 mvdan.cc/sh 解析后，逐条子命令过 argv 级清单（嵌套在 `$(...)`、子 shell、`&&` 中的危险字面量无处隐藏），另查两类跨命令形状——管道入 shell/解释器执行流（`curl | sh`，消费方带脚本文件参数的不算）、写重定向入敏感路径（shell 启动文件、凭证目录、`.git` hooks/config 含 submodule/worktree、`.loom`、关键根）。
- `SessionRules`：记忆条目从 `[]string` 升级为 `{Prefix []string, Grant ExecGrant}`；"allow always" 记录模型在提权重试时声明的能力（§6.4 的 `needs_network`），默认 grant 而非 unsandboxed。复合 shell 命令**按子命令各记一条前缀**（`DeriveRunCmdPrefixes`），匹配侧 `MatchAll` 要求每条子命令都被记忆覆盖、grant 取并集——一个未记忆的子命令即整脚本回落常规判定。
- `AppendRememberedRule`：写入 `remembered.json` 时携带 grant（schema v2）。

### 6.2 `internal/domain`

- 新增平台无关的 `ExecGrant` 值类型（`Unsandboxed` / `NetworkFull` / `WritablePaths`）与 `Verdict`（`Decision` + `Grant` + `Source` + `Reason`），保持依赖方向 `permission → domain`。
- **（实现偏差，已确认更优）**`PreparedCall` 直接增加 `Grant ExecGrant` 字段而非旁路 map：Grant 由 agent 主循环在 Prepare 签名之后、策略评估时赋值，刻意留在 HMAC 指纹之外——能力由策略决定，模型无法影响。`run_cmd.Execute` 直接消费 `prepared.Grant`。

### 6.3 `internal/process`（P0 安全补漏，随 M1 上）

- workspace 可写根的 `<workspace>/.git/hooks`、`<workspace>/.git/config`、`<workspace>/.loom` 保持只读。**（实现偏差）** 不用 codex 式 `require-all + require-not`，改为**尾部 deny**：`(deny file-write* (literal/subpath X))` 放在所有 allow 之后（seatbelt 最后匹配胜出），否则 workspace 位于其他可写根内（如 TMPDIR 下的测试场景）时保护会被后置的宽泛 allow 抵消。日常 `git commit` 不受影响的回归测试已建立（`git init` 因创建 hooks 样板被拦，属已知取舍，与 codex 一致）。
- `sensitiveReadDenies` 每条补 `(deny file-write-unlink ...)`，防删除/重命名探测。
- **（范围扩大）顺手修复潜伏 bug**：seatbelt 按规范化路径匹配，macOS 的 `/var`、`/tmp` 是指向 `/private` 的符号链接——旧 profile 里 `os.TempDir()` 的 `/var/folders/...` 形式**从未生效**，沙箱内 TMPDIR 写入一直静默失败。所有可写路径现在经规范化（含符号链接解析、未存在路径向上溯源；现为 `workspace.Canonicalize`，两层边界共用）。
- 执行入口：`Runner.RunWithGrant(ctx, spec, process.Grant)` 统一映射——`Unsandboxed` → DirectSandbox；零值 → 默认沙箱；其余 → `widenSandbox` 克隆 Seatbelt（网络/可写路径并集）。不支持的平台（Linux）加宽为空操作，fail-closed 保持。

### 6.3.2 默认可写范围（scratch + 工具链缓存）

workspace 之外，沙箱默认放行两类可写目录（`process.ExtraWritableDirs`）：

1. **系统 scratch 目录**：`$TMPDIR`（per-user）与 `/tmp`（canonical）。文件工具侧同样放行——`PathValidator` 的 extra roots 由 `workspace.ScratchDirs()` 提供，两层边界同一来源，不会出现 run_cmd 能写而 write_file 被拒的分裂。风险接受依据：sticky bit 下进程只能删除自己的文件，误操作爆炸半径远小于 home；凭证路径另有 sensitive 清单兜底。`/var/tmp` 不放（语义是重启后保留，算半个 home）。
2. **可再生工具链缓存**（仅沙箱层，文件工具不需要）：Go（`~/Library/Caches/go-build`、`~/.cache/go-build`、`~/go/pkg/mod`）、npm（`~/.npm`）、pip（双平台 caches）、cargo（`~/.cargo/registry`、`~/.cargo/git`）、ccache（双平台）、Gradle（`~/.gradle`）、Maven（`~/.m2/repository`）。依据：损坏最多一次 rebuild；sensitive 清单不覆盖。整放 `~/.cache`/`~/Library/Caches` 被否决：那是杂烩目录（浏览器缓存、应用数据、个别 CLI 的 token），不属于 scratch 语义。

### 6.3.1 Grant 覆盖检查（review C1 修复）

**v1（无 grant）的 allow 只覆盖普通沙箱调用**。当调用声明了规则未曾授予的能力（`require_escalated` 或 `needs_network`）而命中规则的 grant 为零时，RuleDecider/SessionDecider 视为无意见、落入基线：基线 ask 会盖上正确 grant（提权→`Unsandboxed`，网络→`NetworkFull`），用户批准一次即完成 v1→v2 升级（"allow always" 记忆带 grant）。不变量：**L0 信任永远不能被静默升格为 L2 出沙箱执行**；执行层对应地不存在"零 grant + 提权 → DirectSandbox"的兜底——零 grant 在任何路径下都意味着默认沙箱。非零 grant 则始终按其内容执行（网络 grant 对提权调用是合法的降级）。

### 6.4 `internal/tool/command`（run_cmd）

- 参数 schema 增加 `needs_network: bool`（可选）：模型在沙箱失败重试时**声明能力**而非直接提权。`needs_network: true` 的调用保持 R2 评估，on-request 基线下弹一次 ask（批准后选 "allow always" 则记忆 `{prefix, grant:{network:full}}`），unless-dangerous 与 never 模式下沙箱内直接放网。
- **（v2 变更）shell 复合命令不再升级 R3**：旧实现把不可拆解的 `sh -c` 一律升为 R3 逐次审批；AST 分析落地后，组合本身不再是风险——沙箱照常约束执行，规则层按子命令前缀评估，危险清单做脚本级筛查。只有 `require_escalated`（出沙箱）升 R3。工具描述同步改为"单命令优先 argv 形式，需要管道/重定向/&& 时自由使用 sh -c"。
- 工具描述与 `sandboxGuidanceNote` 改写：引导顺序从"失败 → require_escalated"改为"网络类失败 → needs_network 重试；TTY/凭证类失败 → require_escalated"。
- 执行处：消费 `ExecGrant`——`Unsandboxed` → `DirectSandbox{}`（现提权路径）；否则 Seatbelt + `AllowNetwork=grant.NetworkFull` + `WritablePaths+=grant.WritablePaths`；输出 `isolation` 字段如实标注（`seatbelt+net`、`seatbelt`、`direct`）。
- `require_escalated` 保留（L2 路径），但不再被 `RunCmdArgv` 硬排除：提权调用可命中带 `unsandboxed` grant 的用户层规则（L2 豁免）——这是 skill 场景的兜底逃生门，默认不产生、需用户显式批准。

### 6.4.1 复合 shell 的 AST 静态分析（v2 新增）

`internal/process/shell_analyze.go` 用 mvdan.cc/sh 把 `sh -c` 脚本解析为 AST，单次遍历产出 `ShellAnalysis`：

- `Commands`：脚本中每条简单命令（含嵌套在命令替换、子 shell 中的）的字面 argv；含变量/替换/算术展开的词标记为动态（argv 不可证明）。
- `Static`：全脚本可静态证明（无变量、无控制流、无子 shell、无 heredoc、无后台任务、无 env 前缀赋值）。
- `Pipes`：每条管道的「生产者 → 消费者」对，消费者附完整静态 argv（区分 `| sh` 执行流与 `| python3 analyze.py` 跑固定脚本）。
- `WriteRedirects` / `DynamicWrites`：写重定向的静态目标 / 动态目标标记。

三个消费方各自取用：**规则层**仅对全静态且无写重定向的脚本按子命令前缀匹配（一条 deny/ask 即定案，allow 要求条条命中且 grant 并集覆盖调用声明）；**危险清单**无条件筛查——子命令逐条过 argv 级清单 + 管道入解释器 + 重定向入敏感路径，动态部分查不出字面量即视为无证据（交给沙箱兜底）；**会话记忆**仅静态脚本可按子命令前缀记忆（§6.1 SessionRules）。执行路径不受影响——脚本照常经 shell 在沙箱内运行，分析只喂养分类。

### 6.5 `internal/app` + `internal/ui`

- 审批选项从两项扩展（run_cmd 且可推导 grant 时；**已与用户确认两种记忆口味都给，grant 标为推荐**）：
  1. `允许一次`
  2. `始终允许（最小能力）`——默认推荐；记住 `{prefix, grant}`，grant 来源：模型 `needs_network` 声明，或提权调用默认推导为网络 grant（M1 的最常见原因近似），UI 展示推导结果
  3. `完全信任（出沙箱）`——仅提权调用出现的第四行，记住 `{prefix, unsandboxed}`
- `RuleApprover.RememberRunCmd` 升级为记住 `{prefixes, grant}`——复合 shell 每条子命令一条前缀（§6.1）；`RunCmdRulePreview` 展示 grant 摘要与全部前缀（如 `go test && git status (+网络)`）。
- 审批 UI 的 grant 展示即"选择架构"：推荐项默认高亮，出沙箱项需额外确认。
- **桌面通知（v2 新增，`app/notify.go`）**：审批请求产生时经 macOS `osascript` / Linux `notify-send` 发系统通知——长程任务挂起等人时，人通常不在终端前。无人值守（stdin 非 TTY）时请求直接拒绝并通知"已自动拒绝"；通知失败静默降级，永不影响审批流。

### 6.6 `internal/config`

- 新增 `approval.mode`（§4.3）；`rules` 节不变。
- `resolve.go`：mode 解析 + 非法值报错；默认 `on-request`。

## 7. 安全分析

### 7.0 已知语义梯度（review L2，明示）

会话内记忆由 SessionDecider 评估（位于 DangerDecider 之后，危险屏优先）；而 "allow always" 同时持久化到用户层文件，**下个会话**它作为 RuleDecider 命中（位于 DangerDecider 之前）——例如记住 `git push` 后，`git push --force` 当次会话仍弹危险审批，下个会话起免审批放行。这是"显式用户意图可信"的有意设计，但同一份意图在两个时间窗待遇不同，在此明示。

### 7.1 威胁模型不变量

| 不变量 | 保障机制 |
|---|---|
| 不可信层（项目/builtin）永不放宽 | grant/unsandboxed 仅用户层；项目层 allow 仍需显式开关 |
| deny 永不被覆盖 | 链序 + strictest-wins；SessionDecider 只升级"无意见" |
| 模型无法自授 trust | 规则目录在 `~/.loom`（workspace 外，沙箱内不可写）；grant 只能来自用户文件或交互批准 |
| 凭证不出沙箱 | sensitiveReadDenies + unlink 保护对 L0/L1 全部生效；grant 不含读权限放宽 |
| workspace 可写 ≠ 信任链可改 | `.git`/`.loom` 只读（§6.3），hook/规则注入被堵 |
| L2 永远留痕 | 提权/L2 执行在审计事件与 `isolation` 字段中显式标注 |

### 7.2 已知残留风险（明示）

1. **L1 前缀指向可变内容**（`node script.js` 类）：workspace 可写意味着脚本内容可被模型改写。缓解：script 路径进前缀（现行 `DeriveRunCmdPrefix` 已如此）；根治（内容哈希绑定）列入 M2 评估，不做承诺。
2. **`network: full` + workspace 读**：workspace 内容（源码）可被外发。这是能力授予的固有代价，由用户在批准时知情（grant 在审批 UI 明示）；域名级授权（M3）是收敛方向。
3. **危险清单覆盖不全**：启发式必然有漏。定位是"减少明显危险"，不是完备拦截——底座仍是沙箱与 deny 规则。
4. **AST 分析只识别字面量**：动态脚本（变量、替换、控制流）的子命令 argv 不可证明，规则层与会话记忆对其无意见（回落基线），危险清单也只能看到字面部分——经变量间接拼接的危险命令（`X=rm; $X -rf /`）不在筛查视野内，由沙箱与敏感路径写保护兜底。带写重定向的脚本整体不享规则层 allow（argv 规则只为 argv 背书，重定向目标的判断属于危险屏）。

## 8. 里程碑

### M1（已落地）
三层信任梯度全链路：Decider 抽象 + 四种内置实现、规则 schema v2 + grant、on-request 基线、危险清单 v1、P0 写保护（.git/.loom + unlink）、run_cmd `needs_network`、审批三选项 + grant 记忆/持久化、`loom rules check` 输出 Verdict。

**v2 增量（2026-08 已落地）**：复合 shell AST 静态分析（§6.4.1）——规则层按子命令前缀评估、危险清单脚本级筛查（管道入解释器、重定向入敏感路径、嵌套危险字面量）、会话记忆按子命令前缀；shell 复合命令不再升 R3；`unless-dangerous` / `never` 审批模式（never 绝不产生 ask，危险命令与提权带绕行指引直接 deny，denial 原因直达模型）；web_fetch 域名规则（精确 + `*.` 通配，builtin 集含包管理源白名单与外泄渠道黑名单，`loom rules check --url` 干跑）；审批桌面通知（含无人值守自动拒绝通知）；沙箱 `/dev/null`、`/dev/(u)random` 放行补漏。

### M2
危险清单细化（git 写操作、环境窃取模式）；脚本内容哈希绑定评估；`grant.write` 的自动路径推断（从沙箱拒绝日志反推）。

**规则包（rule packs，2026-08 已落地）**：沙箱内 macOS TLS 验证对 Security-framework 依赖型运行时（Go 的 crypto/x509、pip 的 vendored truststore）必然失败（`OSStatus -26276`，实测无文件/mach 级解法，Claude Code 同款结论）。为"已知可信但沙箱不兼容"的命令提供**用户显式开启**的预授权：`internal/permission/packs/*.json` 内嵌模板（元数据 + 标准规则文件），WebUI 设置「规则包」卡片一键启用/停用，安装即写 `~/.loom/rules/pack-<id>.json`（普通规则文件，LoadRuleSets 零特判加载、`loom rules check` 可审计、可手动编辑），安装/卸载即时热重载全部 workspace 策略。内置三包：`go-toolchain`（go mod download/tidy/vendor，中风险）、`cloud-cli`（gh api、gcloud auth list、terraform plan/fmt/validate——**只读形态**，高风险的 apply/destroy 不预授权，写凭证目录仍受保护）、`python-pip`（pip install/download）。安全红线：包规则必须 allow+grant、只授 unsandboxed 不组合凭证写、绝不包含代码执行形态（go run/install）；builtin 只读集仍硬校验"零 grant"。

### M3
域名级网络授权：本地代理 + 按域名审批记忆（`grant.network: {domains: [...]}` 的 schema 预留形状）；Linux bubblewrap 沙箱。

**（M3-lite 已提前落地，2026-07-30；通配符 2026-08 补齐）** web_fetch 的域名级记忆：规则文件新增 `"domains"` 节（`host` 精确匹配、`*.` 后缀通配——`*.example.com` 覆盖任意子域但不含 apex 本身、决策 allow|ask|deny 与 argv 规则同构、strictest wins），RuleDecider/SessionDecider 评估 web_fetch 调用的 URL host；审批浮层对 web_fetch 提供"始终允许 `<host>`"，会话记忆 + 持久化到 `remembered.json` 的 domains 节。项目层域名 allow 同样 tighten-only。builtin 集内置包管理源（npm/pypi/Go proxy/crates/maven/gradle/GitHub 含 `*.githubusercontent.com`）白名单与请求捕获/匿名粘贴类外泄渠道（webhook.site、pastebin.com 等）黑名单。未覆盖的部分留给 M3 完整版：run_cmd 沙箱内网络按域名放行（需本地代理）、端口维度。

### 8.3 接口预留
`RuleGrant.Network` 设计为 `string`（`"full"`）而非 bool，M3 扩展为 `{mode: "domains", domains: [...]}` 对象时不破坏 v2 文件（JSON 反序列化用 `json.RawMessage` 延迟解析）。

## 9. 参照实现索引

| 主题 | 位置 |
|---|---|
| codex Seatbelt profile 组装 | `codex-rs/sandboxing/src/seatbelt.rs`、`seatbelt_base_policy.sbpl` |
| codex allow→bypass_sandbox | `codex-rs/core/src/exec_policy.rs`（`Decision::Allow => Skip{bypass_sandbox}`） |
| codex 提权可记忆（prefix_rule） | `codex-rs/core/src/tools/handlers/shell_spec.rs` |
| codex 可写根保护子路径 | `codex-rs/protocol` `WritableRoot{read_only_subpaths, protected_metadata_names}` |
| Claude Code 沙箱/域名授权/逃生舱 | docs.anthropic.com/en/docs/claude-code/sandboxing |
| Lethal Trifecta | simonwillison.net/2025/Jun/16/the-lethal-trifecta |
