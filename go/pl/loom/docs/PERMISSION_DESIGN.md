# PERMISSION_DESIGN — loom 权限体系：三层信任梯度与可插拔策略

> 状态：草案 v1（M1 实现中）
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
- shell 复合命令逐段评估：loom 保持"shell 不可入规则"的设计（更安全），不引入 shell 解析器。
- 规则 DSL 更换：JSON 规则文件 + 加载时自检（match/not_match）保留，不换 execpolicy 式 DSL。

## 3. 核心概念

### 3.1 三层信任梯度

| 层 | 名称 | 审批 | 执行方式 | 产生途径 |
|---|---|---|---|---|
| L0 | sandboxed | 免审批 | Seatbelt 沙箱（禁外网、写 workspace+tmp、凭证路径不可读） | 默认状态（on-request 基线下所有非危险 `run_cmd`） |
| L1 | granted | 免审批 | 沙箱"按需开口"：按规则 `grant` 放宽网络/可写路径，其余边界不变 | 用户层规则文件，或 "allow always" 记忆 |
| L2 | trusted | 免审批 | 完全出沙箱（完整环境/网络/凭证） | 仅用户显式 opt-in，UI 标红警示 |
| — | denied | — | 不执行 | deny 规则、危险命令清单（R4 基线） |

判定优先级：**deny/危险清单 > L2/L1 规则 > 会话记忆 > L0 基线**。严格者永远胜出，任何上层不可覆盖 deny。

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
- L1：`{NetworkFull: true, WritablePaths: ["~/.talos"]}` 等
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
| 2 | `DangerDecider` | 危险命令启发式（`rm -rf /`、`dd`、fork bomb、`curl\|sh` 等）；仅作用于无规则命中时 | ask / deny |
| 3 | `SessionDecider` | 会话记忆前缀（"allow always" 产物），只能把"无意见"升级为 allow，永不覆盖文件层 | allow(L1) |
| 4 | `BaselineDecider` | 审批模式基线（§4.3），兜底永不返回 nil | allow(L0) / ask / deny |

`RuleDecider` 内部维持现行语义：trusted 目录 basename 归一化（`/bin/ls` 命中 `[ls]`）、项目层 `allow` 需 `project_allow` 显式开启、**项目层与 builtin 的规则禁止携带 grant 与 `unsandboxed`**（不可信层只能收紧，grant 也是一种放宽）。

### 4.3 BaselineDecider 与审批模式

```yaml
# ~/.loom/config.yaml
approval:
  mode: on-request   # on-request | unless-trusted | never（默认 on-request）
```

| 模式 | run_cmd 沙箱内（非危险） | 提权（R3） | R4 | 对应业界 |
|---|---|---|---|---|
| `on-request` | **allow（L0，沙箱即边界）** | ask | deny | codex OnRequest / Claude Code auto-allow |
| `unless-trusted` | ask（现行行为） | ask | deny | Claude Code default 模式 |
| `never` | allow | deny | deny | CI/无人值守 |

- 危险清单命中时任何模式都升级为 ask（never 模式为 deny）。
- 默认选 `on-request`：审批轰炸是安全反模式（§1.3），沙箱+凭证保护使默认放行代价可控（§3.3）。保守用户可显式切回 `unless-trusted`，行为与今天完全一致。
- 非 `run_cmd` 工具（edit/write 等）不受模式影响，维持现行 R 基线（blast radius 随路径变化，不适合 L0 化）。

### 4.4 装配

`internal/app/bootstrap.go` 按配置构造链：

```go
decider := permission.Chain{
    permission.NewRuleDecider(ruleSet),          // nil-safe
    permission.NewDangerDecider(dangerList),     // 内置表 + 可选用户追加
    permission.NewSessionDecider(sessionRules),  // nil-safe
    permission.NewBaselineDecider(approvalMode, riskPolicy),
}
```

每个 Decider 独立可测；新策略（如未来的"分类器 Decider"、"域名网络 Decider"）只需实现接口并插入链的合适位置。

## 5. 规则 Schema v2

```json
{
  "rules": [{
    "argv_prefix": ["/Users/liubang/.talos/bin/talos"],
    "decision": "allow",
    "justification": "talos CLI, needs network + its own config dir",
    "grant": {
      "network": "full",
      "write": ["/Users/liubang/.talos"]
    },
    "match": ["/Users/liubang/.talos/bin/talos query submit --scene SKILL"],
    "not_match": ["talos"]
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

- 新增 `verdict.go`：`Verdict`、`CapabilityGrant`、`Decider`、`Chain`。
- 新增 `decider_rule.go` / `decider_session.go` / `decider_baseline.go` / `decider_danger.go`；现行 `Policy.Evaluate` 的匹配逻辑拆入前两者，风险基线拆入 `BaselineDecider`。`Policy` 结构保留为装配参数包（AutoApproveR1/AskR2/DenyR4 → BaselineDecider 的 unless-trusted 实现）。
- `rules.go`：Rule 增加 `Grant *RuleGrant`；`validateRule` 增加 §5 校验；`LoadRuleSets` 按层剥离非法 grant。
- `danger.go`：危险命令清单（程序级：`dd`/`mkfs`/`shred`；模式级：`rm` 目标为 `/`、`~`、系统目录；管道入 shell：`curl|wget ... | sh`；git：`push --force` 到受保护分支的识别留给 M2，M1 先做程序/参数级）。清单条目含原因文案，供审批 UI 展示。
- `SessionRules`：记忆条目从 `[]string` 升级为 `{Prefix []string, Grant CapabilityGrant}`；"allow always" 记录模型在提权重试时声明的能力（§6.4 的 `needs_network`），默认 grant 而非 unsandboxed。
- `AppendRememberedRule`：写入 `remembered.json` 时携带 grant（schema v2）。

### 6.2 `internal/domain`

- 新增平台无关的 `ExecGrant` 值类型（`Unsandboxed` / `NetworkFull` / `WritablePaths`）与 `Verdict`（`Decision` + `Grant` + `Source` + `Reason`），保持依赖方向 `permission → domain`。
- **（实现偏差，已确认更优）**`PreparedCall` 直接增加 `Grant ExecGrant` 字段而非旁路 map：Grant 由 agent 主循环在 Prepare 签名之后、策略评估时赋值，刻意留在 HMAC 指纹之外——能力由策略决定，模型无法影响。`run_cmd.Execute` 直接消费 `prepared.Grant`。

### 6.3 `internal/process`（P0 安全补漏，随 M1 上）

- workspace 可写根的 `<workspace>/.git/hooks`、`<workspace>/.git/config`、`<workspace>/.loom` 保持只读。**（实现偏差）** 不用 codex 式 `require-all + require-not`，改为**尾部 deny**：`(deny file-write* (literal/subpath X))` 放在所有 allow 之后（seatbelt 最后匹配胜出），否则 workspace 位于其他可写根内（如 TMPDIR 下的测试场景）时保护会被后置的宽泛 allow 抵消。日常 `git commit` 不受影响的回归测试已建立（`git init` 因创建 hooks 样板被拦，属已知取舍，与 codex 一致）。
- `sensitiveReadDenies` 每条补 `(deny file-write-unlink ...)`，防删除/重命名探测。
- **（范围扩大）顺手修复潜伏 bug**：seatbelt 按规范化路径匹配，macOS 的 `/var`、`/tmp` 是指向 `/private` 的符号链接——旧 profile 里 `os.TempDir()` 的 `/var/folders/...` 形式**从未生效**，沙箱内 TMPDIR 写入一直静默失败。所有可写路径现在经 `canonicalWritePath`（含符号链接解析、未存在路径向上溯源）规范化。
- 执行入口：`Runner.RunWithGrant(ctx, spec, process.Grant)` 统一映射——`Unsandboxed` → DirectSandbox；零值 → 默认沙箱；其余 → `widenSandbox` 克隆 Seatbelt（网络/可写路径并集）。不支持的平台（Linux）加宽为空操作，fail-closed 保持。

### 6.3.1 Grant 覆盖检查（review C1 修复）

**v1（无 grant）的 allow 只覆盖普通沙箱调用**。当调用声明了规则未曾授予的能力（`require_escalated` 或 `needs_network`）而命中规则的 grant 为零时，RuleDecider/SessionDecider 视为无意见、落入基线：基线 ask 会盖上正确 grant（提权→`Unsandboxed`，网络→`NetworkFull`），用户批准一次即完成 v1→v2 升级（"allow always" 记忆带 grant）。不变量：**L0 信任永远不能被静默升格为 L2 出沙箱执行**；执行层对应地不存在"零 grant + 提权 → DirectSandbox"的兜底——零 grant 在任何路径下都意味着默认沙箱。非零 grant 则始终按其内容执行（网络 grant 对提权调用是合法的降级）。

### 6.4 `internal/tool/command`（run_cmd）

- 参数 schema 增加 `needs_network: bool`（可选）：模型在沙箱失败重试时**声明能力**而非直接提权。`needs_network: true` 的调用按 R2 评估（不升级为 R3），on-request 基线下仍弹一次 ask（因为基线 grant 不含网络），批准后若选 "allow always" 则记忆 `{prefix, grant:{network:full}}`。
- 工具描述与 `sandboxGuidanceNote` 改写：引导顺序从"失败 → require_escalated"改为"网络类失败 → needs_network 重试；TTY/凭证类失败 → require_escalated"。
- 执行处：消费 `ExecGrant`——`Unsandboxed` → `DirectSandbox{}`（现提权路径）；否则 Seatbelt + `AllowNetwork=grant.NetworkFull` + `WritablePaths+=grant.WritablePaths`；输出 `isolation` 字段如实标注（`seatbelt+net`、`seatbelt`、`direct`）。
- `require_escalated` 保留（L2 路径），但不再被 `RunCmdArgv` 硬排除：提权调用可命中带 `unsandboxed` grant 的用户层规则（L2 豁免）——这是 skill 场景的兜底逃生门，默认不产生、需用户显式批准。

### 6.5 `internal/app` + `internal/ui`

- 审批选项从两项扩展（run_cmd 且可推导 grant 时；**已与用户确认两种记忆口味都给，grant 标为推荐**）：
  1. `允许一次`
  2. `始终允许（最小能力）`——默认推荐；记住 `{prefix, grant}`，grant 来源：模型 `needs_network` 声明，或提权调用默认推导为网络 grant（M1 的最常见原因近似），UI 展示推导结果
  3. `完全信任（出沙箱）`——仅提权调用出现的第四行，记住 `{prefix, unsandboxed}`
- `RuleApprover.RememberRunCmd` 升级为记住 `{prefix, grant}`；`RunCmdRulePreview` 展示 grant 摘要（如 `talos (+网络, +写~/.talos)`）。
- 审批 UI 的 grant 展示即"选择架构"：推荐项默认高亮，出沙箱项需额外确认。

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

## 8. 里程碑

### M1（本次实现）
三层信任梯度全链路：Decider 抽象 + 四种内置实现、规则 schema v2 + grant、on-request 基线、危险清单 v1、P0 写保护（.git/.loom + unlink）、run_cmd `needs_network`、审批三选项 + grant 记忆/持久化、`loom rules check` 输出 Verdict。

### M2
危险清单细化（git 写操作、环境窃取模式）；脚本内容哈希绑定评估；`grant.write` 的自动路径推断（从沙箱拒绝日志反推）。

### M3
域名级网络授权：本地代理 + 按域名审批记忆（`grant.network: {domains: [...]}` 的 schema 预留形状）；Linux bubblewrap 沙箱。

**（M3-lite 已提前落地，2026-07-30）** web_fetch 的域名级记忆：规则文件新增 `"domains"` 节（`host` 精确匹配、不含端口/子域、决策 allow|ask|deny 与 argv 规则同构），RuleDecider/SessionDecider 评估 web_fetch 调用的 URL host；审批浮层对 web_fetch 提供"始终允许 `<host>`"，会话记忆 + 持久化到 `remembered.json` 的 domains 节。项目层域名 allow 同样 tighten-only。未覆盖的部分留给 M3 完整版：run_cmd 沙箱内网络按域名放行（需本地代理）、子域/通配匹配、端口维度。

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
