# Loom

面向软件工程任务的生产级 Coding Agent。Loom 以大语言模型作为决策器，通过受控工具读取和修改代码、执行命令、验证结果，并在有限预算内迭代，直到任务完成、请求用户介入或安全终止。

Loom 不是模型 API 的命令行包装器，而是一个 Agent Harness：模型无关的消息/工具协议、可恢复可取消的 Agent 状态机、安全可审计的工具运行时，以及事件溯源的会话持久化。

## 特性

- **交互式 TUI**：流式输出、工具调用块、diff 预览、审批弹窗、会话恢复与历史浏览（Bubble Tea）。
- **Headless 模式**：`loom run` 单次执行，最终回答写 stdout、诊断写 stderr；非 TTY 下审批自动拒绝（fail-closed），适合管道与脚本。
- **事件溯源会话**：所有会话事件与检查点持久化于 SQLite，支持崩溃恢复、跨进程 `resume`、完整历史回放。
- **安全基座**：工作区路径边界、命令沙箱（macOS sandbox-exec/Seatbelt；Linux 沙箱尚未实现，fail-closed 拒绝执行）、环境变量白名单、R0–R4 风险分级与交互审批，"allow always" 生成类别化命令前缀规则。
- **丰富的内置工具**：`read_file` / `list_dir` / `search` / `glob` / `edit` / `write` / `run_cmd` / `git_status` / `git_diff` / `git_log` / `lint` / `web_fetch` / `update_goal` / `update_plan` / `read_skill`。
- **Skills**：从工作区与用户目录发现 `SKILL.md` 技能，清单注入系统提示词，正文按需渐进披露。
- **上下文工程**：上下文窗口占用感知、自动压缩（掩码 + 摘要交接）、预算提醒。
- **可观测性**：OpenTelemetry / Langfuse 追踪（默认关闭），Context Manifest 审计每次模型请求的上下文构成。

## 快速开始

### 构建

```bash
# Go（Go 1.25+）
cd go && go build ./pl/loom/cmd/loom

# 或 Bazel
bazel build //go/pl/loom/...
```

### 配置

```bash
export LOOM_API_KEY=sk-...
export LOOM_BASE_URL=https://api.openai.com/v1   # 任意 OpenAI 兼容端点
export LOOM_MODEL=gpt-4o
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
| `LOOM_MODEL` | `gpt-4o`（chat） | 模型标识；`loom run` 必填 |
| `LOOM_BASE_URL` / `LOOM_API_KEY` | — | OpenAI 兼容端点与密钥 |
| `LOOM_WIRE_API` | `chat_completions` | 线协议：`chat_completions` 或 `responses` |
| `LOOM_CONTEXT_WINDOW` | 0（自动） | 模型上下文窗口 token 数，驱动压缩 |
| `LOOM_SESSION_DB` | 平台状态目录 | 会话数据库路径 |
| `LOOM_SYSTEM_PROMPT_EXTRA` | — | 追加到系统提示词的自定义指令 |
| `LOOM_SKILLS` / `LOOM_SKILLS_EXTRA_ROOTS` | 开 / — | 技能开关与额外发现根目录 |
| `LOOM_RULES` | 开 | 用户/项目声明式权限规则（`.loom/rules/*.json`） |
| `LOOM_LANGFUSE_*` | 关 | Langfuse 追踪（详见 `docs/DESIGN.md` §33） |

## 架构

```text
Frontend（TUI；未来: loom serve + Web）
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
| [docs/SERVE_DESIGN.md](docs/SERVE_DESIGN.md) | Server 模式设计（HTTP/SSE API + Web 纯渲染前端，进行中） |

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
