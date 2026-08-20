// cfgpath.ts — config 键路径工具与「UI 未管理键」保留逻辑。
// 与旧 settings.js 的路径工具一一对应。

export function getPath(obj: unknown, path: string): unknown {
  return path
    .split('.')
    .reduce<unknown>((o, k) => (o == null ? undefined : (o as Record<string, unknown>)[k]), obj)
}

export function setPath(obj: Record<string, unknown>, path: string, value: unknown) {
  const keys = path.split('.')
  let o = obj
  for (let i = 0; i < keys.length - 1; i++) {
    if (typeof o[keys[i]] !== 'object' || o[keys[i]] === null) o[keys[i]] = {}
    o = o[keys[i]] as Record<string, unknown>
  }
  o[keys[keys.length - 1]] = value
}

// UI 未管理的配置路径：保存时从已加载的配置原样带回，避免静默丢失
// （merge 的语义是「未提供的 key = 从文件删除」）。PRESERVE_PATHS 覆盖
// 已知但 UI 未做编辑器的嵌套键；KNOWN_TOP_KEYS 之外的顶层键（未来新增
// 的配置节）也一律保留 —— UI 完整性不该是正确性的前提。
// skills.disabled 由技能 tab 的禁用开关经专用端点直写（不在表单里），
// 保存时原样带回。
export const PRESERVE_PATHS = ['ui.keymap', 'skills.disabled']
export const KNOWN_TOP_KEYS = new Set([
  'default',
  'providers',
  'limits',
  'context',
  'runaway',
  'prompt',
  'skills',
  'rules',
  'approval',
  'tracing',
  'share',
  'logging',
  'ui',
  'subagent',
  'memory',
  'sessions',
  'image',
  'browser',
  'knowledge_base',
  'mcp_servers',
  'workspaces',
])

export function preserveUnmanaged(cfg: Record<string, unknown>, orig: Record<string, unknown>) {
  for (const [k, v] of Object.entries(orig)) {
    if (!KNOWN_TOP_KEYS.has(k) && cfg[k] === undefined) cfg[k] = v
  }
  for (const path of PRESERVE_PATHS) {
    const v = getPath(orig, path)
    if (v !== undefined && getPath(cfg, path) === undefined) setPath(cfg, path, v)
  }
}
