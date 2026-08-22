// types.ts — wire types for REST/snapshot (one-to-one with the JSON responses
// of the Go-side internal/server; field names keep the wire's snake_case).

import type { ArtifactRef, FailurePayload, Plan, SessionState, TokenUsage } from './events'

// --- snapshot message history ---

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
  reasoning?: { text?: string; duration_ms?: number }
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

// --- sessions/workspaces ---

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
  is_default?: boolean
}

// --- model catalog ---

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

// --- directory browsing ---

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

// --- config (settings panel) ---

// --- workspace file browsing / git changes (right panel) ---

export interface WorkspaceFileEntry {
  name: string
  path: string // workspace-relative path
  kind: 'dir' | 'file' | string
  size?: number
  mod_time?: string
}

export interface WorkspaceFileList {
  path: string
  entries?: WorkspaceFileEntry[]
  truncated?: boolean
}

export interface WorkspaceFileContent {
  path: string
  size: number
  truncated?: boolean
  binary?: boolean
  content?: string
}

export type ApprovalMode = 'on-request' | 'unless-dangerous' | 'never'

export interface GitFileEntry {
  path: string
  status?: string // M/A/D/R/T (tracked files) or U (untracked)
  staged?: boolean
  unstaged?: boolean
  adds?: number
  dels?: number
  no_stat?: boolean // binary or untracked: no line stats
}

export interface WorkspaceGitStatus {
  is_git?: boolean
  branch?: string
  files?: GitFileEntry[]
  adds?: number
  dels?: number
}

export interface WorkspaceGitDiff {
  path: string
  diff?: string
  truncated?: boolean
  untracked?: boolean
  is_dir?: boolean // untracked directory: no meaningful diff
}

// composer @ file completion
export interface WorkspaceFileMatch {
  path: string
  name: string
  kind: 'dir' | 'file' | string
}

export interface WorkspaceFileSearchResult {
  query: string
  matches?: WorkspaceFileMatch[]
  truncated?: boolean
}

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

// --- rule packs ---

export interface RulePack {
  id: string
  name: string
  risk?: 'low' | 'medium' | 'high' | string
  description?: string
  reason?: string
  commands?: string[]
  installed?: boolean
}

// --- environment report (settings-system-dev environment) ---

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

// --- sharing ---

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

// --- other responses ---

export interface SetModelResult {
  Window?: ContextWindow
  window?: ContextWindow
}

export interface SetReasoningResult {
  Overridden?: boolean
  overridden?: boolean
}

// --- Execution-trace maze (GET /v1/sessions/{id}/maze and
// /v1/shared/{token}/maze; mirrors the Go Maze* types in
// internal/app/maze.go) ---

export type MazeVerdict = 'ok' | 'answer' | 'error' | 'deadend' | 'retry' | 'pending'

export interface MazeTool {
  name: string
  args: string // bar label
  args_full?: string // detail panel
  s: number // start (seconds since the lane's first user message)
  e: number | null // null = still executing
  dur: number
  res: string // hover excerpt
  res_full?: string // detail panel
  v: MazeVerdict
  why?: string
  call_id: string
  status?: string
  child_id?: string // sub-session spawned by delegate_task
}

export interface MazeNode {
  step: number
  turn: number
  s: number
  e: number
  tools: MazeTool[]
  rz: number // reasoning block count
  rz_txt?: string // reasoning excerpt
  rz_ms?: number // summed reasoning wall-clock span (ms)
  in_tok?: number | null
  rz_tok?: number | null
  out_tok?: number | null
  v: MazeVerdict
  why?: string
  sub?: boolean // aggregated sub-agent detour node
  label?: string
  attach?: number // main-path step this detour hangs off
  msg_seq?: number // chat-jump anchor
  retries?: number // model-request retry waits
  live?: boolean // in-flight step (growing)
}

export interface MazeStats {
  steps: number
  tools: number
  rz: number
  rz_ms?: number
  in_tok: number
  rz_tok: number
  out_tok: number
  t: number
  main: number
  detours: number
}

export interface MazeLane {
  key: string
  session_id: string
  title?: string
  model?: string
  main: MazeNode[]
  detours: MazeNode[]
  stats: MazeStats
}

export interface MazeData {
  tmax: number
  lanes: MazeLane[]
}
