// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/07/26

package config

import (
	"fmt"
	"os"
	"path/filepath"
)

// FileName is the config file name within the loom data directory.
const FileName = "config.yaml"

// WriteTemplate writes the annotated starter config (0600) at path,
// creating its parent directory (0700) when missing. An existing file is
// never overwritten — the user edits it by hand or removes it first.
func WriteTemplate(path string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}
	if _, err := os.Stat(path); err == nil {
		return fmt.Errorf("config file already exists: %s (edit it directly, or remove it and re-run init)", path)
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("inspect config file: %w", err)
	}
	if err := os.WriteFile(path, []byte(template), 0o600); err != nil {
		return fmt.Errorf("write config file: %w", err)
	}
	return nil
}

// IsDefaultConfigPath reports whether path is the default config location
// (~/.loom/config.yaml). First-run bootstrap applies only there: a missing
// explicit LOOM_CONFIG path names a file that should exist — a user error
// to surface, never a state to paper over with a generated file.
func IsDefaultConfigPath(path string) (bool, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return false, fmt.Errorf("config: resolve config path: %w", err)
	}
	base, err := DefaultBaseDir()
	if err != nil {
		return false, err
	}
	return abs == filepath.Join(base, FileName), nil
}

// EnsureFirstRunConfig writes the starter template at the default config
// path when the file is missing, creating the parent directory (0700) and
// the file (0600). It never touches an explicit LOOM_CONFIG path and never
// overwrites an existing file. When the default location cannot be
// resolved (e.g. HOME is unset) the path is treated as non-default: no
// file is created and the caller keeps its original error. Returns
// created=true when a fresh template was written so the caller can route
// the first-run experience: the CLI prints the path and exits non-zero,
// the desktop keeps booting into the settings UI where the API key is
// collected.
func EnsureFirstRunConfig(path string) (bool, error) {
	ok, err := IsDefaultConfigPath(path)
	if err != nil || !ok {
		return false, nil
	}
	if _, err := os.Stat(path); err == nil {
		return false, nil
	} else if !os.IsNotExist(err) {
		return false, fmt.Errorf("inspect config file: %w", err)
	}
	if err := WriteTemplate(path); err != nil {
		return false, err
	}
	return true, nil
}

// minimalExample is embedded in fail-fast error messages so a user without
// any config can copy-paste their way to a working setup (§9). The key
// placeholder is intentionally non-secret-looking: it must be replaced.
const minimalExample = `  default: deepseek/deepseek-chat
  providers:
    - name: deepseek
      type: openai
      base_url: https://api.deepseek.com/v1
      api_key: <your-api-key>       # 必填：换成你的真实密钥，或用 api_key_env: DEEPSEEK_API_KEY
      models:
        - name: deepseek-chat
          context_window: 65536`

// template is the annotated starter config written by `loom config init`.
// Every key carries its own comment: the file doubles as the user-facing
// configuration reference. Keep in sync with schema.go.
const template = `# ==============================================================================
# loom 配置文件
#
# 位置: ~/.loom/config.yaml（loom 唯一配置来源，LOOM_CONFIG 可指定其他路径）
# 说明: 除 providers 外所有节均可省略，省略时取内置默认值（各项注释中标注）。
#       保存后重启 loom 生效（不支持热加载）。含明文密钥时建议 chmod 600。
# ==============================================================================

# 启动时默认使用的模型，三种写法：
#   provider/model   —— 精确指定（推荐，无歧义）
#   裸模型名          —— 所有 provider 的模型目录中必须唯一匹配，否则报错
#   裸 provider 名    —— 取该 provider 的 default_model
# 省略时取 providers 列表第一个 provider 的默认模型。
default: deepseek/deepseek-chat

# ------------------------------------------------------------------------------
# 模型提供方（必填节，至少一个）。可配置多个 provider，运行中用 /model 切换：
#   /model                     查看当前 provider/model
#   /model deepseek-reasoner   按裸模型名切换（全局唯一匹配）
#   /model openai/gpt-5        精确切换
# ------------------------------------------------------------------------------
providers:
  - name: deepseek              # provider 名（全局唯一，/model 引用用）
    type: openai                # 协议类型：openai（OpenAI 兼容网关，默认）| anthropic
                                # （Messages API）。deepseek/豆包/GLM/vLLM 等兼容
                                # 网关都用 openai；Claude 官方端点用 anthropic。
    base_url: https://api.deepseek.com/v1
                                # 必填。与所选协议类型匹配的端点。
    # API 密钥，必填。二选一（互斥，同时填写会报错）：
    #   api_key: <your-api-key>           # 直接明文书写，把 <your-api-key> 换成真实密钥
    #   api_key_env: DEEPSEEK_API_KEY     # 或只存环境变量名，启动时读取值；适合 CI/入仓
    wire_api: chat              # 请求协议：openai 类型下为 chat（Chat Completions）|
                                # responses（Responses API），省略默认 chat；anthropic
                                # 类型下只有 messages（可省略）。可被单个模型覆盖。
    # api_version: ""           # 协议版本头（仅 anthropic 类型的 anthropic-version；
                                # 省略取实现内置的固定版本）。
    # auth_type: x-api-key      # 认证头方式（仅 anthropic 类型）：x-api-key（默认，
                                # 官方端点）| bearer（Authorization: Bearer，Claude Code
                                # OAuth 与多数 Anthropic 协议网关用的方式）。
    max_retries: 2              # 请求失败重试次数。省略默认 2。
    default_model: deepseek-chat
                                # 该 provider 的默认模型。省略取 models 列表第一个；
                                # 填写时必须是 models 中已声明的名字。
    # reasoning:                # provider 级默认推理（thinking）意图，可被单个模型覆盖：
    #   effort: medium          #   off | low | medium | high（省略由 provider 决定）
    #   budget_tokens: 0        #   显式推理 token 预算，>0 时优先于 effort 推导值
    models:                     # 可选模型目录（至少一个；/model 的可选范围）
      - name: deepseek-chat
        context_window: 65536   # 模型上下文窗口（token 数）。用于状态栏用量占比
                                # 与上下文压缩阈值；0 表示回退到 limits.max_input_tokens。
        max_output_tokens: 8192 # 单次请求的最大输出 token（模型能力上限，可选）。
                                # 与 limits.max_output_tokens（运行预算护栏）取较小值。
      - name: deepseek-reasoner
        context_window: 65536
        wire_api: responses     # 模型级协议覆盖：该模型走 Responses API

  # 第二个 provider 示例（取消注释并填写密钥即可启用）：
  # - name: openai
  #   type: openai
  #   base_url: https://api.openai.com/v1
  #   api_key_env: OPENAI_API_KEY
  #   models:
  #     - name: gpt-5
  #       context_window: 400000

  # Anthropic（Claude）provider 示例：
  # - name: anthropic
  #   type: anthropic
  #   base_url: https://api.anthropic.com
  #   api_key_env: ANTHROPIC_API_KEY
  #   # auth_type: bearer       # 走 Anthropic 协议网关时改用 bearer
  #   models:
  #     - name: claude-sonnet-4-6
  #       context_window: 200000
  #       max_output_tokens: 64000
  #       reasoning:            # 开启扩展思考：带签名回传，tool use 续轮不断链
  #         effort: high

# ------------------------------------------------------------------------------
# 运行预算（护栏；超限先进入软着陆收尾回合，再中止当前 run）。
# 只计量稀缺资源本身（会话级累计 token、成本），全部可选，0 表示不限制。
# ------------------------------------------------------------------------------
limits:
  max_input_tokens: 200000      # 模型未声明 context_window 时的回退窗口（默认 200000）
  max_output_tokens: 16384      # 单次请求的输出 token 上限（默认 16384）
  max_cost_usd: 5.0             # 会话级累计估算成本上限 USD（默认 5.0；需配置 tracing 成本费率）
  max_tokens: 0                 # 会话级累计 token 预算（默认 0 = 不限，opt-in）
  max_tool_output_bytes: 49152  # 单条工具结果保留在对话中的最大字节，
                                # 超出部分头尾截断并转存 artifact（默认 48KB）
  max_artifact_bytes: 104857600 # 单个 artifact 最大字节（默认 100MB）

# ------------------------------------------------------------------------------
# 上下文压缩（阈值均为模型有效窗口的比例，随模型自动伸缩）
# ------------------------------------------------------------------------------
context:
  utilization: 0.95             # 有效窗口 = context_window × utilization（默认 0.95）
  compact_trigger_ratio: 0.80   # 自动压缩触发线（相对有效窗口，默认 0.80）
  compact_target_ratio: 0.50    # 压缩目标（相对有效窗口，默认 0.50，必须 < trigger）
  notice_levels: [0.60, 0.75]   # 梯度占用提醒档位（升序且全部 < trigger）

# ------------------------------------------------------------------------------
# 失控行为检测（非预算：检测死循环/停滞，不限制工作量）
# ------------------------------------------------------------------------------
runaway:
  max_repeated_calls: 3         # 同一 (工具, 参数) 连续重复上限（默认 3，第 2 次警告）
  max_consecutive_failures: 5   # 执行期连续失败上限（默认 5）
  stall_warn_turns: 10          # 连续无进展回合数达到即注入收敛提醒（默认 10，0 关闭）
  stall_timeout: 15m            # 停滞看门狗：距上次进展的活跃时长上限（默认 15m，0 关闭；
                                # 审批等待与用户输入时间不计入）

# ------------------------------------------------------------------------------
# 系统提示词
# ------------------------------------------------------------------------------
prompt:
  # 追加到内置系统提示词末尾的自定义指令（多行文本）。
  extra: |
    Always reply in Chinese.
  disable_builtin: false        # true 则完全禁用内置系统提示词（默认 false）
  managed:                      # Langfuse 托管提示词（需同时配置 tracing）
    name: ""                    # 提示词名称；空表示不使用托管提示词
    label: production           # 发布标签（默认 production）

# ------------------------------------------------------------------------------
# Skills（技能发现与注入）
# ------------------------------------------------------------------------------
skills:
  enabled: true                 # false 关闭技能加载与 read_skill 工具（默认 true）
  extra_roots: []               # 额外的技能搜索目录（列表），支持 ~/ 前缀，如 ["~/skills", "/Users/me/skills"]

# ------------------------------------------------------------------------------
# 开发工具链：loom 启动时会把常见工具目录（mise/homebrew/cargo 等）补进
# 进程 PATH（GUI 启动时 PATH 只有系统目录）；path_extra 登记内置清单之外的
# 目录，优先级高于所有内置候选。支持 ~/ 前缀，必须是绝对路径；保存即热应用。
# ------------------------------------------------------------------------------
tools:
  path_extra: []                # 如 ["~/corp/bin", "/opt/custom/toolchain/bin"]

# ------------------------------------------------------------------------------
# 权限规则（声明式规则：argv 前缀规则作用于 run_cmd，domains 节作用于
# web_fetch，paths 节作用于工作区外写 —— 如 {"paths": [{"path": "~/notes",
# "decision": "allow"}]}，子路径语义，敏感目录的规则会被拒绝）
# ------------------------------------------------------------------------------
rules:
  enabled: true                 # false 关闭全部规则加载含内置集（默认 true）
  builtin: true                 # false 只关闭内置只读命令集（默认 true）
  project: true                 # false 不加载项目层 <workspace>/.loom/rules（默认 true）
  project_allow: false          # true 允许项目层规则包含 allow（默认 false：
                                # 不可信的仓库只能收紧策略，不能放宽）
  persist_remembered: true      # true 将"始终允许"的命令前缀写入用户层规则文件，
                                # 供后续会话继承（默认 true）

# ------------------------------------------------------------------------------
# 审批基线（docs/PERMISSION_DESIGN.md §4.3）：无规则/记忆命中时如何决策
# ------------------------------------------------------------------------------
approval:
  mode: on-request              # on-request（默认：沙箱内命令、工作区内读写免审批；
                                #   仅出沙箱提权/网络放宽/危险清单弹审批）
                                # unless-dangerous（黑名单模式：仅危险清单和出沙箱提权弹审批，
                                #   needs_network 直接放网）
                                # never（无人值守：沙箱内放行，提权与危险命令直接拒绝，
                                #   永不阻塞等待审批）

# ------------------------------------------------------------------------------
# Langfuse 追踪（可观测性）。host 与两个 key 都填写时才启用，否则整体关闭。
# ------------------------------------------------------------------------------
tracing:
  host: ""                      # Langfuse 地址，如 https://langfuse.internal
  public_key: ""                # 项目公钥（也支持 public_key_env 引用，互斥）
  # public_key_env: LOOM_LANGFUSE_PUBLIC_KEY
  secret_key: ""                # 项目密钥（也支持 secret_key_env 引用，互斥）
  # secret_key_env: LOOM_LANGFUSE_SECRET_KEY
  environment: dev              # 追踪环境标签（默认 dev）
  include_content: true         # false 则追踪中只保留结构摘要，
                                # 不上送对话原文（默认 true）
  user: ""                      # 追踪归属用户；空则依次取 git user.email、$USER
  cost_input_usd_per_mtok: 0    # 输入成本费率（USD/百万 token，>0 才统计成本）
  cost_output_usd_per_mtok: 0   # 输出成本费率（USD/百万 token）

# ------------------------------------------------------------------------------
# 局域网分享（loom-desktop，docs/DESKTOP_DESIGN.md §5）。分享 token 持久化在
# 会话库中，固定端口使链接跨重启存活；监听只暴露 /share 只读页面，不带
# 管理 API。设置页与分享按钮的开关写穿到 enabled 并即时生效（热应用）
# ------------------------------------------------------------------------------
share:
  enabled: false                # true = 启动即开启局域网分享监听（默认 false）
  listen: 0.0.0.0:7681          # 监听地址：0.0.0.0 = 所有接口；也可绑特定接口
                                # IP（如 tailnet 地址）；端口须固定（1-65535）

# ------------------------------------------------------------------------------
# 文件日志配额（loom home/logs 下的 glog 风格按日文件；
# loom home = 本配置文件所在目录，其下派生 sessions/ logs/ memories/
# rules/ skills/ 与全局规则文件 LOOM.md）
# ------------------------------------------------------------------------------
logging:
  max_file_mb: 0              # 单个日志文件上限 MiB，超过后轮转为同日序号文件
                              # （默认 0 = 内置 2048；负数关闭该限制）
  max_total_mb: 0             # 日志目录总量上限 MiB，超过后从最旧文件开始回收
                              # （默认 0 = 内置 10240；负数关闭该限制）

# ------------------------------------------------------------------------------
# 终端界面
# ------------------------------------------------------------------------------
ui:
  icons: nerd                   # 图标集：nerd（Nerd Font）| plain（纯文本）。
                                # 终端字体未打 Nerd Font 补丁时选 plain；
                                # TERM=dumb 时自动降级 plain。
  alt_screen: false             # true 使用终端备用屏幕（退出后恢复滚屏，默认 false）
  # keymap:                     # 快捷键覆盖（docs/VIM_UI_DESIGN.md §5.2）：上下文 → 动作 → 键。
  #   chat:                     #   非法条目被忽略并在状态栏给出告警
  #     search_transcript: "ctrl+s"
  #   picker:
  #     close: "ctrl+x"

# ------------------------------------------------------------------------------
# 子代理（delegate_task 工具，docs/SUBAGENT_DESIGN.md §7）
# ------------------------------------------------------------------------------
subagent:
  enabled: true                 # false 从工具注册表移除 delegate_task（默认 true）
  max_tokens: 0                 # 子 run 会话级累计 token 上限；0 继承 limits.max_tokens。
                                # 子代理消耗同时折算回父 run 预算，委托不是预算漏洞
  max_output_tokens: 8192       # 子代理单次模型响应的输出上限（默认 8192，含推理
                                # 余量）；显式 0 继承 limits.max_output_tokens
  model: ""                     # 固定子代理模型（provider/model 或裸名）；
                                # 空表示跟随当前轮次的模型

# ------------------------------------------------------------------------------
# 长期记忆（docs/MEMORY_DESIGN.md）：后台提取/归纳流水线，进程启动时运行
# 一次，之后每隔 run_interval 运行一次；从不在退出路径上运行。
# ------------------------------------------------------------------------------
memory:
  enabled: true                 # false 关闭记忆提取、归纳与工具注册（默认 true）
  extract_model: ""             # 阶段一提取模型（provider/model 或裸名）；
                                # 空跟随默认模型，建议用便宜快速的模型
  consolidation_model: ""       # 阶段二归纳模型；空跟随默认模型
  max_jobs_per_run: 8           # 每轮流水线最多认领的提取任务数（默认 8，1-128）
  run_interval: 30m             # 流水线周期（Go duration；默认 30m，
                                # "0" 表示只在启动时运行一次）
  min_session_idle: 1h          # 跳过最近 min_session_idle 内仍有活动的会话
                                # （默认 1h，避免提取进行中的会话）
  max_session_age: 720h         # 跳过最后活动时间早于 max_session_age 的会话
                                # （默认 720h = 30 天）

# ------------------------------------------------------------------------------
# 文生图（generate_image 工具）：复用指定 provider 的 base_url/api_key，
# 因此 provider 必须是 openai 类型。provider 与 model 都填写时才注册工具
# （enabled 缺省时），显式 enabled: false 总是关闭。
# ------------------------------------------------------------------------------
image:
  # enabled: true             # 缺省 = provider 与 model 都设置时启用；
                              # 显式 false 强制关闭
  provider: ""                  # 复用凭据的 provider 名（须为 openai 类型）
  model: ""                     # 生图模型名
  size: ""                      # 默认尺寸（OpenAI Images API 词汇：1024x1024 等；
                              # 空 = auto）
  quality: ""                   # 默认质量（low | medium | high；空 = auto）

# ------------------------------------------------------------------------------
# MCP 服务器（Model Context Protocol）
# 两种传输二选一：command（stdio，loom 拉起子进程）或 url（streamable
# HTTP，连接远程端点）。配置后 loom 启动时自动连接、发现工具并注册到
# 工具表中，工具名格式 mcp__{服务器名}__{工具名}，与内置工具隔离无冲突。
# 单个服务器启动失败仅记录警告，不影响其他服务器和内置工具。
# ------------------------------------------------------------------------------
mcp_servers: {}
  # 示例：连接一个文件系统 MCP 服务器（stdio 传输）
  # filesystem:
  #   command: npx
  #   args: ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
  #   # env: {}                       # 额外环境变量（追加到当前进程环境）
  #   # cwd: ""                       # 子进程工作目录；空继承 loom 的工作目录
  #   # startup_timeout_sec: 30       # 连接+握手超时（默认 30s）
  #   # tool_timeout_sec: 300         # 单次 tools/call 超时（默认 300s）
  #   # enabled_tools: []             # 白名单：仅注册列出的工具；空=全部
  #   # disabled_tools: []            # 黑名单：跳过列出的工具
  #
  # 示例：连接一个远程 MCP 服务器（streamable HTTP 传输）
  # remote:
  #   url: https://mcp.example.com/mcp
  #   headers:                      # 静态请求头；${VAR} 在加载时展开为环境
  #     Authorization: Bearer ${MCP_TOKEN}  # 变量值（变量未设置则报错），
  #                                   # 令牌不落盘，与 api_key_env 同理

# ------------------------------------------------------------------------------
# 预注册工作区（docs/WORKSPACE_DESIGN.md §10）。可选；启动目录始终会注册
# 为默认工作区，与此列表无关。root 支持以 "~" 开头表示家目录。
# ------------------------------------------------------------------------------
workspaces: []
  # 示例：
  # - name: playground          # 工作区显示名（可选）
  #   root: ~/workspace/playground
`
