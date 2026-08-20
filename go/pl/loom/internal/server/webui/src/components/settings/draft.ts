// draft.ts — 设置面板的草稿数据模型。
// 旧版以 DOM 为草稿（fill/collect 双向搬运）；React 版以数据为草稿：
// 加载时 fill 一次，编辑直接写草稿，保存时 collect —— 未打开的 tab 的
// 字段天然不会丢失（旧版靠「渲染全部 tab 再整体收集」保证这一点）。

import type { ControlState } from './convert'

export interface CardDraft {
  id: string // React key / 定位用（非配置内容）
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
  // 全局 scope 字段（简单 tab + skills 配置小节 + providers 顶部 default）
  globals: Record<string, ControlState>
  providers: ProviderDraft[]
  mcpServers: McpDraft[]
  workspaces: CardDraft[]
}

let nextDraftId = 1
export function draftId(): string {
  return 'd' + nextDraftId++
}
