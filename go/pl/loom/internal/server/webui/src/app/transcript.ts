// transcript.ts — framework-agnostic message-flow controller: event → block
// model dispatch.
// State comes only from snapshot (first paint) + SSE (everything after);
// unknown kinds are always ignored.
// Logic mirrors the legacy static/js/components/transcript.js, but produces a
// data model (BlockModel) instead of touching the DOM; the React view layer
// (components/TranscriptView) subscribes to the model and renders. The
// streaming throttle curve matches the legacy version (60ms min interval + rAF
// batching).

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

// --- block model ---

// Image attachments on user messages: artifact reference ({id, size}) or
// inline base64 ({data}).
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
  v: number // change version: the comparison basis for React memo
}

// Distributive Omit: Omit each union member individually (plain Omit would
// collapse the union to its common properties)
type DistributiveOmit<T, K extends keyof never> = T extends unknown ? Omit<T, K> : never

export type BlockModel = Base &
  (
    | { kind: 'user'; text: string; createdAt?: string; images?: UserImage[] }
    | { kind: 'assistant'; text: string; actions?: AssistantActionContext }
    | { kind: 'stream'; text: string }
    | { kind: 'reasoning'; text: string; durationMs?: number; live?: boolean }
    | { kind: 'thinking' }
    | {
        kind: 'tool'
        callId?: string
        toolName: string
        target?: string
        diff?: string
        diffSuppressed?: boolean // during approval the diff moves into the approval card
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
  // Scroll-follow intent: the view layer uses this to decide whether to snap
  // to bottom after render.
  // forceFollow forces a snap for new blocks (not cancelled by user scrolling
  // before the rAF); following is the streaming follow.
  following: boolean
  followSeq: number // bumped on every snap-to-bottom request (view-layer effect trigger)
  // Preserve scroll on resync: the caller writes the pre-rebuild scrollTop
  // before applySnapshot(preserveScroll); the view layer restores it after the
  // DOM rebuild and clears it.
  preserveScrollTop?: number | null
}

// --- IO dependencies (injected by AppController) ---

export interface TranscriptIO {
  resolveApproval: (
    payload: ApprovalRequestedPayload,
    opts: { decision: 'allow' | 'deny'; always: boolean; trust?: string },
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
  private streamId: string | null = null // current stream block id
  private streamBuf = ''
  private streamScheduled = false
  private streamDestroyed = false
  private streamLastRender = 0
  private reasoningId: string | null = null
  private reasoningStartTs = '' // first delta's event time, for the thinking duration
  private thinkingId: string | null = null
  private tools = new Map<string, string>() // call_id → block id
  private approvals = new Map<string, string>() // approval_id → block id
  private questions = new Map<string, string>() // question_id → block id
  private steers: { id: string; text: string }[] = [] // pending steer notice (FIFO)
  private followups: { id: string; text: string }[] = [] // pending followup notice (FIFO)
  private forceFollow = false
  private pendingStreamTs = '' // first text_delta event time, stamped into the action row at finalize
  private turnAssistantId: string | null = null // latest assistant block of this turn (action row attaches at turn end)
  private turnAssistantTs = ''
  private turnRunID = '' // run id of this turn (feedback vote target)
  private turnErrorShown = false

  constructor(io: TranscriptIO) {
    this.io = io
  }

  // --- scroll following (reported/consumed by the view layer) ---

  // View-layer scroll event report: within threshold of the bottom counts as
  // following.
  // No emit when the value is unchanged (scroll events are high-frequency;
  // avoid useless broadcasts).
  setFollowing(following: boolean) {
    if (this.store.get().following === following) return
    this.store.update((s) => {
      s.following = following
    })
  }

  // Request a snap-to-bottom after render. force=true (new block / approval
  // card / question card): record the forced intent first, so it still scrolls
  // even if a user scroll event flips following back to false before the rAF
  // callback.
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

  // Follow-button click: pin to bottom immediately.
  followNow() {
    this.store.update((s) => {
      s.following = true
      s.followSeq++
    })
  }

  // --- block operations ---

  private append(block: DistributiveOmit<BlockModel, 'id' | 'v'>): string {
    const id = 'b' + nextBlockId++
    this.store.update((s) => {
      s.blocks = [...s.blocks, { ...block, id, v: 0 } as BlockModel]
      s.following = true // a new block means new conversation: force-snap to bottom
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
    this.reasoningStartTs = ''
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

  // --- feedback context: no thumbs up/down when runId is empty (old messages
  // without a trace) ---
  private fbAction(runId: string): AssistantActionContext | undefined {
    if (!runId || !this.io.sendFeedback) return undefined
    return {
      runId,
      feedback: this.io.getFeedback ? this.io.getFeedback(runId) : '',
    }
  }

  // At turn end, attach the action row (copy/feedback + time) to the final
  // assistant block of the turn.
  private attachTurnActions() {
    if (!this.turnAssistantId) return
    const createdAt = this.turnAssistantTs
    const fb = this.fbAction(this.turnRunID)
    this.store.update((s) => {
      s.blocks = s.blocks.map((b) => {
        if (b.id !== this.turnAssistantId || (b.kind !== 'assistant' && b.kind !== 'stream')) {
          return b
        }
        if (b.kind === 'assistant' && b.actions) return b // idempotent
        return { ...b, kind: 'assistant', text: b.text, actions: { createdAt, ...fb }, v: b.v + 1 }
      })
    })
  }

  // --- thinking animation (waiting for model output) ---

  private showThinking() {
    if (this.thinkingId) return
    this.thinkingId = this.append({ kind: 'thinking' })
  }

  private hideThinking() {
    this.removeBlock(this.thinkingId)
    this.thinkingId = null
  }

  // --- first paint: snapshot → blocks (§3.3) ---

  // preserveScroll (same-session resync / reconnect rebuild): keep the scroll
  // position when the user is reading up, instead of unconditionally snapping
  // to bottom. Returns whether scroll was preserved — the view layer restores
  // scrollTop accordingly.
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
      // Image attachments on user messages: collected and rendered into the
      // bubble together with the text (images above text), not as standalone
      // blocks after the bubble.
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
          // Feedback target: the agent loop has stamped run_id metadata when
          // persisting the message
          const runId = (m.metadata && m.metadata.run_id) || ''
          if (runId) lastRunId = runId
          if (m.status === 'interrupted') {
            // Partial message from an interrupted stream: render as a
            // persistent interrupted block, not a normal assistant block —
            // otherwise the error trace vanishes after switching sessions
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
              // duration_ms is persisted with the reasoning part (stamped by the agent
              // at seal time): rebuilt history shows the span too, not just live turns.
              this.append({
                kind: 'reasoning',
                text: p.reasoning.text,
                durationMs: p.reasoning.duration_ms || undefined,
              })
            }
            break
          case 'tool_call': {
            flushText()
            if (p.tool_call) {
              // Diffs are not persisted (they only exist in the live
              // tool.prepared payload): recompute locally from edit/write
              // arguments during history rebuild (diff.ts diffForToolCall)
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
                // Mirror into the instance map: a pending approval card pulls
                // the diff out of the tool block (dedup), and when a reconnect
                // lands mid-run the later tool.completed must pair with this
                // block.
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
            // Inline image parts of user messages (historical data) also go
            // into the bubble.
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
            // User attachments (artifact refs) go into the bubble; no flush,
            // so the images stay in the same block as the text.
            if (m.role === 'user' && p.artifact && !p.model_only) {
              userImages.push(p.artifact)
              break
            }
            flushText()
            // model_only images are for the model only; the display channel
            // renders a text reference (see the same-name filter in
            // histCompletion).
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
    // Rebuild pending steer/followup queues (STEER_DESIGN §4.5: snapshot is
    // the fallback)
    for (const text of snap.pending_steers || []) this.addSteerNotice(text)
    for (const text of snap.pending_followups || []) this.addFollowupNotice(text)
    // Persistent error block for the last failed turn: live-path error blocks
    // are not rebuilt from snapshot — without this, failure traces disappear
    // after switching sessions / refreshing
    if (snap.last_error && snap.last_error.message) {
      this.append({ kind: 'fatal', text: failureText(snap.last_error) })
      this.turnErrorShown = true
    }
    // The snapshot may cut mid-turn: an in-flight turn gets no action row
    // (left to the turn-terminal event; state hands off to the live path); a
    // finished turn is closed out here with its action row.
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
      // Appends during the rebuild already set following/forceFollow and
      // queued snap-to-bottom rAFs; those callbacks fire only after this
      // synchronous method returns, so resetting here suppresses them. The
      // caller (view layer) restores the viewport to its pre-rebuild position.
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

  // --- SSE event dispatch (§5; unknown kinds ignored) ---

  handleEvent(evt: RuntimeEvent) {
    const p = (evt.payload || {}) as Record<string, unknown>
    // run_id follow policy: turn.started's envelope run_id is untrustworthy
    // (at publish time the new run doesn't exist yet — zero value on the first
    // turn, the previous turn's id afterwards); all other events come from the
    // in-loop publishingStore and carry the real run id, so just track the
    // latest non-empty value.
    if (evt.kind === 'turn.started') {
      this.turnRunID = ''
    } else if (evt.run_id) {
      this.turnRunID = evt.run_id
    }
    switch (evt.kind) {
      case 'turn.started': {
        this.hideThinking()
        // New turn: if the previous turn's reasoning leaked its live state due
        // to reconnect/lost events (flicker leak), seal it here — the live flag
        // must never survive across turns.
        this.finalizeReasoning('')
        this.turnErrorShown = false
        this.drainSteerNotices((p.prompt as string) || '')
        this.drainFollowupNotices((p.prompt as string) || '')
        // Image attachments render live from the event payload (artifact refs,
        // authenticated load) — no longer requiring a snapshot replay after
        // switching sessions to become visible.
        this.append({
          kind: 'user',
          text: (p.prompt as string) || '',
          createdAt: evt.time || '',
          images: (p.images as ArtifactRef[]) || undefined,
        })
        // New turn: the previous turn's trailing action row was attached at
        // that turn's end (as its closing marker); this turn's state starts
        // fresh (run_id arrives with later events)
        this.turnAssistantId = null
        this.turnAssistantTs = ''
        this.showThinking()
        break
      }
      case 'turn.finished': {
        this.hideThinking()
        this.finalizeStream()
        this.finalizeReasoning(evt.time || '')
        this.sweepLiveReasoning()
        this.attachTurnActions()
        // Fallback display for turn-level failures not covered by
        // model.request_failed / runtime.fatal (e.g. persistence errors); skip
        // if an error block was already shown
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
        // Body text starting = thinking is over: seal reasoning immediately
        // (live state / duration both end here).
        // Covers the path where "the provider doesn't send response_completed
        // on tool-call responses" — otherwise the thinking block keeps
        // flickering through the entire tool phase.
        this.finalizeReasoning(evt.time || '')
        // Record the first delta's event time, stamped into the message time
        // hint when the draft finalizes
        if (!this.pendingStreamTs && evt.time) this.pendingStreamTs = evt.time
        this.streamAppend((p.delta as string) || '')
        this.requestFollow(false)
        break
      case 'model.reasoning_delta':
        this.hideThinking()
        this.reasoningAppend((p.delta as string) || '', evt.time || '')
        this.requestFollow(false)
        break
      case 'model.response_completed':
        this.hideThinking()
        // Canonical correction: replace the draft wholesale with
        // completed.text (§3.2 iron rule 3)
        if (p.text) {
          this.discardStream()
          this.turnAssistantId = this.append({ kind: 'assistant', text: p.text as string })
          this.turnAssistantTs = evt.time || ''
        } else {
          this.finalizeStream()
        }
        this.finalizeReasoning(evt.time || '')
        break
      case 'model.request_failed': {
        this.hideThinking()
        // Failure also ends thinking: seal reasoning, otherwise the live state
        // leaks into a permanent flicker
        this.finalizeReasoning(evt.time || '')
        this.sweepLiveReasoning()
        // Same fatal block as the snapshot.last_error rebuild: live and
        // history stay consistent
        this.append({ kind: 'fatal', text: failureText(p as FailurePayload) })
        this.turnErrorShown = true
        break
      }
      case 'model.request_retrying': {
        // Rate-limit/transient error: waiting to retry. Keep the thinking
        // animation so the turn still looks alive instead of silently stuck
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
        // Entering the tool phase = thinking is over (tool-call responses may
        // not carry response_completed)
        this.finalizeReasoning(evt.time || '')
        const payload = p as unknown as import('../protocol/events').ToolPreparedPayload
        // Dedup: a late/replayed prepared (catch-up leftovers from the old
        // connection, or a call the snapshot already rebuilt) must not append
        // a twin block — otherwise the same command shows an extra card stuck
        // running forever.
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
        break // block was created at prepared; running is the default state
      case 'tool.completed': {
        const payload = p as unknown as ToolCompletedPayload
        const id = payload.call_id ? this.tools.get(payload.call_id) : undefined
        if (id) {
          const cur = this.store.get().blocks.find((b) => b.id === id)
          // Idempotent: a late/replayed tool.completed must not append output twice
          if (cur && cur.kind === 'tool' && !cur.completion) {
            this.patchBlock(id, { completion: payload as ToolCompletion })
          }
        }
        // After the tool completes the model keeps thinking about the next
        // step — relight the waiting animation
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
        // The cell is strictly FIFO: remove the head queued notice, promote it
        // to a real user block
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
        this.finalizeReasoning(evt.time || '')
        this.attachTurnActions()
        break
      case 'context.compacted':
        this.append({ kind: 'compact', payload: p as unknown as ContextCompactedPayload })
        break
      case 'budget.notice':
        // The backend already produced the concrete copy (graduated reminder /
        // soft landing); display it directly
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
        this.finalizeReasoning(evt.time || '')
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
        break // unknown kind: ignore (contract clause 2)
    }
  }

  // --- pending steer notice lifecycle (queued → injected / turn.started handoff) ---

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

  // Followups hand off at the turn boundary: when turn.started's prompt
  // exactly equals the head notice's text, remove that notice (one handoff per
  // turn); a mismatch means this turn was triggered by another submission.
  private drainFollowupNotices(prompt: string) {
    if (this.followups.length === 0) return
    const head = this.followups[0]
    if (head.text && head.text === prompt) {
      this.removeBlock(head.id)
      this.followups.shift()
    }
  }

  // turn.started's prompt may be a steer handoff product (leftovers joined
  // with "\n\n"): remove queued notices whose text matches; unmatched ones
  // stay (steer.injected still fires within the turn).
  private drainSteerNotices(prompt: string) {
    if (this.steers.length === 0) return
    const kept: { id: string; text: string }[] = []
    for (const s of this.steers) {
      if (s.text && prompt.includes(s.text)) this.removeBlock(s.id)
      else kept.push(s)
    }
    this.steers = kept
  }

  // --- stream draft block (throttled rendering, curve matches legacy: 60ms
  // min interval + rAF) ---

  private ensureStream(): string {
    if (!this.streamId) {
      // A new draft is this turn's latest assistant block; the action row is
      // attached uniformly at turn end
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
    // The draft is already live-rendered markdown: just do the final render
    // at finalize; remove empty drafts
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

  private reasoningAppend(delta: string, ts: string) {
    // The block reasoningId points to may no longer exist (dangling reference
    // after a resync rebuild): recreate it when missing, otherwise later
    // deltas would be silently dropped.
    const alive = this.reasoningId && this.store.get().blocks.some((b) => b.id === this.reasoningId)
    if (!alive) {
      this.reasoningId = this.appendReasoningBlock()
      this.reasoningStartTs = ts
    }
    const id = this.reasoningId!
    const cur = this.store.get().blocks.find((b) => b.id === id)
    const text = (cur && cur.kind === 'reasoning' ? cur.text : '') + delta
    this.patchBlock(id, { text })
  }

  // Under interleaved protocols (OpenAI family: text and reasoning may mix in
  // one chunk), reasoning arriving after body text has started would pile up
  // below the streaming draft if appended at the end; inserting before the
  // draft is correct.
  private appendReasoningBlock(): string {
    const block = { kind: 'reasoning', text: '', live: true } as const
    const streamId = this.streamId
    if (!streamId) return this.append(block)
    const id = 'b' + nextBlockId++
    this.store.update((s) => {
      const nb = { ...block, id, v: 0 } as BlockModel
      const idx = s.blocks.findIndex((b) => b.id === streamId)
      s.blocks =
        idx < 0 ? [...s.blocks, nb] : [...s.blocks.slice(0, idx), nb, ...s.blocks.slice(idx)]
      s.following = true
    })
    this.requestFollow(true)
    return id
  }

  // Seal the reasoning block: clear the live flag (drives the live
  // presentation — head text, tail preview, pulse) and stamp the thinking
  // span (first delta → the terminal event) so the header can show it.
  private finalizeReasoning(endTs = '') {
    const id = this.reasoningId
    this.reasoningId = null
    if (!id) {
      this.reasoningStartTs = ''
      return
    }
    const patch: { live: boolean; durationMs?: number } = { live: false }
    if (endTs && this.reasoningStartTs) {
      const ms = Date.parse(endTs) - Date.parse(this.reasoningStartTs)
      if (Number.isFinite(ms) && ms >= 0) patch.durationMs = ms
    }
    this.patchBlock(id, patch)
    this.reasoningStartTs = ''
  }

  // Turn-level backstop: any live reasoning block leaked by any path (lost
  // events, reconnect, unknown provider behavior) is force-sealed at the turn
  // boundary — the flicker animation must stop when thinking ends.
  private sweepLiveReasoning() {
    for (const b of this.store.get().blocks) {
      if (b.kind === 'reasoning' && b.live) this.patchBlock(b.id, { live: false })
    }
  }

  // --- approval / question card lifecycle ---

  private addApprovalCard(payload: ApprovalRequestedPayload) {
    if (!payload.approval_id || this.approvals.has(payload.approval_id)) return
    // Diff dedup: the diff already rendered by the tool block moves into the
    // approval card (concretely: the card holds a copy of the diff text, the
    // tool block is marked suppressed); it moves back on collapse.
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

  // Approval button callback (invoked by the view layer)
  async resolveApproval(
    approvalId: string,
    decision: 'allow' | 'deny',
    always: boolean,
    trust?: string,
  ) {
    const id = this.approvals.get(approvalId)
    if (!id) return
    const block = this.store.get().blocks.find((b) => b.id === id)
    if (!block || block.kind !== 'approval') return
    this.patchBlock(id, { resolving: true })
    try {
      await this.io.resolveApproval(block.payload, { decision, always, trust })
      this.collapseApproval(approvalId, decision === 'allow', 'you')
    } catch (e) {
      const err = e as Error & { code?: string; status?: number }
      // binding_mismatch / not_idle both mean the approval was already handled
      // or expired (e.g. a duplicate request from the same origin was auto-
      // allowed by a remembered rule) — collapse silently
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
    // Move the diff back to the tool block
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

  // Question submit callback (invoked by the view layer)
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

  // Feedback vote (invoked by the view layer): failures are rethrown so the
  // view layer rolls back the selection state.
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

// --- history (snapshot rebuild) tool block helpers (same logic as legacy
// blocks.js) ---

// argv display quoting rules: exactly match the Go-side
// render.CommandLineForDisplay (same safe charset as shlex.join), so the live
// path and the snapshot rebuild path render identical command lines. Elements
// containing whitespace/metachars/quotes, or empty strings, are wrapped in
// single quotes.
const DISPLAY_SAFE_ARG = /^[A-Za-z0-9_@%+=:,./-]+$/
function quoteArgForDisplay(arg: string): string {
  if (DISPLAY_SAFE_ARG.test(arg)) return arg
  return `'${arg.replaceAll("'", `'"'"'`)}'`
}

// histTarget extracts the display target from tool_call.arguments (already an
// object on the wire).
// run_cmd's arguments are program + args, joined into a command line for
// display (consistent with the live path and the TUI); other tools are
// extracted by known keys like path/command/pattern.
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

// histCompletion maps a ToolResult to a tool completion payload.
// preview is bounded (600 chars) for display; full_text is kept whole for copying.
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
  // model_only artifacts (view_image) skip the display channel: the text
  // header already carries audit info (path/type/dimensions); the image body
  // is for the model only (display is present_image's job).
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

// Failure text formatting: the live model.request_failed event and
// snapshot.last_error share the same copy, so the error block looks identical
// before and after switching sessions.
export function failureText(err: FailurePayload): string {
  const detail = (err.message || '').slice(0, 300)
  if (!err.code && !err.stage) return `turn failed — ${detail}`
  const head = `model request failed (${err.stage || 'unknown'}): ${err.code || ''}`
  return detail ? `${head} — ${detail}` : head
}

// Message type-guard helper (for snapshot traversal)
export type { Message }
