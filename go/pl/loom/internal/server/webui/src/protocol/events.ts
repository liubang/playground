// events.ts — SSE 运行时事件的类型模型，与 Go 侧 internal/runtimeevent
// 一一对应。dispatch 处用 kind 判別 + never 兜底获得编译期穷尽性检查。

export type SessionState =
  'idle' | 'running' | 'awaiting_approval' | 'cancelling' | 'booting' | 'fatal' | 'closed'

export interface TokenUsage {
  input_tokens?: number
  output_tokens?: number
  context_tokens?: number
  cached_input_tokens?: number
}

export interface ArtifactRef {
  id: string
  size: number
  media_type?: string
}

export interface ImagePayload {
  media_type: string
  data: string // base64（无 data: 前缀）
}

export interface ToolImage {
  media_type: string
  data: string
}

// --- payload 类型（按事件 kind） ---

export interface TurnStartedPayload {
  prompt?: string
  images?: ArtifactRef[]
}

export interface TurnFinishedPayload {
  error?: string
}

export interface TextDeltaPayload {
  delta?: string
}

export interface ResponseCompletedPayload {
  text?: string
}

export interface FailurePayload {
  code?: string
  stage?: string
  message?: string
}

export interface RequestRetryingPayload {
  code?: string
  wait_ms?: number
  attempt?: number
  max_attempts?: number
}

export interface ToolPreparedPayload {
  call_id?: string
  tool_name?: string
  target?: string
  diff?: string
  arguments?: Record<string, unknown>
}

export interface ToolCompletedPayload {
  call_id?: string
  status?: 'success' | 'error' | 'cancelled' | string
  duration_ms?: number
  preview?: string
  error_message?: string
  error?: string
  images?: ToolImage[]
  artifacts?: ArtifactRef[]
}

export interface ApprovalRequestedPayload {
  approval_id?: string
  call_id?: string
  args_hash?: string
  tool_name?: string
  arguments?: Record<string, unknown>
  target?: string
  description?: string
  risk?: number | string
  rule_preview?: string
}

export interface ApprovalResolvedPayload {
  approval_id?: string
  decision?: 'allow' | 'deny' | string
  actor?: string
}

export interface QuestionOption {
  label: string
  description?: string
}

export interface QuestionPayload {
  question_id?: string
  id?: string
  text?: string
  allow_multiple?: boolean
  options?: QuestionOption[]
}

export interface QuestionAnsweredPayload {
  question_id?: string
  skipped?: boolean
}

export interface SteerQueuedPayload {
  queue?: 'steer' | 'followup' | string
  text?: string
  prompt?: string
}

export interface SteerInjectedPayload {
  text?: string
}

export interface ContextUsagePayload {
  occupancy_tokens?: number
}

export interface ContextCompactedPayload {
  est_tokens_before?: number
  est_tokens_after?: number
  trigger?: string
  masked_outputs?: number
  masked_bytes?: number
  archived_messages?: number
  summarized?: boolean
}

export interface BudgetNoticePayload {
  text?: string
}

export interface ReasoningChangedPayload {
  effective?: { effort?: string }
  overridden?: boolean
}

export interface PlanItem {
  status?: 'todo' | 'in_progress' | 'done' | string
  goal?: string
}

export interface Plan {
  title?: string
  items?: PlanItem[]
}

export interface SubagentPayload {
  role?: string
  session_id?: string
}

export interface RuntimeMessagePayload {
  message?: string
}

// --- 事件信封 ---

interface Envelope<K extends string, P> {
  kind: K
  session_id?: string
  sequence?: number
  time?: string
  run_id?: string
  payload?: P
}

export type RuntimeEvent =
  | Envelope<'turn.started', TurnStartedPayload>
  | Envelope<'turn.finished', TurnFinishedPayload>
  | Envelope<'model.text_delta', TextDeltaPayload>
  | Envelope<'model.reasoning_delta', TextDeltaPayload>
  | Envelope<'model.response_completed', ResponseCompletedPayload>
  | Envelope<'model.request_failed', FailurePayload>
  | Envelope<'model.request_retrying', RequestRetryingPayload>
  | Envelope<'tool.prepared', ToolPreparedPayload>
  | Envelope<'tool.started', Record<string, unknown>>
  | Envelope<'tool.completed', ToolCompletedPayload>
  | Envelope<'approval.requested', ApprovalRequestedPayload>
  | Envelope<'approval.resolved', ApprovalResolvedPayload>
  | Envelope<'question.asked', QuestionPayload>
  | Envelope<'question.answered', QuestionAnsweredPayload>
  | Envelope<'steer.queued', SteerQueuedPayload>
  | Envelope<'steer.injected', SteerInjectedPayload>
  | Envelope<'run.cancel_requested', Record<string, unknown>>
  | Envelope<'run.cancelled', Record<string, unknown>>
  | Envelope<'context.usage', ContextUsagePayload>
  | Envelope<'context.compacted', ContextCompactedPayload>
  | Envelope<'budget.updated', TokenUsage>
  | Envelope<'budget.notice', BudgetNoticePayload>
  | Envelope<'plan.updated', Plan>
  | Envelope<'reasoning.changed', ReasoningChangedPayload>
  | Envelope<'runtime.warning', RuntimeMessagePayload>
  | Envelope<'runtime.fatal', RuntimeMessagePayload>
  | Envelope<'subagent.started', SubagentPayload>
  | Envelope<'subagent.finished', SubagentPayload>
// 注：不加「未知 kind」兜底成员——它会破坏 switch 按 kind 的类型收窄。
// wire 上的未知 kind 由运行时 default 分支忽略（契约第 2 条）。
