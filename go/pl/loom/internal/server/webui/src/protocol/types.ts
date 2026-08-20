// types.ts — REST/snapshot 的 wire 类型（与 Go 侧 internal/server 的
// JSON 响应一一对应；字段名保持 wire 上的 snake_case）。

import type { ArtifactRef, FailurePayload, Plan, SessionState, TokenUsage } from './events'

// --- snapshot 消息历史 ---

export interface ToolCall {
  id?: string
  name?: string
  arguments?: Record<string, unknown>
}

export interface ToolResultContent {
  kind?: 'text' | 'image' | 'artifact_ref' | string
  text?: string
  image?: { media_type: string; data: string }
  artifact?: ArtifactRef
}

export interface ToolResult {
  call_id?: string
  status?: 'success' | 'error' | 'cancelled' | string
  content?: ToolResultContent[]
  error?: { message?: string }
  started_at?: string
  finished_at?: string
}

export interface MessagePart {
  kind?: 'text' | 'reasoning' | 'tool_call' | 'tool_result' | 'image' | 'artifact_ref' | string
  text?: string
  reasoning?: { text?: string }
  tool_call?: ToolCall
  tool_result?: ToolResult
  image?: { media_type: string; data: string }
  artifact?: ArtifactRef
  model_only?: boolean
}

export interface Message {
  role?: 'user' | 'assistant' | string
  status?: 'interrupted' | string
  created_at?: string
  metadata?: { run_id?: string }
  parts?: MessagePart[]
}

export interface PendingRequest {
  kind?: 'approval' | 'question' | string
  approval?: import('./events').ApprovalRequestedPayload
  question?: import('./events').QuestionPayload
}

export interface ContextWindow {
  nominal?: number
  effective?: number
  compact_trigger?: number
  compact_target?: number
}

export interface Snapshot {
  state?: SessionState
  event_seq?: number
  messages?: Message[]
  pending_requests?: PendingRequest[]
  pending_steers?: string[]
  pending_followups?: string[]
  last_error?: FailurePayload
  usage?: TokenUsage
  turn_count?: number
  window?: ContextWindow
  occupancy?: number
  provider_name?: string
  model_name?: string
  reasoning_effort?: string
  reasoning_overridden?: boolean
  delegated?: boolean
  parent_session_id?: string
  plan?: Plan | null
}

// --- 会话/工作区 ---

export interface SessionSummary {
  id: string
  state?: SessionState | string
  title?: string
  created_at?: string
  updated_at?: string
  model_name?: string
  workspace_id?: string
  parent_session_id?: string
}

export interface Workspace {
  id: string
  name?: string
  root_path?: string
  session_count?: number
}

// --- 模型目录 ---

export interface ModelEntry {
  provider: string
  name: string
  context_window?: number
  modalities?: string[]
}

export interface ModelCatalog {
  models?: ModelEntry[]
  default?: string
}

// --- 目录浏览 ---

export interface DirEntry {
  name: string
  path: string
}

export interface DirBrowseResult {
  path: string
  parent?: string
  home?: string
  entries?: DirEntry[]
}

// --- 配置（设置面板） ---

export interface ConfigEnvelope {
  path: string
  exists: boolean
  revision: string
  config: Record<string, unknown>
}

export interface ApplyReport {
  immediate?: string[]
  next_turn?: string[]
  restart?: string[]
}

export interface PutConfigResult {
  revision?: string
  applied?: ApplyReport
  apply_error?: string
}

export interface SecretRef {
  kind: string
  name?: string
  field?: string
}

// --- Skills ---

export interface SkillInfo {
  name: string
  description?: string
  path: string
  scope?: string
  disabled?: boolean
}

export interface SkillGroup {
  workspace_name: string
  root?: string
  skills?: SkillInfo[]
  issues?: string[]
}

export interface SkillListResult {
  enabled?: boolean
  reason?: string
  groups?: SkillGroup[]
}

// --- MCP ---

export interface McpTool {
  name: string
  description?: string
}

export interface McpServerStatus {
  name: string
  connected?: boolean
  error?: string
  tools?: McpTool[]
}

// --- 规则包 ---

export interface RulePack {
  id: string
  name: string
  risk?: 'low' | 'medium' | 'high' | string
  description?: string
  reason?: string
  commands?: string[]
  installed?: boolean
}

// --- 环境报告（设置-系统-开发环境） ---

export interface EnvTool {
  name: string
  found?: boolean
  path?: string
}

export interface EnvDir {
  path: string
  source?: 'config' | 'static' | 'glob' | string
  status?: 'prepended' | 'existing' | 'missing' | string
}

export interface EnvironmentReport {
  tools?: EnvTool[]
  dirs?: EnvDir[]
  effective_path?: string
}

// --- 分享 ---

export interface ShareEndpoint {
  enabled?: boolean
  url?: string
  error?: string
}

export interface ShareCreateResult {
  path: string
  url?: string
}

export interface SharedView {
  title?: string
  session_id?: string
  updated_at?: string
  messages?: Message[]
}

// --- 其他响应 ---

export interface SetModelResult {
  Window?: ContextWindow
  window?: ContextWindow
}

export interface SetReasoningResult {
  Overridden?: boolean
  overridden?: boolean
}
