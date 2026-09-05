// cfgpath.ts — Config key-path utilities and preservation logic for UI-unmanaged keys.
// Mirrors the path utilities of the old settings.js one-to-one.

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

// UI-unmanaged config paths: carried back verbatim from the loaded config on save to avoid silent loss
// (merge semantics: "unprovided key = removed from the file"). PRESERVE_PATHS covers
// nested keys that are known but have no UI editor; top-level keys outside KNOWN_TOP_KEYS (future
// config sections) are likewise always preserved — UI completeness should not be a precondition of correctness.
// skills.disabled is written directly via a dedicated endpoint by the skills tab's disable toggle (not in the form),
// and carried back verbatim on save.
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
