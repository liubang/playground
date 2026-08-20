// convert.ts — 控件值 ↔ config 值的双向转换（spec 驱动）。
// 与旧 settings.js 的 fillControl/collectControl 一一对应，但操作纯数据
// 而非 DOM：draft 里存控件态（raw），保存时收集为 config 结构。
//
// 空值语义与文件一致：留空 = 不写入该键（omitempty），默认值全部隐式。

import type { FieldSpec } from './spec'
import { setPath } from './cfgpath'

// 控件态：文本类一律 string；bool/flag-list 为 boolean。
export type ControlState = string | boolean

// fillValue 把 config 值转为控件态（加载时填充）。
export function fillValue(spec: FieldSpec, value: unknown): ControlState {
  const t = spec.type || 'text'
  if (value === undefined || value === null) value = t === 'bool' || t === 'flag-list' ? false : ''
  switch (t) {
    case 'bool':
      return value === true
    case 'flag-list':
      return Array.isArray(value) && value.length > 0
    case 'tristate':
      return value === '' || value === false || value === true
        ? value === ''
          ? ''
          : String(value)
        : String(value)
    case 'list-text':
      return ((value as string[]) || []).join('\n')
    case 'pair-list':
      return (
        ((value as { name: string; description?: string }[]) || []) as {
          name: string
          description?: string
        }[]
      )
        .map((c) => (c.description ? `${c.name}: ${c.description}` : c.name))
        .join('\n')
    case 'kv-text':
      return Object.entries((value as Record<string, string>) || {})
        .map(([k, v]) => `${k}=${v}`)
        .join('\n')
    case 'float-list':
      return ((value as number[]) || []).join(', ')
    default:
      return value === '' ? '' : String(value)
  }
}

// collectValue 把控件态收集进 obj[key]（保存时）；空值不写键。
export function collectValue(spec: FieldSpec, state: ControlState, obj: Record<string, unknown>) {
  const key = spec.key
  const t = spec.type || 'text'
  switch (t) {
    case 'password': {
      if (state !== '') setPath(obj, key, state) // 密钥不 trim
      break
    }
    case 'number': {
      if (String(state).trim() !== '') setPath(obj, key, Number(state))
      break
    }
    case 'bool': {
      if (state === true) setPath(obj, key, true) // false = 默认，不写入
      break
    }
    case 'flag-list': {
      if (state === true) setPath(obj, key, [...(spec.flagValue || [])]) // 不勾 = 默认（省略键）
      break
    }
    case 'tristate': {
      if (state !== '') setPath(obj, key, state === 'true')
      break
    }
    case 'select': {
      if (state !== '') setPath(obj, key, state)
      break
    }
    case 'list-text': {
      const items = String(state)
        .split('\n')
        .map((s) => s.trim())
        .filter(Boolean)
      if (items.length) setPath(obj, key, items)
      break
    }
    case 'pair-list': {
      const items = String(state)
        .split('\n')
        .map((s) => s.trim())
        .filter(Boolean)
        .map((line) => {
          const i = line.indexOf(':')
          return i > 0
            ? { name: line.slice(0, i).trim(), description: line.slice(i + 1).trim() }
            : { name: line }
        })
      if (items.length) setPath(obj, key, items)
      break
    }
    case 'kv-text': {
      const m: Record<string, string> = {}
      for (const line of String(state).split('\n')) {
        const i = line.indexOf('=')
        if (i > 0 && line.slice(0, i).trim()) m[line.slice(0, i).trim()] = line.slice(i + 1).trim()
      }
      if (Object.keys(m).length) setPath(obj, key, m)
      break
    }
    case 'float-list': {
      const nums = String(state)
        .split(/[,\s]+/)
        .filter(Boolean)
        .map(Number)
        .filter((n) => !Number.isNaN(n))
      if (nums.length) setPath(obj, key, nums)
      break
    }
    default: {
      const v = String(state).trim()
      if (v !== '') setPath(obj, key, v)
    }
  }
}

// collectFields 收集一组同 scope 的字段（卡片/分组用）。
export function collectFields(
  specs: FieldSpec[],
  states: Record<string, ControlState>,
  obj: Record<string, unknown>,
) {
  for (const spec of specs) {
    collectValue(
      spec,
      states[spec.key] ?? (spec.type === 'bool' || spec.type === 'flag-list' ? false : ''),
      obj,
    )
  }
}
