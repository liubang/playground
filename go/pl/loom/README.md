# Loom

面向软件工程任务的生产级 Coding Agent。Loom 以大语言模型作为决策器，通过受控工具读取和修改代码、执行命令、验证结果，并在有限预算内迭代，直到任务完成、请求用户介入或安全终止。

Loom 不是模型 API 的命令行包装器，而是一个 Agent Harness：模型无关的消息/工具协议、可恢复可取消的 Agent 状态机、安全可审计的工具运行时，以及事件溯源的会话持久化。

## 特性

- **交互式 TUI**：流式输出、工具调用块、diff 预览、审批弹窗、会话恢复与历史浏览（Bubble Tea）。
- **Headless 模式**：`loom run` 单次执行，最终回答写 stdout、诊断写 stderr；非 TTY 下审批自动拒绝（fail-closed），适合管道与脚本。
- **事件溯源会话**：所有会话事件与检查点持久化于 SQLite，支持崩溃恢复、跨进程 `resume`、完整历史回放。
- **安全基座**：工作区路径边界、命令沙箱（macOS sandbox-exec/Seatbelt；Linux 沙箱尚未实现，fail-closed 拒绝执行）、环境变量白名单、R0–R4 风险分级与交互审批，"allow always" 生成类别化命令前缀规则。
- **丰富的内置工具**：`read_file` / `list_dir` / `grep` / `glob` / `edit` / `write` / `run_cmd` / `exec_session` / `write_stdin`（交互式后台进程会话）/ `git_status` / `git_diff` / `git_log` / `lint` / `web_fetch` / `web_search` / `update_goal` / `update_plan` / `read_skill`。
- **Skills**：从工作区与用户目录发现 `SKILL.md` 技能，清单注入系统提示词，正文按需渐进披露。
- **上下文工程**：上下文窗口占用感知、自动压缩（掩码 + 摘要交接）、预算提醒。
- **可观测性**：OpenTelemetry / Langfuse 追踪（默认关闭），Context Manifest 审计每次模型请求的上下文构成。

## 快速开始

### 构建

```bash
# Go（Go 1.26+）
cd go && go build ./pl/loom/cmd/loom

# 或 Bazel
bazel build //go/pl/loom/...
```

### 安装与打包（Bazel）

```bash
# CLI：装入 GOBIN（$GOBIN > go env GOBIN > GOPATH/bin > ~/go/bin）
bazel run //go/pl/loom/cmd/loom:install

# WebUI（浏览器模式）：前端已以 embed.FS 内嵌进二进制，装完 CLI 即可
loom serve                     # 默认 127.0.0.1:7680
loom serve --listen <addr>     # 换地址/端口

# 桌面应用（Wails，macOS）
bazel run //go/pl/loom/cmd/loom-desktop:install          # 一键装入 /Applications/Loom.app 并稳定签名
bazel run //go/pl/loom/cmd/loom-desktop:package_app      # 产物落 dist/Loom.app（检查/调试）
bazel run //go/pl/loom/cmd/loom-desktop:package_release  # dist/ 下产出 macOS 双架构 DMG + Linux CLI deb
bazel run //go/pl/loom/cmd/loom-desktop:make-signing-cert  # 创建本地稳定签名身份（幂等，一台机器一次；首次 install 也会自动补）

open /Applications/Loom.app    # 安装后手动启动（不要用终端直接跑 bundle 内二进制）
```

要点：

- **`--config=desktop` 已不再是必需**：`production` build tag 与 `--stamp` 已提为 `.bazelrc` 全局默认（2026-09）；`build:desktop` 保留为空转别名，老命令（CI/脚本）不受影响。
- loom-desktop 整个包带 `tags = ["manual"]`，不进 `bazel build //go/...` 通配构建（CI 有独立 desktop job）。
- **改了 WebUI 前端源码后**，须先重建前端再编 Go 侧（非 hermetic，需 node>=22 + pnpm；产物 `web/dist` 提交入库，由 embed.FS 消费）：

```bash
bazel run //go/pl/loom/internal/server/webui:build       # 等价于 pnpm build
bazel build //go/pl/loom/...
```

- **单实例锁**：`loom serve` 与 Loom.app 共享数据目录 flock，同一 loom home 同时只能跑一个；install 会杀掉 `/Applications` 下正在运行的旧实例（不影响 `bazel run`/`dist/` 调试进程）。
- 安装/签名相关环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| `LOOM_INSTALL_DIR` | `/Applications` | `:install` 的目标目录 |
| `LOOM_SIGN_IDENTITY` | 自动发现/创建 | 显式指定 codesigning 身份 |
| `LOOM_CERT_CN` | `Loom Dev (liubang)` | 自签身份 CN；材料存 `~/.config/loom/codesign/` |

### 配置

```bash
loom                              # 首次运行自动生成 ~/.loom/config.yaml 模板
$EDITOR ~/.loom/config.yaml       # 填入 api_key（或 api_key_env）与 base_url
```

### 使用

```bash
loom                              # TTY 下进入交互式 TUI
loom chat --resume <session-id>   # 恢复会话
loom run "修复 ./pkg 下的编译错误"   # 单次执行（headless）
loom resume <session-id> "继续"     # 在已有会话上继续执行
loom sessions                     # 列出会话
loom inspect <session-id>         # 导出会话完整事件/检查点（JSON）
loom gc                           # 回收无引用的 artifact
```

### 主要环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `LOOM_HOME` | `~/.loom` | loom home（数据根目录）：配置文件 `<loom home>/config.yaml`、会话库、日志、记忆、规则、技能均派生其下 |
| `LOOM_LANGFUSE_PUBLIC_KEY` / `LOOM_LANGFUSE_SECRET_KEY` | — | Langfuse 追踪密钥，经 config.yaml 的 `public_key_env` / `secret_key_env` 引用（默认关闭） |
| `LOOM_WEB_SEARCH_PROVIDER` | 自动探测 | web_search 工具后端：`brave` / `tavily` / `ddg`（需对应 API key；未显式指定时按已配置 key 探测，再否则无 key DuckDuckGo） |

其余配置（`base_url`、`wire_api`、`api_key` / `api_key_env`、`context_window` 等）均在 `~/.loom/config.yaml` 中；无环境变量覆盖层。

## 架构

```text
Frontend（TUI / loom serve + Web）
    │  命令 + 事件订阅
Application（app.Controller / Bootstrap）
    │
Agent Runtime（状态机 / Loop / 预算 / 压缩）── Session & Event Store（SQLite 事件溯源 + checkpoint + artifact）
    │
Model Provider（OpenAI 兼容）   Tool Runtime（registry / policy / approval / sandbox）
```

核心包导读（`internal/`）：

| 包 | 职责 |
|------|------|
| `domain` | 稳定领域类型、错误与接口（不依赖任何实现） |
| `agent` | Agent 状态机、主循环、计划/目标、上下文压缩、工具注册表 |
| `app` | 装配根（Bootstrap）、无头会话控制器（Controller）、审批桥 |
| `runtimeevent` | 前端事件协议：版本化 envelope、Broker 非阻塞扇出 |
| `session` | SQLite 事件存储、检查点、transcript 投影 |
| `permission` | 风险分级策略、声明式规则、会话记忆规则 |
| `process` | 进程执行、沙箱、进程组清理 |
| `tool/*` | 内置工具实现 |
| `ui` | Bubble Tea TUI |
| `trace` | OTel/Langfuse 观测 |

## 文档

设计文档统一存放在 [`docs/`](docs/)：

| 文档 | 内容 |
|------|------|
| [docs/DESIGN.md](docs/DESIGN.md) | 总体设计：领域模型、协议、安全、持久化、Daemon（主文档） |
| [docs/TUI_DESIGN.md](docs/TUI_DESIGN.md) | TUI 前端设计 |
| [docs/PLAN_DESIGN.md](docs/PLAN_DESIGN.md) | 计划（Plan）子系统设计 |
| [docs/SKILL_DESIGN.md](docs/SKILL_DESIGN.md) | Skills 发现、注入与读取设计 |
| [docs/SERVE_DESIGN.md](docs/SERVE_DESIGN.md) | Server 模式设计（HTTP/SSE API + Web 纯渲染前端，已实现） |

## 开发

```bash
# 测试
go test ./pl/loom/...            # 在 go/ 目录下
bazel test //go/pl/loom/...      # 或 Bazel

# 端到端（scripted FakeModel 驱动真实 agent loop）
go test ./pl/loom/e2e/...
```

代码风格：Go 标准 `gofmt`；所有源文件携带 Apache-2.0 许可证头。

## License

Apache License 2.0
