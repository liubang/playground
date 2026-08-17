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

// EnsureFirstRunConfig writes the starter template at <home>/config.yaml
// when home is the DEFAULT loom home and the file is missing, creating
// the parent directory (0700) and the file (0600). An explicit LOOM_HOME
// directory is never bootstrapped: it names a place that should already
// be set up — a user error to surface, never a state to paper over with
// a generated file. An existing file is never overwritten. When the
// default home cannot be resolved (e.g. HOME is unset) the directory is
// treated as non-default: no file is created and the caller keeps its
// original error. Returns created=true when a fresh template was written
// so the caller can route the first-run experience: the CLI prints the
// path and exits non-zero, the desktop keeps booting into the settings
// UI where the API key is collected.
func EnsureFirstRunConfig(home string) (bool, error) {
	abs, err := filepath.Abs(home)
	if err != nil {
		return false, fmt.Errorf("config: resolve loom home: %w", err)
	}
	def, err := DefaultHomeDir()
	if err != nil || abs != def {
		return false, nil
	}
	path := ConfigPathForHome(abs)
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
const template = `# loom 配置文件 — 位置: <loom home>/config.yaml（默认 ~/.loom，可用 LOOM_HOME 指定）
# 除 providers 外所有节可省略，省略取内置默认值。含明文密钥时建议 chmod 600。

# 默认模型。写法: provider/model（推荐）| 裸模型名（须全局唯一）| 裸 provider 名。
# 省略取 providers[0] 的默认模型。
default: deepseek/deepseek-chat

# 模型提供方（必填，至少一个）。运行中用 /model 切换。
providers:
  - name: deepseek
    type: openai                # openai（兼容网关，默认）| anthropic（Messages API）
    base_url: https://api.deepseek.com/v1
    # 密钥二选一（互斥）: api_key 明文书写 | api_key_env 引用环境变量名
    api_key: <your-api-key>
    # api_key_env: DEEPSEEK_API_KEY
    wire_api: chat              # openai: chat（默认）| responses；anthropic: messages
    max_retries: 2
    default_model: deepseek-chat
    # reasoning:                # provider 级推理意图（可被模型覆盖）
    #   effort: medium          # off | low | medium | high
    #   budget_tokens: 0        # 显式推理预算，>0 优先于 effort
    models:
      - name: deepseek-chat
        context_window: 65536
        max_output_tokens: 8192
      - name: deepseek-reasoner
        context_window: 65536
        wire_api: responses     # 模型级协议覆盖

  # - name: openai
  #   type: openai
  #   base_url: https://api.openai.com/v1
  #   api_key_env: OPENAI_API_KEY
  #   models:
  #     - name: gpt-5
  #       context_window: 400000

  # - name: anthropic
  #   type: anthropic
  #   base_url: https://api.anthropic.com
  #   api_key_env: ANTHROPIC_API_KEY
  #   # auth_type: bearer       # 网关用 bearer，官方用 x-api-key（默认）
  #   models:
  #     - name: claude-sonnet-4-6
  #       context_window: 200000
  #       max_output_tokens: 64000
  #       reasoning:
  #         effort: high

# 运行预算。超限先收尾再中止当前 run。0 = 不限。
limits:
  max_input_tokens: 200000      # 模型未声明 context_window 时的回退窗口
  max_output_tokens: 16384      # 单次输出上限
  max_cost_usd: 5.0             # 会话级成本上限 USD（需配置 tracing 费率）
  max_tokens: 0                 # 会话级累计 token 预算
  max_tool_output_bytes: 49152  # 单条工具结果保留字节（超出转存 artifact）
  max_artifact_bytes: 104857600 # 单个 artifact 最大字节

# 上下文压缩（比例随模型窗口自动伸缩）。
context:
  utilization: 0.95             # 有效窗口 = context_window × utilization
  compact_trigger_ratio: 0.80   # 自动压缩触发线
  compact_target_ratio: 0.50    # 压缩目标（须 < trigger）
  notice_levels: [0.60, 0.75]   # 占用提醒档位（升序且 < trigger）

# 失控检测（检测死循环/停滞，不限工作量）。
runaway:
  max_repeated_calls: 3         # 同一 (工具, 参数) 连续重复上限
  max_consecutive_failures: 5
  stall_warn_turns: 10          # 无进展回合数达此值注入提醒；0 关闭
  stall_timeout: 15m            # 停滞看门狗；0 关闭（审批等待不计入）

# 系统提示词。
prompt:
  extra: |
    Always reply in Chinese.
  disable_builtin: false
  managed:                      # Langfuse 托管提示词（需配置 tracing）
    name: ""
    label: production

# Skills。
skills:
  enabled: true
  extra_roots: []               # 额外技能搜索目录，支持 ~/ 前缀

# 开发工具链。path_extra 补充内置 PATH 候选之外的目录，保存即热应用。
tools:
  path_extra: []

# 权限规则。argv 前缀作用于 run_cmd，domains 作用于 web_fetch，
# paths 作用于工作区外写（子路径语义，敏感目录拒绝）。
rules:
  enabled: true
  builtin: true                 # 内置只读命令集
  project: true                 # 项目层 <workspace>/.loom/rules
  project_allow: false          # 不可信仓库只能收紧策略
  persist_remembered: true      # "始终允许"写入用户层规则文件

# 审批基线。无规则/记忆命中时的决策策略。
approval:
  mode: on-request              # on-request（默认）| unless-dangerous | never

# Langfuse 追踪。host 与两个 key 都填写才启用。
tracing:
  host: ""
  public_key: ""                # 也支持 public_key_env
  secret_key: ""                # 也支持 secret_key_env
  environment: dev
  include_content: true         # false 不上送对话原文
  user: ""                      # 空则取 git user.email / $USER
  cost_input_usd_per_mtok: 0    # 输入费率 USD/Mtok
  cost_output_usd_per_mtok: 0

# 局域网分享。固定端口使链接跨重启存活；只暴露只读页面。保存即热应用。
share:
  enabled: false
  listen: 0.0.0.0:7681

# 文件日志配额（<loom home>/logs 下按日文件）。
logging:
  max_file_mb: 0                # 0 = 内置 2048；负数关闭
  max_total_mb: 0               # 0 = 内置 10240；负数关闭

# 终端界面。
ui:
  icons: nerd                   # nerd（Nerd Font）| plain
  alt_screen: false
  # keymap:                     # 快捷键覆盖: 上下文 → 动作 → 键
  #   chat:
  #     search_transcript: "ctrl+s"

# 子代理（delegate_task 工具）。
subagent:
  enabled: true
  max_tokens: 0                 # 0 继承 limits.max_tokens
  max_output_tokens: 8192      # 0 继承 limits.max_output_tokens
  model: ""                     # 空跟随当前轮次模型

# 长期记忆。后台提取/归纳流水线，启动时运行一次，之后每 run_interval 运行。
memory:
  enabled: true
  extract_model: ""             # 空跟随默认模型，建议用便宜模型
  consolidation_model: ""
  max_jobs_per_run: 8           # 1-128
  run_interval: 30m             # 0 = 只在启动时运行一次
  min_session_idle: 1h          # 跳过近期活跃会话
  max_session_age: 720h         # 跳过过旧会话（30 天）

# 文生图（generate_image）。复用 openai 类型 provider 的凭据。
image:
  # enabled: true               # 缺省 = provider+model 都设置时启用
  provider: ""
  model: ""
  size: ""                      # 1024x1024 等；空 = auto
  quality: ""                   # low | medium | high；空 = auto

# MCP 服务器。command（stdio）或 url（streamable HTTP）二选一。
mcp_servers: {}
  # filesystem:
  #   command: npx
  #   args: ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
  #   # startup_timeout_sec: 30
  #   # tool_timeout_sec: 300
  # remote:
  #   url: https://mcp.example.com/mcp
  #   headers:
  #     Authorization: Bearer ${MCP_TOKEN}  # ${VAR} 加载时展开

# 知识库（kb_search / kb_read）。连接 minisearch 服务，只读消费。
# knowledge_base:
#   enabled: true
#   base_url: http://127.0.0.1:8200
#   api_key: msk_xxxxxxxxxxxxxxxx   # --auth=off 时留空
#   timeout_ms: 10000              # 1000-60000
#   default_top_k: 5              # 1-20
#   default_collection: loom-kb   # 省略取第一项
#   collections:                   # 至少一个；description 帮助模型路由
#     - name: loom-kb
#       description: Loom 设计文档与使用手册

# 预注册工作区。启动目录始终注册为默认工作区。root 支持 ~ 前缀。
workspaces: []
  # - name: playground
  #   root: ~/workspace/playground
`
