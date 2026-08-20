// spec.ts — 设置面板的声明式字段 spec（key 即 config.yaml 的键路径）。
// 同一套 spec 驱动渲染与收集 —— 新增配置项只需加一行 spec。
// 数据与旧 settings.js 的 TABS/字段定义一一对应。

import type { SecretRef } from '../../protocol/types'
import type { IconName } from '../../lib/icons'

export type FieldType =
  | 'text'
  | 'password'
  | 'number'
  | 'bool'
  | 'tristate'
  | 'select'
  | 'textarea'
  | 'list-text' // 每行一项 → []string
  | 'kv-text' // 每行 k=v → map
  | 'pair-list' // 每行 "name: description" → [{name, description}]
  | 'float-list' // 逗号分隔 → []number
  | 'flag-list' // 勾选 → 写入固定 []string（spec.flagValue），不勾 = 省略

export interface FieldSpec {
  key: string
  label?: string // 合成 spec（如卡片头行内 name 输入）可不带 label
  hint?: string
  ph?: string
  type?: FieldType
  options?: [string, string][]
  optionHints?: Record<string, string>
  step?: number
  rows?: number
  def?: string
  required?: boolean
  revealRef?: SecretRef
  flagValue?: string[]
}

export interface TabSpec {
  id: string
  label: string
  icon: IconName
  sections?: [string, FieldSpec[]][]
}

// 与 internal/config/edit.go 的 SecretMask 保持一致。
export const SECRET_MASK = '••••••••••'

// 未发现 skill 时的目录约定提示（扫描为空与删空后共用）
export const SKILLS_EMPTY_HINT =
  '未发现任何 skill。目录约定：工作区 .loom/skills/、.agents/skills/，用户级 ~/.loom/skills/、~/.agents/skills/。'

const EFFORT_OPTS: [string, string][] = [
  ['', '默认（provider 决定）'],
  ['off', 'off'],
  ['low', 'low'],
  ['medium', 'medium'],
  ['high', 'high'],
]

export const REASONING_FIELDS: FieldSpec[] = [
  {
    key: 'reasoning.effort',
    label: '推理强度',
    type: 'select',
    options: EFFORT_OPTS,
  },
  {
    key: 'reasoning.budget_tokens',
    label: '推理 token 预算',
    type: 'number',
    ph: '0',
    hint: '显式预算，>0 时优先于强度推导值',
  },
]

export const PROVIDER_BASE_FIELDS: FieldSpec[] = [
  {
    key: 'type',
    label: '协议类型',
    type: 'select',
    options: [
      ['openai', 'openai（兼容网关）'],
      ['anthropic', 'anthropic（Messages API）'],
    ],
  },
  {
    key: 'base_url',
    label: 'Base URL',
    required: true,
    ph: 'https://api.deepseek.com/v1',
  },
  {
    key: 'api_key',
    label: 'API Key',
    type: 'password',
    hint: '与「Key 环境变量」二选一；同时填写会报错',
  },
  {
    key: 'api_key_env',
    label: 'Key 环境变量',
    ph: '如 DEEPSEEK_API_KEY',
    hint: '只存变量名，启动时读取值',
  },
  { key: 'default_model', label: '默认模型', ph: '留空取模型列表第一个' },
]

export const PROVIDER_ADV_FIELDS: FieldSpec[] = [
  {
    key: 'wire_api',
    label: '请求协议',
    type: 'select',
    options: [
      ['', '默认'],
      ['chat', 'chat（Chat Completions）'],
      ['responses', 'responses（Responses API）'],
      ['messages', 'messages（仅 anthropic）'],
    ],
  },
  {
    key: 'auth_type',
    label: '认证头方式',
    type: 'select',
    options: [
      ['', '默认（x-api-key）'],
      ['x-api-key', 'x-api-key'],
      ['bearer', 'bearer'],
    ],
    hint: '仅 anthropic 类型有意义',
  },
  {
    key: 'api_version',
    label: '协议版本头',
    hint: '仅 anthropic 类型；留空取内置版本',
  },
  { key: 'max_retries', label: '失败重试次数', type: 'number', ph: '2' },
  ...REASONING_FIELDS,
]

export const MODEL_FIELDS: FieldSpec[] = [
  { key: 'name', label: '模型名', required: true, ph: '如 deepseek-chat' },
  {
    key: 'context_window',
    label: '上下文窗口',
    type: 'number',
    ph: '如 65536',
  },
  {
    key: 'max_output_tokens',
    label: '单次输出上限',
    type: 'number',
    ph: '如 8192',
  },
  {
    key: 'modalities',
    label: '图片输入（多模态）',
    type: 'flag-list',
    flagValue: ['text', 'image'],
    hint: '仅当模型确实支持图片输入时勾选（写入 modalities: [text, image]）；勾选后输入框可粘贴/拖入图片，纯文本模型勾选会被网关报错',
  },
  {
    key: 'wire_api',
    label: '协议覆盖',
    type: 'select',
    options: [
      ['', '跟随 provider'],
      ['chat', 'chat'],
      ['responses', 'responses'],
    ],
  },
  {
    key: 'window_utilization',
    label: '窗口利用率覆盖',
    type: 'number',
    step: 0.01,
    ph: '跟随全局',
  },
  ...REASONING_FIELDS,
]

export const MCP_STDIO_FIELDS: FieldSpec[] = [
  { key: 'command', label: '命令', required: true, ph: '如 npx' },
  { key: 'args', label: '参数', type: 'list-text', hint: '每行一个参数' },
  {
    key: 'env',
    label: '环境变量',
    type: 'kv-text',
    hint: '每行一个 KEY=VALUE（追加到进程环境）',
  },
  { key: 'cwd', label: '工作目录', hint: '留空继承 loom 的工作目录' },
]

export const MCP_HTTP_FIELDS: FieldSpec[] = [
  {
    key: 'url',
    label: 'URL',
    required: true,
    ph: 'https://mcp.example.com/mcp',
  },
  {
    key: 'headers',
    label: '请求头',
    type: 'kv-text',
    hint: '每行一个 KEY=VALUE；值支持 ${VAR} 引用',
  },
]

export const MCP_COMMON_FIELDS: FieldSpec[] = [
  {
    key: 'startup_timeout_sec',
    label: '启动超时 (秒)',
    type: 'number',
    ph: '30',
  },
  {
    key: 'tool_timeout_sec',
    label: '工具调用超时 (秒)',
    type: 'number',
    ph: '300',
  },
  {
    key: 'enabled_tools',
    label: '工具白名单',
    type: 'list-text',
    hint: '留空注册全部工具',
  },
  { key: 'disabled_tools', label: '工具黑名单', type: 'list-text' },
]

export const TABS: TabSpec[] = [
  { id: 'providers', label: '模型', icon: 'layer-group' }, // 自定义渲染（概览 → 详情两级）
  {
    id: 'limits',
    label: '限额与保护',
    icon: 'shield-halved',
    sections: [
      [
        '运行预算',
        [
          {
            key: 'limits.max_input_tokens',
            label: '回退上下文窗口',
            type: 'number',
            ph: '200000',
            hint: '模型未声明 context_window 时使用',
          },
          {
            key: 'limits.max_output_tokens',
            label: '单次输出上限',
            type: 'number',
            ph: '16384',
          },
          {
            key: 'limits.max_cost_usd',
            label: '成本上限 (USD)',
            type: 'number',
            step: 0.01,
            ph: '5.0',
            hint: '会话级累计估算成本，0 = 不限（需配置追踪成本费率）',
          },
          {
            key: 'limits.max_tokens',
            label: 'Token 总预算',
            type: 'number',
            ph: '0',
            hint: '会话级累计 token，0 = 不限',
          },
          {
            key: 'limits.max_tool_output_bytes',
            label: '工具输出保留字节',
            type: 'number',
            ph: '49152',
          },
          {
            key: 'limits.max_artifact_bytes',
            label: 'Artifact 最大字节',
            type: 'number',
            ph: '104857600',
          },
        ],
      ],
      [
        '上下文压缩',
        [
          {
            key: 'context.utilization',
            label: '窗口利用率',
            type: 'number',
            step: 0.01,
            ph: '0.95',
          },
          {
            key: 'context.compact_trigger_ratio',
            label: '压缩触发线',
            type: 'number',
            step: 0.01,
            ph: '0.80',
          },
          {
            key: 'context.compact_target_ratio',
            label: '压缩目标',
            type: 'number',
            step: 0.01,
            ph: '0.50',
            hint: '必须小于触发线',
          },
          {
            key: 'context.notice_levels',
            label: '占用提醒档位',
            type: 'float-list',
            ph: '0.60, 0.75',
            hint: '逗号分隔，升序且小于触发线',
          },
        ],
      ],
      [
        '失控检测',
        [
          {
            key: 'runaway.max_repeated_calls',
            label: '重复调用上限',
            type: 'number',
            ph: '3',
          },
          {
            key: 'runaway.max_consecutive_failures',
            label: '连续失败上限',
            type: 'number',
            ph: '5',
          },
          {
            key: 'runaway.stall_warn_turns',
            label: '停滞提醒回合数',
            type: 'number',
            ph: '10',
            hint: '0 = 关闭',
          },
          {
            key: 'runaway.stall_timeout',
            label: '停滞看门狗',
            ph: '15m',
            hint: 'Go duration 语法；0 = 关闭',
          },
        ],
      ],
    ],
  },
  {
    id: 'permission',
    label: '权限与审批',
    icon: 'lock',
    sections: [
      [
        '审批基线',
        [
          {
            key: 'approval.mode',
            label: '审批模式',
            type: 'select',
            options: [
              ['', '默认（on-request）'],
              ['on-request', 'on-request · 沙箱内/工作区内免审批'],
              ['unless-dangerous', 'unless-dangerous · 黑名单模式'],
              ['never', 'never · 无人值守'],
            ],
            hint: '无规则/记忆命中时的决策策略',
            optionHints: {
              '': 'on-request（默认）：沙箱内命令、工作区内读写免审批；仅出沙箱提权/网络放宽/危险清单弹审批',
              'on-request': '沙箱内命令、工作区内读写免审批；仅出沙箱提权/网络放宽/危险清单弹审批',
              'unless-dangerous':
                '黑名单模式：仅危险清单和出沙箱提权弹审批，needs_network 直接放网',
              never: '无人值守：沙箱内放行，提权与危险命令直接拒绝，永不阻塞等待审批',
            },
          },
        ],
      ],
      [
        '规则层',
        [
          { key: 'rules.enabled', label: '启用规则', type: 'tristate' },
          { key: 'rules.builtin', label: '内置只读命令集', type: 'tristate' },
          { key: 'rules.project', label: '项目层规则', type: 'tristate' },
          {
            key: 'rules.project_allow',
            label: '项目层允许 allow 规则',
            type: 'tristate',
            def: '关',
            hint: '不可信仓库只能收紧、不能放宽',
          },
          {
            key: 'rules.persist_remembered',
            label: '持久化「始终允许」',
            type: 'tristate',
            hint: '写入用户层规则文件供后续会话继承',
          },
        ],
      ],
    ],
  },
  {
    id: 'agent',
    label: '智能体',
    icon: 'robot',
    sections: [
      [
        '系统提示词',
        [
          {
            key: 'prompt.extra',
            label: '附加指令',
            type: 'textarea',
            hint: '追加到内置系统提示词末尾',
          },
          {
            key: 'prompt.disable_builtin',
            label: '禁用内置提示词',
            type: 'bool',
          },
          {
            key: 'prompt.managed.name',
            label: '托管提示词名',
            hint: 'Langfuse 托管提示词（需配置追踪）',
          },
          {
            key: 'prompt.managed.label',
            label: '托管提示词标签',
            ph: 'production',
          },
        ],
      ],
      [
        '子智能体',
        [
          { key: 'subagent.enabled', label: '启用子智能体', type: 'tristate' },
          {
            key: 'subagent.model',
            label: '固定模型',
            ph: 'provider/model',
            hint: '留空跟随当前轮次模型',
          },
          {
            key: 'subagent.max_tokens',
            label: 'Token 上限',
            type: 'number',
            ph: '0',
            hint: '0 = 继承运行预算',
          },
          {
            key: 'subagent.max_output_tokens',
            label: '单次输出上限',
            type: 'number',
            ph: '8192',
          },
        ],
      ],
      [
        '长期记忆',
        [
          { key: 'memory.enabled', label: '启用记忆', type: 'tristate' },
          {
            key: 'memory.extract_model',
            label: '提取模型',
            ph: 'provider/model',
            hint: '建议用便宜快速的模型；留空跟随默认模型',
          },
          {
            key: 'memory.consolidation_model',
            label: '归纳模型',
            ph: 'provider/model',
          },
          {
            key: 'memory.max_jobs_per_run',
            label: '每轮任务上限',
            type: 'number',
            ph: '8',
          },
          {
            key: 'memory.run_interval',
            label: '流水线周期',
            ph: '30m',
            hint: '0 = 只在启动时运行一次',
          },
          { key: 'memory.min_session_idle', label: '会话静默阈值', ph: '1h' },
          { key: 'memory.max_session_age', label: '会话最大年龄', ph: '720h' },
        ],
      ],
      [
        '会话归档',
        [
          {
            key: 'sessions.auto_archive_after',
            label: '自动归档阈值',
            ph: '如 720h',
            hint: '超过该时长未活跃的会话自动归档（只读，可随时取消归档）；留空或 0 = 关闭',
          },
        ],
      ],
      [
        '文生图',
        [
          {
            key: 'image.enabled',
            label: '启用文生图',
            type: 'tristate',
            def: '自动',
            hint: '缺省：provider 与 model 都设置时启用',
          },
          {
            key: 'image.provider',
            label: '凭据 provider',
            hint: '复用其 base_url/api_key（须为 openai 类型）',
          },
          { key: 'image.model', label: '生图模型' },
          { key: 'image.size', label: '默认尺寸', ph: '如 1024x1024' },
          {
            key: 'image.quality',
            label: '默认质量',
            type: 'select',
            options: [
              ['', '自动'],
              ['low', 'low'],
              ['medium', 'medium'],
              ['high', 'high'],
            ],
          },
        ],
      ],
    ],
  },
  { id: 'skills', label: 'Skills', icon: 'puzzle-piece' }, // 自定义渲染（配置 + 运行时发现视图）
  { id: 'mcp', label: 'MCP', icon: 'plug' }, // 自定义渲染（概览 → 详情两级）
  {
    id: 'kb',
    label: '知识库',
    icon: 'database',
    sections: [
      [
        '连接',
        [
          {
            key: 'knowledge_base.enabled',
            label: '启用知识库',
            type: 'tristate',
            def: '自动',
            hint: '自动 = 有 base_url 即启用；修改需重启生效',
          },
          {
            key: 'knowledge_base.base_url',
            label: '服务地址',
            ph: 'http://127.0.0.1:8200',
            hint: 'minisearch v2 REST 地址',
            required: true,
          },
          {
            key: 'knowledge_base.api_key',
            label: 'API 密钥',
            type: 'password',
            revealRef: { kind: 'knowledge_base' },
            hint: 'minisearch bearer token（msk_…）；--auth=off 时留空',
          },
          {
            key: 'knowledge_base.timeout_ms',
            label: '请求超时 (ms)',
            type: 'number',
            ph: '10000',
            hint: '范围 1000–60000',
          },
        ],
      ],
      [
        '检索',
        [
          {
            key: 'knowledge_base.default_top_k',
            label: '默认返回数',
            type: 'number',
            ph: '5',
            hint: '范围 1–20',
          },
          {
            key: 'knowledge_base.default_collection',
            label: '默认集合',
            ph: '留空取第一个',
          },
          {
            key: 'knowledge_base.collections',
            label: '集合列表',
            type: 'pair-list',
            rows: 4,
            ph: 'name: 描述（每行一个）',
            hint: '至少一个；描述会写入工具 schema 帮助模型按主题路由',
            required: true,
          },
        ],
      ],
    ],
  },
  {
    id: 'system',
    label: '系统',
    icon: 'gear',
    sections: [
      [
        '开发工具链',
        [
          {
            key: 'tools.path_extra',
            label: '额外 PATH 目录',
            type: 'list-text',
            rows: 3,
            ph: '~/corp/bin',
            hint: '每行一个绝对路径（支持 ~/ 前缀）；优先于所有内置候选目录，保存即热应用',
          },
        ],
      ],
      [
        'Langfuse 追踪',
        [
          {
            key: 'tracing.host',
            label: '服务地址',
            ph: 'https://langfuse.internal',
          },
          {
            key: 'tracing.public_key',
            label: '公钥',
            type: 'password',
            revealRef: { kind: 'tracing', field: 'public_key' },
          },
          { key: 'tracing.public_key_env', label: '公钥环境变量' },
          {
            key: 'tracing.secret_key',
            label: '密钥',
            type: 'password',
            revealRef: { kind: 'tracing', field: 'secret_key' },
          },
          { key: 'tracing.secret_key_env', label: '密钥环境变量' },
          { key: 'tracing.environment', label: '环境标签', ph: 'dev' },
          {
            key: 'tracing.include_content',
            label: '上送对话原文',
            type: 'tristate',
          },
          {
            key: 'tracing.user',
            label: '归属用户',
            hint: '留空依次取 git user.email、$USER',
          },
          {
            key: 'tracing.cost_input_usd_per_mtok',
            label: '输入费率 (USD/Mtok)',
            type: 'number',
            step: 0.01,
            ph: '0',
          },
          {
            key: 'tracing.cost_output_usd_per_mtok',
            label: '输出费率 (USD/Mtok)',
            type: 'number',
            step: 0.01,
            ph: '0',
          },
        ],
      ],
      [
        '局域网分享',
        [
          {
            key: 'share.enabled',
            label: '开启局域网分享',
            type: 'tristate',
            def: '关闭',
            hint: '保存后立即生效（热应用）；监听仅暴露只读分享页，不挂管理 API',
          },
          {
            key: 'share.listen',
            label: '监听地址',
            ph: '0.0.0.0:7681',
            hint: '固定端口使分享链接跨重启存活；0.0.0.0 = 所有接口，也可绑特定接口 IP',
          },
        ],
      ],
      [
        '日志',
        [
          {
            key: 'logging.max_file_mb',
            label: '单日志文件上限 (MiB)',
            type: 'number',
            ph: '2048',
          },
          {
            key: 'logging.max_total_mb',
            label: '日志总量上限 (MiB)',
            type: 'number',
            ph: '10240',
          },
        ],
      ],
      [
        '浏览器',
        [
          {
            key: 'browser.enabled',
            label: '启用浏览器工具',
            type: 'tristate',
            def: '开',
            hint: '默认启用；关闭后浏览器工具不再注册',
          },
          {
            key: 'browser.chrome_path',
            label: 'Chrome 路径',
            ph: '留空自动探测',
            hint: 'Chrome/Chromium 二进制路径；留空时自动探测系统常见位置',
          },
          {
            key: 'browser.cdp_url',
            label: 'CDP 远程地址',
            ph: 'ws://127.0.0.1:9222',
            hint: '远程 Chrome DevTools Protocol 地址；设置后连接外部 Chrome 而非本地启动（可绕过反爬验证）',
          },
          {
            key: 'browser.idle_ttl',
            label: '空闲回收时间',
            ph: '5m',
            hint: '浏览器实例空闲超过此时间后自动关闭（Go 时长语法）',
          },
          {
            key: 'browser.nav_timeout_ms',
            label: '导航超时 (ms)',
            type: 'number',
            ph: '30000',
            hint: '页面导航超时，范围 5000–120000',
          },
          {
            key: 'browser.screenshot_quality',
            label: '截图质量',
            type: 'number',
            ph: '80',
            hint: 'JPEG 质量，范围 10–100',
          },
          {
            key: 'browser.viewport_width',
            label: '视口宽度',
            type: 'number',
            ph: '1280',
          },
          {
            key: 'browser.viewport_height',
            label: '视口高度',
            type: 'number',
            ph: '720',
          },
        ],
      ],
      [
        '终端界面（TUI）',
        [
          {
            key: 'ui.icons',
            label: '图标集',
            type: 'select',
            options: [
              ['', '默认（nerd）'],
              ['nerd', 'nerd（Nerd Font）'],
              ['plain', 'plain（纯文本）'],
            ],
          },
          {
            key: 'ui.alt_screen',
            label: '使用备用屏幕',
            type: 'bool',
            hint: '退出后恢复滚屏',
          },
        ],
      ],
    ], // workspaces 追加为自定义小节（SystemExtras）
  },
]

// skills tab 的配置小节（归属全局 scope）
export const SKILLS_CONFIG_FIELDS: FieldSpec[] = [
  { key: 'skills.enabled', label: '启用技能', type: 'tristate' },
  {
    key: 'skills.extra_roots',
    label: '额外搜索目录',
    type: 'list-text',
    hint: '每行一个目录；支持 ~ 开头自动展开为家目录',
  },
]

// providers tab 顶部「启动模型」字段（归属全局 scope）
export const DEFAULT_MODEL_FIELD: FieldSpec = {
  key: 'default',
  label: '默认模型',
  ph: 'provider/model',
  hint: '留空取第一个 provider 的默认模型',
}

// 全部全局 scope 字段的注册表（保存时按 spec 收集）
export function globalFieldSpecs(): FieldSpec[] {
  const out: FieldSpec[] = [DEFAULT_MODEL_FIELD, ...SKILLS_CONFIG_FIELDS]
  for (const tab of TABS) {
    for (const [, fields] of tab.sections || []) out.push(...fields)
  }
  return out
}
