// convert.ts — Bidirectional conversion of control values ↔ config values (spec-driven).
// Mirrors fillControl/collectControl of the old settings.js one-to-one, but operates on pure data
// instead of the DOM: drafts hold control state (raw) and are collected into the config structure on save.
//
// Empty-value semantics match the file: blank = key not written (omitempty); all defaults are implicit.

import type { FieldSpec } from './spec'
import { setPath } from './cfgpath'

// Control state: text-like types are always string; bool/flag-list are boolean.
export type ControlState = string | boolean

// fillValue converts a config value to control state (fills on load).
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

// collectValue collects control state into obj[key] (on save); empty values do not write the key.
export function collectValue(spec: FieldSpec, state: ControlState, obj: Record<string, unknown>) {
  const key = spec.key
  const t = spec.type || 'text'
  switch (t) {
    case 'password': {
      if (state !== '') setPath(obj, key, state) // secrets are not trimmed
      break
    }
    case 'number': {
      if (String(state).trim() !== '') setPath(obj, key, Number(state))
      break
    }
    case 'bool': {
      if (state === true) setPath(obj, key, true) // false = default, not written
      break
    }
    case 'flag-list': {
      if (state === true) setPath(obj, key, [...(spec.flagValue || [])]) // unchecked = default (key omitted)
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

// collectFields collects a group of fields in the same scope (for cards/groups).
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
