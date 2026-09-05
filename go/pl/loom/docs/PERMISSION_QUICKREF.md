# 审批模式速查（Approval Mode Quick Reference）

> 定位：permission 系统的**行为速查卡**——什么会弹审批、什么会被直接拒绝、什么自动放行。完整设计与判定链见 [PERMISSION_DESIGN.md](PERMISSION_DESIGN.md)（§4.3 审批基线、§7 安全分析）。代码入口：`internal/permission/decide.go`（决策链）、`internal/permission/semcmd_*.go`（语义推导）、`internal/permission/builtin.json`（内置域名规则）。

---

## 1. 三种审批模式

来源：`~/.loom/config.yaml` 的 `approval.mode`，或 WebUI Composer 的审批快捷切换（**工作区级覆盖，下一轮生效，不写入配置**）。

| 模式 | 配置值 | UI 短名 | 一句话语义 |
|---|---|---|---|
| 标准（默认） | `on-request` | standard | 沙箱内/工作区内读写免审批；越界（网络、额外写、GUI、提权）与危险形状逐次询问 |
| 开发模式 | `danger-only` | dev | 无危险信号的越界能力**全部按声明授权放行**；仅 deny 规则、危险指标、破坏性/共享状态后果仍弹审批 |
| 无人值守 | `never` | auto | 危险形状**直接拒绝**（带绕行指引），永不产生 ask、永不等待 |

> `never` 不产生 ask；长程无人任务不会死挂在提示上。危险形状在交互模式下是 ask，在 `never` 下是 deny。

---

## 2. 判定顺序（每次工具调用走同一条链）

见 `internal/permission/decide.go` 的 `Decide`。命中即返回，前面的分支优先：

1. **deny 绑定**（内置/用户/项目/记忆，任何层）→ **直接拒绝**（所有模式一致）
2. **显式 ask 绑定**（规则文件 `decision: ask`）→ **弹审批**（所有交互模式一致）
3. 用户在本轮对话提到的主机且**无危险指标** → 放行（`never` 不适用）
4. **危险指标闸**：命中危险指标的形状，只能被**同参数的精确绑定**（exact binding）豁免 → 否则弹审批 / `never` 拒绝
   - `danger-only` 唯一例外：过滤掉 browser 的 real-identity 指标（正常浏览是合法工作）
5. **强制 ask 残余**（非只读第三方 MCP 工具、配额消耗者）→ `on-request` 弹审批；**`danger-only` 自动放行**；`never` 拒绝
6. 类别允许包 / 默认沙箱覆盖 → 放行
7. **模式残余兜底**：`danger-only` 只对后果级别 > confined（破坏性/共享状态）弹审批，其余越界全部按声明授权放行

---

## 3. danger-only 仍会弹审批（ask）

### 3.1 命中显式 ask 规则

- 用户/项目/内置规则文件里写了 `decision: ask` 的形状——任何模式都弹，优先于一切放行。

### 3.2 命中危险指标（危险清单）

危险指标形状**永不静默放行、不可被类别规则覆盖**；只接受同参数精确绑定豁免。常见形状：

| 类别 | 典型形状 | 指标含义 |
|---|---|---|
| 解释器 stdin 代码 | `python3 - <<PY`、`node -`、`sh -s`、here-string | 程序文本经 stdin/heredoc 喂解释器，代码无法静态审查（`OpaquePayload`） |
| 网络内容管道进解释器 | `curl x | sh`、`wget -qO- … | python3` | 远程代码执行模式（RCE） |
| 敏感/持久化路径写入 | `.zshrc`/`.bashrc`/`.gitconfig`/`.profile` 等启动与全局配置、凭证路径（`.ssh`/`.aws`/`id_rsa`/`.git-credentials`/`.netrc`）、git hooks/config（含 submodule/worktree）、`.loom` 元数据、关键根 `/` | 写持久化/提权载体，逃逸沙箱后仍生效 |
| 提权 | `sudo` / `su` / `doas`（任意参数） | 越出所有用户级边界（同时是 local-destructive） |
| macOS 自动化/持久化 | `osascript`（Apple Events）、`launchctl`、`crontab`（编辑；`crontab -l` 除外） | 驱动其他应用 / 安装持久后台服务 |
| 容器/集群逃逸 | `docker run/exec/create` + `-v`/`--privileged`/`--network=host`；`kubectl exec/cp/port-forward/attach/proxy` | 容器边界溶解 / 集群内执行与数据搬运（payload 不可分析） |
| git 配置注入 | `git -c core.sshCommand=…` 等敏感注入、`core.hooksPath` 重定向 | 注入配置即持久化/代码执行 |
| 凭证外泄形状 | `curl/wget/ssh …` argv 携带凭证路径（如 `curl -d @~/.ssh/…`） | 外泄尝试，绝不静默放行 |

### 3.3 破坏性 / 共享状态后果（残余兜底）

后果级别 > confined（local-destructive / shared-state / shared-destructive）且无 allow 包覆盖时弹审批。典型：

| 后果级别 | 典型命令 |
|---|---|
| 共享状态（他人可观察的变更） | `git push`、`npm/pnpm/yarn publish`、`docker push`、`kubectl apply/create/scale/rollout/patch/replace/edit/label/annotate/expose/autoscale` |
| 共享破坏（改写/销毁共享状态） | `git push --force/-f/--delete/--mirror`、`kubectl delete/drain/cordon/taint` |
| 本地破坏（不可逆本地销毁） | `rm` 指向关键根（`/`、`~`、`/etc` 等）或 `-r` 逃逸 cwd；`chmod/chown` 关键根；`dd`/`mkfs`/`shred`/`fdisk`/`diskutil`/`newfs_*`/`hdiutil`（任何目标）；`find -delete`、`find -exec rm …`、`xargs rm/shred/dd`；`git reset --hard`/`git clean -f` |

> 例外：上述命令若命中**用户级 allow 规则**或**"始终允许"记忆**（且授权上限足够覆盖后果），会先放行不弹。

---

## 4. 直接拒绝（deny，无审批选项）

- **内置外泄站点黑名单**（`internal/permission/builtin.json` 的 deny host）：`webhook.site`、`requestbin.com`（含 `*.`）、`pipedream.com`（含 `*.`）、`pastebin.com`、`paste.ee`、`hastebin.com`、`transfer.sh`、`0x0.st`、`bashupload.com`、`file.io`——请求捕获/匿名粘贴/匿名分享类外泄通道。
- **用户/项目层 deny 规则**（argv / 域名 / 路径均可）：deny **永远赢**，覆盖任何 allow 记忆，所有模式一致。
- deny ≠ ask：没有审批弹窗，工具直接被拒。

---

## 5. danger-only 自动放行（不弹审批）

- **工作区内命令**：`go test`、`make`、`npm install`（自动放网）、`git add/commit`、`rm -rf build`（可重建 → confined）等；
- **越界能力按声明授权**：`needs_network`、越界写 `writable_paths`、GUI `open`、`require_escalated` 出沙箱——只要**无危险信号且后果非破坏性**；
- **第三方 MCP 工具 / 配额消耗者**（强制 ask 残余在 danger-only 下自动放行）；
- **`web_fetch` 匿名 GET**（无凭证；SSRF 防护默认拦截私有/回环/链路本地目标）；
- **`browser` 真实身份访问**（real-identity 指标被 danger-only 过滤；deny 域名仍优先拒绝）；
- **已批准过的同参数精确命令**（exact binding）→ 静默放行。

---

## 6. 常见问题

**Q：开发模式下 `python3 - <<PY`（heredoc 脚本）为什么还弹审批？**
A：这是"解释器 stdin 代码"危险指标（`OpaquePayload`，内容不可静态分析，RCE 经典形状）。`danger-only` 只过滤 browser 的 real-identity 指标，其余指标一律不静默放行；且此类调用**不允许记成常驻类别规则**（防不同 payload 误命中），只能逐次/按精确参数批准。

**Q：想让这类脚本免弹，怎么改？**
A：把代码写成 `.py` 文件再 `python3 xxx.py` 执行——脚本文件形态可静态证明、工作区内直接放行。

**Q：某条命令到底会怎么判？**
A：`loom rules check '<命令>'` 输出最终 Verdict（decision + grant + 命中规则来源）；`loom rules check --url <url>` 查域名。

**Q：切了开发模式为什么当前这轮还弹？**
A：策略在每次 run 构造时捕获，工作区级切换**下一轮生效**（UI 提示"下一轮生效，不写入配置"）。

---

## 7. 代码位置索引

| 主题 | 位置 |
|---|---|
| 决策链 / 模式残余 | `internal/permission/decide.go`（`Decide`、`residualVerdict`、`withoutBenignIndicators`） |
| 危险指标定义 | `internal/permission/indicators.go`、`semcmd_*.go` |
| 命令语义推导 | `internal/permission/semcmd.go`、`semcmd_file.go`、`semcmd_git.go`、`semcmd_interp.go`、`semcmd_net.go` |
| 内置域名规则（白/黑名单） | `internal/permission/builtin.json` |
| 后果分级 | `internal/permission/effect.go`（`Consequence`） |
| UI 模式选择 | `internal/server/webui/src/components/Composer.tsx`（`APPROVAL_OPTIONS`） |
