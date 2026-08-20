// transcript.ts — 消息流控制器（框架无关）：事件 → 块模型调度。
// 状态来源仅 snapshot（首屏）+ SSE（此后全部）；未知 kind 一律忽略。
// 逻辑与旧 static/js/components/transcript.js 一一对应，但产出数据模型
// （BlockModel）而非直接操作 DOM；React 视图层（components/TranscriptView）
// 订阅模型渲染。流式节流曲线与旧版一致（60ms 最小间隔 + rAF 合帧）。

import type {
  ApprovalRequestedPayload,
  ArtifactRef,
  ContextCompactedPayload,
  FailurePayload,
  ImagePayload,
  QuestionPayload,
  RuntimeEvent,
  ToolCompletedPayload,
} from '../protocol/events'
import type { Message, Snapshot, ToolResult } from '../protocol/types'
import { diffForToolCall } from '../lib/diff'
import { Store } from '../store/store'

// --- 块模型 ---

// 用户消息的图片附件：artifact 引用（{id, size}）或内联 base64（{data}）。
export type UserImage = ArtifactRef | ImagePayload

export function isInlineImage(img: UserImage): img is ImagePayload {
  return (img as ImagePayload).data != null
}

export interface ToolCompletion {
  status?: string
  duration_ms?: number
  preview?: string
  full_text?: string
  error_message?: string
  error?: string
  images?: { media_type: string; data: string }[]
  artifacts?: ArtifactRef[]
}

export interface AssistantActionContext {
  createdAt?: string
  runId?: string
  feedback?: string // "up" | "down" | ""
}

interface Base {
  id: string
  v: number // 变更版本号：React memo 的比较依据
}

// 分布式 Omit：对联合成员逐个 Omit（直接 Omit 会把联合塌缩成公共属性）
type DistributiveOmit<T, K extends keyof never> = T extends unknown ? Omit<T, K> : never

export type BlockModel = Base &
  (
    | { kind: 'user'; text: string; createdAt?: string; images?: UserImage[] }
    | { kind: 'assistant'; text: string; actions?: AssistantActionContext }
    | { kind: 'stream'; text: string }
    | { kind: 'reasoning'; text: string }
    | { kind: 'thinking' }
    | {
        kind: 'tool'
        callId?: string
        toolName: string
        target?: string
        diff?: string
        diffSuppressed?: boolean // 审批期间 diff 移入审批卡片展示
        completion?: ToolCompletion
      }
    | { kind: 'approval'; payload: ApprovalRequestedPayload; diff?: string; resolving?: boolean }
    | { kind: 'question'; payload: QuestionPayload; resolving?: boolean }
    | { kind: 'notice'; text: string; warn?: boolean }
    | { kind: 'resolved'; ok: boolean; actor: string; what: string }
    | { kind: 'fatal'; text: string }
    | { kind: 'interrupted'; text: string }
    | { kind: 'compact'; payload: ContextCompactedPayload }
    | { kind: 'image'; mediaType: string; data: string }
    | { kind: 'artifact'; artifact: ArtifactRef }
  )

export interface TranscriptState {
  blocks: BlockModel[]
  // 滚动跟随意图：视图层据此决定是否在渲染后回底。
  // forceFollow 为新块强制回底（rAF 前不被用户滚动取消）；following 为流式跟随。
  following: boolean
  followSeq: number // 每次请求回底递增（视图层 effect 触发器）
  // resync 保留滚动：applySnapshot(preserveScroll) 前由调用方写入重建前
  // 的 scrollTop，视图层在 DOM 重建后恢复并清零。
  preserveScrollTop?: number | null
}

// --- IO 依赖（由 AppController 注入） ---

export interface TranscriptIO {
  resolveApproval: (
    payload: ApprovalRequestedPayload,
    opts: { decision: 'allow' | 'deny'; always: boolean },
  ) => Promise<unknown>
  answerQuestion: (questionId: string, answer: unknown) => Promise<unknown>
  sendFeedback?: (runId: string, value: 0 | 1) => Promise<unknown>
  getFeedback?: (runId: string) => string
  onError: (e: Error & { code?: string }) => void
}

let nextBlockId = 1

const STREAM_RENDER_MIN_INTERVAL_MS = 60

export class TranscriptController {
  readonly store = new Store<TranscriptState>({
    blocks: [],
    following: true,
    followSeq: 0,
  })

  private io: TranscriptIO
  private streamId: string | null = null // 当前 stream 块 id
  private streamBuf = ''
  private streamScheduled = false
  private streamDestroyed = false
  private streamLastRender = 0
  private reasoningId: string | null = null
  private thinkingId: string | null = null
  private tools = new Map<string, string>() // call_id → block id
  private approvals = new Map<string, string>() // approval_id → block id
  private questions = new Map<string, string>() // question_id → block id
  private steers: { id: string; text: string }[] = [] // pending steer notice（FIFO）
  private followups: { id: string; text: string }[] = [] // pending followup notice（FIFO）
  private forceFollow = false
  private pendingStreamTs = '' // 首个 text_delta 事件时间，收笔注入操作行
  private turnAssistantId: string | null = null // 本轮最新 assistant 块（轮结束挂操作行）
  private turnAssistantTs = ''
  private turnRunID = '' // 本轮 run id（反馈投票目标）
  private turnErrorShown = false

  constructor(io: TranscriptIO) {
    this.io = io
  }

  // --- 滚动跟随（视图层上报/消费） ---

  // 视图层滚动事件上报：距底 < 阈值视为跟随中。
  // 值未变时不 emit（滚动事件高频，避免无效广播）。
  setFollowing(following: boolean) {
    if (this.store.get().following === following) return
    this.store.update((s) => {
      s.following = following
    })
  }

  // 请求渲染后回底。force=true（新块/审批卡/问答卡）：强制回底意图先记下，
  // 即使 rAF 回调前用户滚动事件把 following 置回 false 也照样滚动。
  private requestFollow(force: boolean) {
    if (force) this.forceFollow = true
    if (!this.store.get().following && !this.forceFollow) return
    requestAnimationFrame(() => {
      const follow = this.forceFollow || this.store.get().following
      this.forceFollow = false
      if (follow) {
        this.store.update((s) => {
          s.followSeq++
        })
      }
    })
  }

  // 跟随按钮点击：立即钉底。
  followNow() {
    this.store.update((s) => {
      s.following = true
      s.followSeq++
    })
  }

  // --- 块操作 ---

  private append(block: DistributiveOmit<BlockModel, 'id' | 'v'>): string {
    const id = 'b' + nextBlockId++
    this.store.update((s) => {
      s.blocks = [...s.blocks, { ...block, id, v: 0 } as BlockModel]
      s.following = true // 新块到达视为收到新对话：强制回到底部
    })
    this.requestFollow(true)
    return id
  }

  private patchBlock(id: string, patch: Partial<BlockModel>) {
    this.store.update((s) => {
      s.blocks = s.blocks.map((b) =>
        b.id === id ? ({ ...b, ...patch, v: b.v + 1 } as BlockModel) : b,
      )
    })
  }

  private removeBlock(id: string | null) {
    if (!id) return
    this.store.update((s) => {
      s.blocks = s.blocks.filter((b) => b.id !== id)
    })
  }

  clear() {
    this.streamId = null
    this.streamBuf = ''
    this.streamScheduled = false
    this.streamDestroyed = false
    this.reasoningId = null
    this.thinkingId = null
    this.tools.clear()
    this.approvals.clear()
    this.questions.clear()
    this.steers = []
    this.followups = []
    this.forceFollow = false
    this.pendingStreamTs = ''
    this.turnAssistantId = null
    this.turnAssistantTs = ''
    this.turnRunID = ''
    this.turnErrorShown = false
    this.store.update((s) => {
      s.blocks = []
      s.following = true
    })
  }

  // --- 反馈上下文：runId 为空（无 trace 的旧消息）时不渲染赞/踩 ---
  private fbAction(runId: string): AssistantActionContext | undefined {
    if (!runId || !this.io.sendFeedback) return undefined
    return {
      runId,
      feedback: this.io.getFeedback ? this.io.getFeedback(runId) : '',
    }
  }

  // 轮结束时把操作行（复制/反馈 + 时间）挂到本轮末段 assistant 块上。
  private attachTurnActions() {
    if (!this.turnAssistantId) return
    const createdAt = this.turnAssistantTs
    const fb = this.fbAction(this.turnRunID)
    this.store.update((s) => {
      s.blocks = s.blocks.map((b) => {
        if (b.id !== this.turnAssistantId || (b.kind !== 'assistant' && b.kind !== 'stream')) {
          return b
        }
        if (b.kind === 'assistant' && b.actions) return b // 幂等
        return { ...b, kind: 'assistant', text: b.text, actions: { createdAt, ...fb }, v: b.v + 1 }
      })
    })
  }

  // --- thinking 动画（等待模型输出） ---

  private showThinking() {
    if (this.thinkingId) return
    this.thinkingId = this.append({ kind: 'thinking' })
  }

  private hideThinking() {
    this.removeBlock(this.thinkingId)
    this.thinkingId = null
  }

  // --- 首屏：snapshot → 块（§3.3） ---

  // preserveScroll（同会话 resync/重连重建）：用户上翻阅读时保留滚动位置，
  // 不再无条件回底。返回是否发生了「保留滚动」——视图层据此恢复 scrollTop。
  applySnapshot(snap: Snapshot, { preserveScroll = false } = {}): { preserved: boolean } {
    const wasFollowing = this.store.get().following
    this.clear()
    const histTools = new Map<string, string>() // call_id → block id
    let lastAssistantId: string | null = null
    let lastAssistantText = ''
    let lastTs = ''
    let lastRunId = ''

    const closeTurn = () => {
      if (!lastAssistantId) return
      const createdAt = lastTs
      const fb = this.fbAction(lastRunId)
      const id = lastAssistantId
      this.store.update((s) => {
        s.blocks = s.blocks.map((b) =>
          b.id === id && b.kind === 'assistant'
            ? { ...b, actions: { createdAt, ...fb }, v: b.v + 1 }
            : b,
        )
      })
      lastAssistantId = null
    }

    for (const m of snap.messages || []) {
      let textRun: string[] = []
      // 用户消息的图片附件：收集后随文字一起渲染进气泡（图在文字上方），
      // 而不是作为独立块挂在气泡后面。
      let userImages: UserImage[] = []
      const createdAt = m.created_at || ''
      const flushText = () => {
        if (textRun.length === 0 && userImages.length === 0) return
        const text = textRun.join('\n')
        const images = userImages
        textRun = []
        userImages = []
        if (m.role === 'user') {
          closeTurn()
          this.append({ kind: 'user', text, createdAt, images })
        } else {
          // 反馈目标：消息落盘时 agent loop 已打上 run_id metadata
          const runId = (m.metadata && m.metadata.run_id) || ''
          if (runId) lastRunId = runId
          if (m.status === 'interrupted') {
            // 流式中断的残段消息：渲染为持久的中断块，而不是普通
            // assistant 块——否则错误痕迹在切换会话后凭空消失
            lastAssistantId = this.append({
              kind: 'interrupted',
              text: (text ? text + '\n' : '') + '[interrupted]',
            })
            lastAssistantText = ''
          } else {
            lastAssistantId = this.append({ kind: 'assistant', text })
            lastAssistantText = text
          }
          lastTs = createdAt
        }
      }
      for (const p of m.parts || []) {
        switch (p.kind) {
          case 'text':
            if (p.text) textRun.push(p.text)
            break
          case 'reasoning':
            flushText()
            if (p.reasoning?.text) {
              this.append({ kind: 'reasoning', text: p.reasoning.text })
            }
            break
          case 'tool_call': {
            flushText()
            if (p.tool_call) {
              // diff 不落盘（只在实时 tool.prepared 载荷里）：历史重建时从
              // edit/write 参数本地重算（diff.ts diffForToolCall）
              const diffText = diffForToolCall(p.tool_call.name || '', p.tool_call.arguments)
              const id = this.append({
                kind: 'tool',
                callId: p.tool_call.id,
                toolName: p.tool_call.name || 'tool',
                target: histTarget(p.tool_call),
                diff: diffText || undefined,
              })
              if (p.tool_call.id) {
                histTools.set(p.tool_call.id, id)
                // 同步进实例表：pending 审批卡片要从工具块移入 diff（去重），
                // 且重连截在运行中途时后续 tool.completed 需配对到该块。
                this.tools.set(p.tool_call.id, id)
              }
            }
            break
          }
          case 'tool_result':
            flushText()
            if (p.tool_result) {
              const id = histTools.get(p.tool_result.call_id || '')
              if (id) this.patchBlock(id, { completion: histCompletion(p.tool_result) })
            }
            break
          case 'image':
            // 用户消息的内联 image part（历史数据）同样收进气泡。
            if (m.role === 'user' && p.image) {
              userImages.push({
                media_type: p.image.media_type,
                data: p.image.data,
              })
              break
            }
            flushText()
            if (p.image) {
              this.append({ kind: 'image', mediaType: p.image.media_type, data: p.image.data })
            }
            break
          case 'artifact_ref':
            // 用户附件（artifact 引用）收进气泡；不 flush，保证图随文字同块。
            if (m.role === 'user' && p.artifact && !p.model_only) {
              userImages.push(p.artifact)
              break
            }
            flushText()
            // model_only 的图片只给模型看，展示通道渲染文本引用即可（见
            // histCompletion 的同名过滤）。
            if (p.artifact && !p.model_only) {
              this.append({ kind: 'artifact', artifact: p.artifact })
            }
            break
          default:
            break
        }
      }
      flushText()
    }
    for (const pr of snap.pending_requests || []) {
      if (pr.kind === 'approval' && pr.approval) this.addApprovalCard(pr.approval)
      else if (pr.kind === 'question' && pr.question) this.addQuestionCard(pr.question)
    }
    // pending steer/followup 队列重建（STEER_DESIGN §4.5：snapshot 兜底）
    for (const text of snap.pending_steers || []) this.addSteerNotice(text)
    for (const text of snap.pending_followups || []) this.addFollowupNotice(text)
    // 上一轮失败的持久错误块：实时路径的错误块不随 snapshot 重建，
    // 没有它的话切换会话/刷新后失败痕迹就消失了
    if (snap.last_error && snap.last_error.message) {
      this.append({ kind: 'fatal', text: failureText(snap.last_error) })
      this.turnErrorShown = true
    }
    // 快照可能截在轮次中途：进行中的轮不挂行（留给轮终止事件挂，
    // 状态接力给实时路径）；已完结的轮在此收尾挂行。
    const running =
      snap.state === 'running' || snap.state === 'awaiting_approval' || snap.state === 'cancelling'
    if (running) {
      this.turnAssistantId = lastAssistantId
      this.turnAssistantTs = lastTs
      this.turnRunID = lastRunId
      void lastAssistantText
    } else {
      closeTurn()
    }
    if (preserveScroll && !wasFollowing) {
      // 重建期间的 append 已把 following/forceFollow 置位并挂了回底 rAF；
      // 这些回调在本同步方法结束后才触发，这里复位即可压掉它们，调用方
      // （视图层）负责把视口恢复到重建前的位置。
      this.store.update((s) => {
        s.following = false
      })
      this.forceFollow = false
      return { preserved: true }
    }
    this.store.update((s) => {
      s.followSeq++
    })
    return { preserved: false }
  }

  // --- SSE 事件调度（§5；未知 kind 忽略） ---

  handleEvent(evt: RuntimeEvent) {
    const p = (evt.payload || {}) as Record<string, unknown>
    // run_id 跟随策略：turn.started 的信封 run_id 不可信（发布时新 run
    // 尚未创建——首轮为零值、后续轮带的是上一轮 id），其余事件由 loop 内
    // publishingStore 发出、携带真实 run id，单调跟随最新非空值即可。
    if (evt.kind === 'turn.started') {
      this.turnRunID = ''
    } else if (evt.run_id) {
      this.turnRunID = evt.run_id
    }
    switch (evt.kind) {
      case 'turn.started': {
        this.hideThinking()
        this.turnErrorShown = false
        this.drainSteerNotices((p.prompt as string) || '')
        this.drainFollowupNotices((p.prompt as string) || '')
        // 图片附件随事件载荷实时渲染（artifact 引用，鉴权加载），不再
        // 依赖切会话后的 snapshot 重放才能看到。
        this.append({
          kind: 'user',
          text: (p.prompt as string) || '',
          createdAt: evt.time || '',
          images: (p.images as ArtifactRef[]) || undefined,
        })
        // 新一轮开始：上一轮末尾的操作行已在上轮终止时挂载（作为该轮
        // 结束标志），本轮状态从新起算（run_id 由后续事件带回）
        this.turnAssistantId = null
        this.turnAssistantTs = ''
        this.showThinking()
        break
      }
      case 'turn.finished': {
        this.hideThinking()
        this.finalizeStream()
        this.finalizeReasoning()
        this.attachTurnActions()
        // 兜底展示未被 model.request_failed / runtime.fatal 覆盖的轮级
        // 失败（如持久化错误）；已展示过错误块则不重复
        if (p.error && !this.turnErrorShown) {
          this.append({
            kind: 'fatal',
            text: `turn failed — ${String(p.error || '').slice(0, 300)}`,
          })
          this.turnErrorShown = true
        }
        break
      }
      case 'model.text_delta':
        this.hideThinking()
        // 记录首个 delta 的事件时间，供草稿收笔时注入消息时间提示
        if (!this.pendingStreamTs && evt.time) this.pendingStreamTs = evt.time
        this.streamAppend((p.delta as string) || '')
        this.requestFollow(false)
        break
      case 'model.reasoning_delta':
        this.hideThinking()
        this.reasoningAppend((p.delta as string) || '')
        this.requestFollow(false)
        break
      case 'model.response_completed':
        this.hideThinking()
        // canonical 校正：以 completed.text 整段替换草稿（§3.2 铁律 3）
        if (p.text) {
          this.discardStream()
          this.turnAssistantId = this.append({ kind: 'assistant', text: p.text as string })
          this.turnAssistantTs = evt.time || ''
        } else {
          this.finalizeStream()
        }
        this.finalizeReasoning()
        break
      case 'model.request_failed': {
        this.hideThinking()
        // 与 snapshot.last_error 重建时同款 fatal 块：实时与历史一致
        this.append({ kind: 'fatal', text: failureText(p as FailurePayload) })
        this.turnErrorShown = true
        break
      }
      case 'model.request_retrying': {
        // 限流/瞬态错误：等待重试中。保持 thinking 动画，让轮次看起来
        // 仍然存活，而不是静默卡死
        const waitS = Math.max(1, Math.round(((p.wait_ms as number) || 0) / 1000))
        this.append({
          kind: 'notice',
          text: `model request ${(p.code as string) || 'failed'} — retrying in ${waitS}s (attempt ${(p.attempt as string) || '?'}/${(p.max_attempts as string) || '?'})`,
          warn: true,
        })
        this.showThinking()
        break
      }
      case 'tool.prepared': {
        this.hideThinking()
        const payload = p as unknown as import('../protocol/events').ToolPreparedPayload
        // 去重：迟到/重放的 prepared（旧连接追帧残余、快照已重建过该调用）
        // 不再追加孪生块——否则同一命令会出现一个永远 running 的多余卡片。
        if (payload.call_id && this.tools.has(payload.call_id)) break
        const id = this.append({
          kind: 'tool',
          callId: payload.call_id,
          toolName: payload.tool_name || 'tool',
          target: payload.target,
          diff: payload.diff || undefined,
        })
        if (payload.call_id) this.tools.set(payload.call_id, id)
        break
      }
      case 'tool.started':
        break // 块在 prepared 已建，running 为默认态
      case 'tool.completed': {
        const payload = p as unknown as ToolCompletedPayload
        const id = payload.call_id ? this.tools.get(payload.call_id) : undefined
        if (id) {
          const cur = this.store.get().blocks.find((b) => b.id === id)
          // 幂等：迟到/重放的 tool.completed 不二次追加输出
          if (cur && cur.kind === 'tool' && !cur.completion) {
            this.patchBlock(id, { completion: payload as ToolCompletion })
          }
        }
        // 工具完成后模型会继续思考下一步，重新亮起等待动画
        this.showThinking()
        break
      }
      case 'approval.requested':
        this.hideThinking()
        this.addApprovalCard(p as unknown as ApprovalRequestedPayload)
        break
      case 'approval.resolved':
        this.collapseApproval(
          (p.approval_id as string) || '',
          p.decision === 'allow',
          (p.actor as string) || 'another client',
        )
        this.showThinking()
        break
      case 'question.asked':
        this.hideThinking()
        this.addQuestionCard(p as unknown as QuestionPayload)
        break
      case 'question.answered':
        this.collapseQuestion((p.question_id as string) || '', !!p.skipped)
        this.showThinking()
        break
      case 'steer.queued':
        if (p.queue === 'followup')
          this.addFollowupNotice((p.text as string) || (p.prompt as string) || '')
        else this.addSteerNotice((p.text as string) || (p.prompt as string) || '')
        break
      case 'steer.injected': {
        // cell 严格 FIFO：移除头部第一条 queued notice，转为正式 user block
        const head = this.steers.shift()
        if (head) this.removeBlock(head.id)
        if (p.text) this.append({ kind: 'user', text: p.text as string, createdAt: evt.time || '' })
        this.showThinking()
        break
      }
      case 'run.cancel_requested':
        this.append({ kind: 'notice', text: 'cancelling…' })
        break
      case 'run.cancelled':
        this.hideThinking()
        this.append({ kind: 'notice', text: 'turn cancelled', warn: true })
        this.finalizeStream()
        this.attachTurnActions()
        break
      case 'context.compacted':
        this.append({ kind: 'compact', payload: p as unknown as ContextCompactedPayload })
        break
      case 'budget.notice':
        // 后端已生成具体文案（梯度提醒 / 软着陆），直接展示
        this.append({ kind: 'notice', text: (p.text as string) || 'budget notice', warn: true })
        break
      case 'runtime.warning':
        this.append({
          kind: 'notice',
          text: (p.message as string) || 'runtime warning',
          warn: true,
        })
        break
      case 'runtime.fatal':
        this.hideThinking()
        this.append({ kind: 'fatal', text: (p.message as string) || 'runtime fatal' })
        this.turnErrorShown = true
        this.attachTurnActions()
        break
      case 'subagent.started':
        this.append({
          kind: 'notice',
          text: `subagent started: ${(p.role as string) || (p.session_id as string) || ''}`,
        })
        break
      case 'subagent.finished':
        this.append({
          kind: 'notice',
          text: `subagent finished: ${(p.role as string) || (p.session_id as string) || ''}`,
        })
        break
      default:
        break // 未知 kind：忽略（契约第 2 条）
    }
  }

  // --- pending steer 通知生命周期（queued → injected / turn.started 接力） ---

  private addSteerNotice(text: string) {
    const id = this.append({ kind: 'notice', text: `steer queued: “${text}”` })
    this.steers.push({ id, text })
  }

  private addFollowupNotice(text: string) {
    const id = this.append({
      kind: 'notice',
      text: `followup queued: “${text}” — runs as the next turn`,
    })
    this.followups.push({ id, text })
  }

  // followup 在 turn 边界接力：turn.started 的 prompt 精确等于队首文本时
  // 移除该 notice（每轮只接力一条）；不匹配则说明本轮由别的提交触发。
  private drainFollowupNotices(prompt: string) {
    if (this.followups.length === 0) return
    const head = this.followups[0]
    if (head.text && head.text === prompt) {
      this.removeBlock(head.id)
      this.followups.shift()
    }
  }

  // turn.started 的 prompt 可能是 steer 接力产物（leftover 以 "\n\n" 合并）：
  // 文本命中 queued notice 即移除；未命中的保留（turn 内仍会 steer.injected）。
  private drainSteerNotices(prompt: string) {
    if (this.steers.length === 0) return
    const kept: { id: string; text: string }[] = []
    for (const s of this.steers) {
      if (s.text && prompt.includes(s.text)) this.removeBlock(s.id)
      else kept.push(s)
    }
    this.steers = kept
  }

  // --- stream 草稿块（节流渲染，曲线与旧版一致：60ms 最小间隔 + rAF） ---

  private ensureStream(): string {
    if (!this.streamId) {
      // 新草稿即本轮最新 assistant 块；操作行留待轮终止时统一挂载
      this.streamBuf = ''
      this.streamDestroyed = false
      this.streamLastRender = 0
      this.streamId = this.append({ kind: 'stream', text: '' })
      this.turnAssistantId = this.streamId
      this.turnAssistantTs = this.pendingStreamTs
      this.pendingStreamTs = ''
    }
    return this.streamId
  }

  private streamAppend(delta: string) {
    this.ensureStream()
    this.streamBuf += delta
    if (this.streamScheduled || this.streamDestroyed) return
    this.streamScheduled = true
    const wait = Math.max(
      0,
      STREAM_RENDER_MIN_INTERVAL_MS - (performance.now() - this.streamLastRender),
    )
    setTimeout(() => {
      requestAnimationFrame(() => {
        this.streamScheduled = false
        if (this.streamDestroyed || !this.streamId) return
        this.streamLastRender = performance.now()
        this.patchBlock(this.streamId, { text: this.streamBuf })
      })
    }, wait)
  }

  private finalizeStream() {
    if (!this.streamId) return
    const id = this.streamId
    this.streamId = null
    this.streamDestroyed = true
    this.pendingStreamTs = ''
    // 草稿已是 markdown 实时渲染：收笔做最终渲染即可；空草稿移除
    if (this.streamBuf.trim()) {
      this.patchBlock(id, { text: this.streamBuf })
    } else {
      this.removeBlock(id)
    }
    this.streamBuf = ''
    this.requestFollow(false)
  }

  private discardStream() {
    if (!this.streamId) return
    this.streamDestroyed = true
    this.removeBlock(this.streamId)
    this.streamId = null
    this.streamBuf = ''
    this.pendingStreamTs = ''
  }

  private reasoningAppend(delta: string) {
    if (!this.reasoningId) {
      this.reasoningId = this.append({ kind: 'reasoning', text: '' })
    }
    const id = this.reasoningId
    const cur = this.store.get().blocks.find((b) => b.id === id)
    const text = (cur && cur.kind === 'reasoning' ? cur.text : '') + delta
    this.patchBlock(id, { text })
  }

  private finalizeReasoning() {
    this.reasoningId = null
  }

  // --- 审批 / 问答卡片生命周期 ---

  private addApprovalCard(payload: ApprovalRequestedPayload) {
    if (!payload.approval_id || this.approvals.has(payload.approval_id)) return
    // diff 去重：工具块已渲染的 diff 移入审批卡片展示（数据化表达：
    // 卡片持有 diff 文本副本，工具块标记 suppressed），收编时移回。
    const toolBlockId = payload.call_id ? this.tools.get(payload.call_id) : undefined
    let diff: string | undefined
    if (toolBlockId) {
      const tb = this.store.get().blocks.find((b) => b.id === toolBlockId)
      if (tb && tb.kind === 'tool' && tb.diff) {
        diff = tb.diff
        this.patchBlock(toolBlockId, { diffSuppressed: true })
      }
    }
    const id = this.append({ kind: 'approval', payload, diff })
    this.approvals.set(payload.approval_id, id)
  }

  // 审批按钮回调（视图层调用）
  async resolveApproval(approvalId: string, decision: 'allow' | 'deny', always: boolean) {
    const id = this.approvals.get(approvalId)
    if (!id) return
    const block = this.store.get().blocks.find((b) => b.id === id)
    if (!block || block.kind !== 'approval') return
    this.patchBlock(id, { resolving: true })
    try {
      await this.io.resolveApproval(block.payload, { decision, always })
      this.collapseApproval(approvalId, decision === 'allow', 'you')
    } catch (e) {
      const err = e as Error & { code?: string; status?: number }
      // binding_mismatch / not_idle 都意味着该审批已被处理或已过期
      // （例如同域名的重复申请已被记住的规则自动放行），静默收起即可
      if (err.code === 'binding_mismatch' || err.code === 'not_idle') {
        this.collapseApproval(approvalId, true, 'another client')
      } else {
        this.patchBlock(id, { resolving: false })
        this.io.onError(err)
      }
    }
  }

  private collapseApproval(approvalId: string, allowed: boolean, actor: string) {
    const id = this.approvals.get(approvalId)
    if (!id) return
    this.approvals.delete(approvalId)
    // diff 移回工具块
    const block = this.store.get().blocks.find((b) => b.id === id)
    if (block && block.kind === 'approval' && block.payload.call_id) {
      const toolBlockId = this.tools.get(block.payload.call_id)
      if (toolBlockId) this.patchBlock(toolBlockId, { diffSuppressed: false })
    }
    this.store.update((s) => {
      s.blocks = s.blocks.map((b) =>
        b.id === id
          ? ({
              id: b.id,
              v: b.v + 1,
              kind: 'resolved',
              ok: allowed,
              actor,
              what: 'approval',
            } as BlockModel)
          : b,
      )
    })
    this.requestFollow(false)
  }

  private addQuestionCard(payload: QuestionPayload) {
    const id = payload.question_id || payload.id
    if (!id || this.questions.has(id)) return
    const blockId = this.append({ kind: 'question', payload })
    this.questions.set(id, blockId)
  }

  // 问答提交回调（视图层调用）
  async answerQuestion(questionId: string, answer: unknown, skipped: boolean) {
    const id = this.questions.get(questionId)
    if (!id) return
    this.patchBlock(id, { resolving: true })
    try {
      await this.io.answerQuestion(questionId, answer)
      this.collapseQuestion(questionId, skipped)
    } catch (e) {
      const err = e as Error & { code?: string }
      if (err.code === 'binding_mismatch') {
        this.collapseQuestion(questionId, false)
      } else {
        this.patchBlock(id, { resolving: false })
        this.io.onError(err)
      }
    }
  }

  private collapseQuestion(questionId: string, skipped: boolean) {
    const id = this.questions.get(questionId)
    if (!id) return
    this.questions.delete(questionId)
    this.store.update((s) => {
      s.blocks = s.blocks.map((b) =>
        b.id === id
          ? ({
              id: b.id,
              v: b.v + 1,
              kind: 'notice',
              text: skipped ? 'question skipped' : 'question answered',
            } as BlockModel)
          : b,
      )
    })
    this.requestFollow(false)
  }

  // 反馈投票（视图层调用）：失败抛回由视图层回滚选中态。
  async sendFeedback(runId: string, value: 0 | 1) {
    if (!this.io.sendFeedback) return
    try {
      await this.io.sendFeedback(runId, value)
    } catch (e) {
      this.io.onError(e as Error & { code?: string })
      throw e
    }
  }
}

// --- 历史（snapshot 重建）工具块辅助（与旧 blocks.js 同逻辑） ---

// argv 展示引号规则：与 Go 侧 render.CommandLineForDisplay 完全一致
// （shlex.join 同款安全字符集），保证实时路径与 snapshot 重建路径渲染出
// 相同的命令行。含空白/元字符/引号的元素或空串用单引号包裹。
const DISPLAY_SAFE_ARG = /^[A-Za-z0-9_@%+=:,./-]+$/
function quoteArgForDisplay(arg: string): string {
  if (DISPLAY_SAFE_ARG.test(arg)) return arg
  return `'${arg.replaceAll("'", `'"'"'`)}'`
}

// histTarget 从 tool_call.arguments（wire 上已是 object）提取展示用目标。
// run_cmd 的参数是 program + args，拼成命令行展示（与实时路径、TUI 一致）；
// 其余工具按 path/command/pattern 等已知键提取。
export function histTarget(call: { arguments?: Record<string, unknown> }): string {
  const a = call?.arguments
  if (!a || typeof a !== 'object') return ''
  if (typeof a.program === 'string' && a.program !== '') {
    const rest = Array.isArray(a.args)
      ? a.args.filter((x): x is string => typeof x === 'string')
      : []
    return [a.program, ...rest].map(quoteArgForDisplay).join(' ')
  }
  const v = a.path || a.file_path || a.command || a.cmd || a.query || a.pattern || a.url || ''
  return String(v)
}

// histCompletion 把 ToolResult 映射为 tool completion 载荷。
// preview 有界（600 chars）用于展示；full_text 完整保留用于复制。
export function histCompletion(r: ToolResult): ToolCompletion {
  const status =
    r.status === 'success' ? 'success' : r.status === 'cancelled' ? 'canceled' : 'error'
  const content = r.content || []
  const texts = content.filter((c) => c.kind === 'text' && c.text).map((c) => c.text as string)
  const fullText = texts.join('\n')
  let preview = fullText
  if (preview.length > 600) preview = preview.slice(0, 600) + '\n…'
  let durationMs: number | undefined
  if (r.started_at && r.finished_at) {
    const ms = new Date(r.finished_at).getTime() - new Date(r.started_at).getTime()
    if (Number.isFinite(ms) && ms >= 0) durationMs = ms
  }
  const images = content.filter((c) => c.kind === 'image' && c.image).map((c) => c.image) as {
    media_type: string
    data: string
  }[]
  // model_only 的 artifact（view_image）不进展示通道：文本头已包含路径/
  // 类型/尺寸等审计信息，图片本体只给模型看（展示是 present_image 的职责）。
  const artifacts = content
    .filter(
      (c) => c.kind === 'artifact_ref' && c.artifact && !(c as { model_only?: boolean }).model_only,
    )
    .map((c) => c.artifact) as ArtifactRef[]
  return {
    status,
    duration_ms: durationMs,
    preview,
    full_text: fullText,
    error_message: r.error?.message || '',
    images,
    artifacts,
  }
}

// 失败信息格式化：实时 model.request_failed 事件与 snapshot.last_error
// 共用同一文案，保证切换会话前后看到的错误块一致。
export function failureText(err: FailurePayload): string {
  const detail = (err.message || '').slice(0, 300)
  if (!err.code && !err.stage) return `turn failed — ${detail}`
  const head = `model request failed (${err.stage || 'unknown'}): ${err.code || ''}`
  return detail ? `${head} — ${detail}` : head
}

// Message 类型守卫辅助（snapshot 遍历用）
export type { Message }
