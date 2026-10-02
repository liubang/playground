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
const minimalExample = `default: deepseek/deepseek-chat
providers:
  - name: deepseek
    type: openai
    base_url: https://api.deepseek.com/v1
    api_key: <your-api-key>       # 必填：换成你的真实密钥，或用 api_key_env: DEEPSEEK_API_KEY
    models:
      - name: deepseek-chat
        context_window: 65536`

// template is the starter config written by `loom config init`. It is
// two layers: an active minimal layer (default + providers, the only
// required section) and a commented reference layer documenting every
// remaining section with its built-in defaults — an uncommented key is
// always a deliberate override, never template noise. The reference
// layer doubles as the user-facing configuration reference; keep it in
// sync with schema.go (TestTemplateCoversSchemaSections locks this).
const template = `# loom 配置文件 — 位置: <loom home>/config.yaml（默认 ~/.loom，可用 LOOM_HOME 指定）
# 仅 providers 必填。下方注释区是全部可选配置的参考，省略即取标注的
# 内置默认值——只有需要覆盖默认值的键才取消注释。含明文密钥时建议 chmod 600。

# 默认模型: provider/model（推荐）| 裸模型名（须全局唯一）| 裸 provider 名（取其第一个
# 模型）。省略时取 providers[0] 的第一个模型；运行中用 /model 切换。
default: deepseek/deepseek-chat

# 模型提供方（必填，至少一个）。
providers:
  - name: deepseek
    type: openai                # openai（兼容网关，默认）| anthropic（Messages API）
    base_url: https://api.deepseek.com/v1
    # 密钥二选一（互斥）: api_key 明文书写 | api_key_env 引用环境变量名
    #   api_key: <your-api-key>
    #   api_key_env: DEEPSEEK_API_KEY
    models:
      - name: deepseek-chat
        context_window: 65536
        max_output_tokens: 8192
      - name: deepseek-reasoner
        context_window: 65536

  # 更多 provider 示例：
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
  #   # auth_type: bearer       # 仅 anthropic：网关用 bearer，官方 x-api-key（默认）
  #   # api_version: ""         # 仅 anthropic：协议版本头，空 = 内置固定版本
  #   models:
  #     - name: claude-sonnet-4-6
  #       context_window: 200000
  #       max_output_tokens: 64000
  #       reasoning: {effort: high}

# ═══════════════════ 可选配置参考（省略 = 取标注的内置默认值）═══════════════════

# provider 级可选键（写在 provider 条目内）：
#   wire_api: chat                 # openai: chat（默认）| responses；anthropic 固定 messages
#   max_retries: 2                 # 默认 2
#   response_header_timeout: 60s   # 等响应头上限（默认 60s，"0" 关闭）
#   stream_idle_timeout: 120s      # 流中沉默上限，任何字节重置（默认 120s，"0" 关闭）
#   attempt_timeout: 0             # 单次尝试总时长上限（默认关闭）
#   reasoning:                     # 推理意图（模型级可覆盖）
#     effort: auto                 # auto（默认：规划轮 high / 执行轮 low，连败升级）| off | low | medium | high
#     budget_tokens: 0             # 显式推理预算，>0 优先于 effort
#
# model 级可选键（写在 models 条目内）：
#     max_output_tokens: 0      # 单次响应能力上限（请求参数），0 = 不设（预算护栏在 limits.max_output_tokens）
#     wire_api: ""              # 覆盖 provider 级协议
#     modalities: [text]        # 加入 "image" 才允许图片输入，如 [text, image]
#     window_utilization: 0     # 覆盖 context.utilization（网关虚报窗口时用），(0, 1]
#     reasoning: {}             # 同 provider 级，覆盖之

# 运行预算（0 = 不限；超限先收尾再中止当前 run）：
# limits:
#   max_input_tokens: 200000      # 默认 200000，模型未声明 context_window 时的回退窗口
#   max_output_tokens: 16384      # 默认 16384，单次输出上限
#   max_cost_usd: 5.0             # 默认 5.0，会话级成本上限 USD（需配置 tracing 费率）
#   max_tokens: 0                 # 默认 0，会话级累计 token 预算
#   max_tool_output_bytes: 49152  # 默认 49152，单条工具结果保留字节（超出转存 artifact）
#   max_artifact_bytes: 104857600 # 默认 100MiB，单个 artifact 最大字节

# 上下文压缩（比例随模型窗口自动伸缩）：
# context:
#   utilization: 0.95             # 默认 0.95，有效窗口 = context_window × utilization
#   compact_trigger_ratio: 0.80   # 默认 0.80，自动压缩触发线
#   compact_target_ratio: 0.50    # 默认 0.50，压缩目标（须 < trigger）
#   notice_levels: [0.60, 0.75]   # 默认 [0.60, 0.75]，占用提醒档位（升序且 < trigger）

# 失控检测（检测死循环/停滞，不限工作量）：
# runaway:
#   max_repeated_calls: 3         # 默认 3，同一 (工具, 参数) 连续重复上限
#   max_consecutive_failures: 5   # 默认 5，连续工具失败上限
#   stall_warn_turns: 10          # 默认 10，无进展回合数达此值注入提醒；0 关闭
#   stall_timeout: 15m            # 默认 15m，停滞看门狗；0 关闭（审批等待不计入）

# 系统提示词：
# prompt:
#   extra: |                      # 追加到内置提示词之后
#     Your custom instructions here.
#   disable_builtin: false        # 默认 false；true = 只用 extra，不用内置提示词
#   managed:                      # Langfuse 托管提示词（需配置 tracing）
#     name: ""
#     label: production

# Skills：
# skills:
#   enabled: true                 # 默认 true
#   extra_roots: []               # 额外技能搜索目录，支持 ~/ 前缀
#   disabled: []                  # 按名禁用（加载期剔除，跨所有作用域）

# 开发工具链：
# tools:
#   path_extra: []                # 补充内置 PATH 候选之外的目录，支持 ~/，保存即热应用

# 权限规则（argv 前缀作用于 run_cmd，domains 作用于 web_fetch，paths 作用于工作区外写）：
# rules:
#   enabled: true                 # 默认 true
#   builtin: true                 # 默认 true，内置只读命令集
#   project: true                 # 默认 true，项目层 <workspace>/.loom/rules
#   project_allow: false          # 默认 false，不可信仓库只能收紧策略
#   persist_remembered: true      # 默认 true，"始终允许"写入用户层规则文件

# 审批基线（无规则/记忆命中时的决策策略）：
# approval:
#   mode: on-request              # on-request（默认）| danger-only | never
#   trust_user_urls: true         # 默认 true，自动放行用户在对话中提到的 host

# Langfuse 追踪（host 与两个 key 都填写才启用；key 也支持 *_env 引用环境变量）：
# tracing:
#   host: ""
#   public_key: ""
#   secret_key: ""
#   environment: dev              # 默认 dev
#   include_content: true         # 默认 true；false 不上送对话原文
#   user: ""                      # 空则取 git user.email / $USER
#   cost_input_usd_per_mtok: 0    # 输入费率 USD/Mtok，0 = 不计成本
#   cost_output_usd_per_mtok: 0

# 局域网分享（loom-desktop；固定端口使链接跨重启存活，只暴露只读页面，保存即热应用）：
# share:
#   enabled: false                # 默认 false
#   listen: 0.0.0.0:7681          # 默认 0.0.0.0:7681

# 文件日志配额（<loom home>/logs 下按日文件）：
# logging:
#   max_file_mb: 0                # 0 = 内置 2048；负数关闭
#   max_total_mb: 0               # 0 = 内置 10240；负数关闭

# 终端界面：
# ui:
#   icons: nerd                   # nerd（默认，Nerd Font）| plain
#   alt_screen: false             # 默认 false
#   keymap:                       # 快捷键覆盖: 上下文 → 动作 → 键
#     chat:
#       search_transcript: "ctrl+s"

# 子代理（delegate_task 工具）：
# subagent:
#   enabled: true                 # 默认 true
#   max_tokens: 0                 # 默认 0 = 继承 limits.max_tokens
#   max_output_tokens: 8192       # 默认 8192；显式 0 = 继承 limits.max_output_tokens
#   model: ""                     # 空 = 跟随当前轮次模型

# 长期记忆（后台提取/归纳流水线，启动时运行一次，之后每 run_interval 运行）：
# memory:
#   enabled: true                 # 默认 true
#   extract_model: ""             # 空 = 跟随默认模型，建议用便宜模型
#   consolidation_model: ""       # 空 = 跟随默认模型
#   max_jobs_per_run: 8           # 默认 8（1-128）
#   run_interval: 30m             # 默认 30m；0 = 只在启动时运行一次
#   min_session_idle: 1h          # 默认 1h，跳过近期活跃会话
#   max_session_age: 720h         # 默认 720h（30 天），跳过过旧会话

# 会话生命周期（默认都关闭）：
# sessions:
#   auto_archive_after: "0"       # 如 "720h"：超龄未活跃会话自动归档（隐藏且只读）
#   gc_archived_after: "0"        # 如 "720h"：归档超龄后永久删除（含事件/检查点/历史）

# 文生图（generate_image；provider 与 model 都设置即启用，复用 openai 类型 provider 凭据）：
# image:
#   provider: ""
#   model: ""
#   size: ""                      # 1024x1024 等；空 = auto
#   quality: ""                   # low | medium | high；空 = auto

# 无头浏览器（默认启用，本地启动 Chrome）：
# browser:
#   enabled: true                 # 默认 true
#   chrome_path: ""               # 空 = 自动探测
#   cdp_url: ""                   # 设置后连接远程 Chrome（ws:// 或 http://），不再本地启动
#   idle_ttl: 5m                  # 默认 5m，空闲回收
#   nav_timeout_ms: 30000         # 默认 30000（5000-120000）
#   screenshot_quality: 80        # 默认 80（10-100）
#   viewport_width: 1280          # 默认 1280（320-4096）
#   viewport_height: 720          # 默认 720（320-4096）

# MCP 服务器（command = stdio / url = streamable HTTP，二选一）：
# mcp_servers:
#   filesystem:
#     command: npx
#     args: ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
#     # startup_timeout_sec: 30
#     # tool_timeout_sec: 300
#   remote:
#     url: https://mcp.example.com/mcp
#     headers:
#       Authorization: Bearer ${MCP_TOKEN}   # ${VAR} 加载时展开

# 知识库（kb_search / kb_read；连接 minisearch 服务，opt-in，只读消费）：
# knowledge_base:
#   enabled: true
#   base_url: http://127.0.0.1:8200
#   api_key: msk_xxxxxxxxxxxxxxxx     # --auth=off 时留空
#   timeout_ms: 10000                 # 默认 10000（1000-60000）
#   default_top_k: 5                  # 默认 5（1-20）
#   default_collection: loom-kb       # 省略取 collections 第一项
#   collections:                      # 至少一个；description 帮助模型路由
#     - name: loom-kb
#       description: Loom 设计文档与使用手册

# 预注册工作区（启动目录始终注册为默认工作区；root 支持 ~ 前缀）：
# workspaces:
#   - name: playground
#     root: ~/workspace/playground
`
