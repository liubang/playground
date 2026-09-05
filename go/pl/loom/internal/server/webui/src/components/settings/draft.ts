// draft.ts — Draft data model for the settings panel.
// The old version used the DOM as the draft (fill/collect shuttled both ways); the React version uses data as the draft:
// fill once on load, edits write the draft directly, collect on save — fields of tabs not
// opened are never lost by construction (the old version relied on "render all tabs, then collect").

import type { ControlState } from './convert'

export interface CardDraft {
  id: string // React key / lookup (not config content)
  fields: Record<string, ControlState>
}

export interface ProviderDraft extends CardDraft {
  models: CardDraft[]
}

export type McpTransport = 'stdio' | 'http'

export interface McpDraft {
  id: string
  name: string
  transport: McpTransport
  stdio: Record<string, ControlState>
  http: Record<string, ControlState>
  common: Record<string, ControlState>
}

export interface SettingsDraft {
  // Global-scope fields (simple tabs + skills config sections + providers top default)
  globals: Record<string, ControlState>
  providers: ProviderDraft[]
  mcpServers: McpDraft[]
  workspaces: CardDraft[]
}

let nextDraftId = 1
export function draftId(): string {
  return 'd' + nextDraftId++
}
