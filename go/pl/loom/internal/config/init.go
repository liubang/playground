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

// minimalExample is embedded in fail-fast error messages so a user without
// any config can copy-paste their way to a working setup (§9).
const minimalExample = `  default: deepseek/deepseek-chat
  providers:
    - name: deepseek
      type: openai
      base_url: https://api.deepseek.com/v1
      api_key: sk-xxxxxxxx          # 或 api_key_env: DEEPSEEK_API_KEY
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
    api_key: sk-xxxxxxxx        # API 密钥，直接明文书写即可（配置文件即私有数据）
    # api_key_env: DEEPSEEK_API_KEY
                                # 或者改用环境变量引用：只存变量名，启动时读取值。
                                # 适合 CI 或需要分享/入仓配置文件的场景。
                                # 与 api_key 互斥，同时填写会报错。
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
  extra_roots: []               # 额外的技能搜索目录（列表），如 [/Users/me/skills]

# ------------------------------------------------------------------------------
# 权限规则（声明式 argv 前缀规则，作用于 run_cmd）
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
  mode: on-request              # on-request（默认：沙箱内非危险命令免审批）
                                # unless-dangerous（黑名单模式：沙箱能约束的一切——
                                #   含 needs_network 放网、workspace 写入——免审批；
                                #   仅危险清单/复合 shell/出沙箱提权弹审批）
                                # unless-trusted（保守：每个未匹配的 R2+ 调用都审批）
                                # never（无人值守：沙箱内放行，提权直接拒绝）

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
# 存储
# ------------------------------------------------------------------------------
storage:
  session_db: ""                # 会话数据库路径。空则按系统约定推导：
                                #   macOS: ~/Library/Application Support/loom/sessions.db
                                #   Linux: $XDG_STATE_HOME/loom/sessions.db
                                #     或  ~/.local/state/loom/sessions.db

# ------------------------------------------------------------------------------
# 终端界面
# ------------------------------------------------------------------------------
ui:
  icons: nerd                   # 图标集：nerd（Nerd Font）| plain（纯文本）。
                                # 终端字体未打 Nerd Font 补丁时选 plain；
                                # TERM=dumb 时自动降级 plain。
  alt_screen: false             # true 使用终端备用屏幕（退出后恢复滚屏，默认 false）

# ------------------------------------------------------------------------------
# 子代理（delegate_task 工具，docs/SUBAGENT_DESIGN.md §7）
# ------------------------------------------------------------------------------
subagent:
  enabled: true                 # false 从工具注册表移除 delegate_task（默认 true）
  max_tokens: 0                 # 子 run 会话级累计 token 上限；0 继承 limits.max_tokens。
                                # 子代理消耗同时折算回父 run 预算，委托不是预算漏洞
  model: ""                     # 固定子代理模型（provider/model 或裸名）；
                                # 空表示跟随当前轮次的模型
`
