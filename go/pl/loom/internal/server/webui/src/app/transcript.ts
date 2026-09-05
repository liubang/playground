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
        // Lazy diff params for snapshot rebuilds: edit/write history diffs are not in the
        // snapshot, so recompute them locally (LCS 400×400 cap); ToolBlock computes on demand
        // when the block enters the render window instead of batching LCS for 1K-message snapshots
        // synchronously in buildFromSnapshot (once slowed first render). Approval cards compute synchronously once when quoting the diff (low-frequency path).
        diffArgs?: { name: string; args: unknown }
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

// State of in-session full-text search: the view layer (TranscriptSearch) holds a
// local copy of the input text and writes it here debounced on each change; matches
// is the hit block id list, navSeq increments on every query change/page turn — the view layer scrolls to position on it (same mechanism as followSeq).
export interface TranscriptSearchState {
  query: string
  matches: string[]
  index: number
  navSeq: number
}

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
  search: TranscriptSearchState | null
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
    search: null,
  })

  private io: TranscriptIO
  private streamId: string | null = null // current stream block id
  private streamBuf = ''
  private streamScheduled = false
  private streamDestroyed = false
  private streamLastRender = 0
  private reasoningId: string | null = null
  private reasoningStartTs = '' // first delta's event time, for the thinking duration
  // Merged-frame rendering for reasoning, symmetric to streamBuf: high-frequency reasoning_delta
  // no longer triggers a full blocks scan + store emit per delta (previously find+map double O(N), same chained cost as stream).
  private reasoningBuf = ''
  private reasoningScheduled = false
  private reasoningLastRender = 0
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
  // Staging during applySnapshot rebuild: the whole snapshot's blocks pile up in a
  // local array, committed in a single store.update (avoids per-block O(N) array copies and render ticks).
  private batchedBlocks: BlockModel[] | null = null

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
    if (this.batchedBlocks) {
      this.batchedBlocks = [...this.batchedBlocks, { ...block, id, v: 0 } as BlockModel]
    } else {
      this.store.update((s) => {
        s.blocks = [...s.blocks, { ...block, id, v: 0 } as BlockModel]
        s.following = true // a new block means new conversation: force-snap to bottom
      })
      this.requestFollow(true)
    }
    return id
  }

  // findIndex+slice replaces the previous map(): single linear scan and no
  // per-element closure/allocation when the target isn't in the list.
  private patchBlock(id: string, patch: Partial<BlockModel>) {
    const blocks = this.blocksNow()
    const i = blocks.findIndex((b) => b.id === id)
    if (i < 0) return
    const next = blocks.slice()
    next[i] = { ...next[i], ...patch, v: next[i].v + 1 } as BlockModel
    this.setBlocks(next)
  }

  private removeBlock(id: string | null) {
    if (!id) return
    this.setBlocks(this.blocksNow().filter((b) => b.id !== id))
  }

  clear() {
    this.streamId = null
    this.streamBuf = ''
    this.streamScheduled = false
    this.streamDestroyed = false
    this.reasoningId = null
    this.reasoningStartTs = ''
    this.reasoningBuf = ''
    this.reasoningScheduled = false
    this.reasoningLastRender = 0
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
      s.search = null // session switch/clear: search closes with the session
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

  // --- block storage helpers ---

  // Reads/writes of the block list go through these two helpers so that
  // applySnapshot can stage an entire rebuild in a local array and commit it
  // in a single store.update (per-block update+emit on a long snapshot is
  // O(N²) work and one render tick per message).
  private blocksNow(): BlockModel[] {
    return this.batchedBlocks ?? this.store.get().blocks
  }

  private setBlocks(blocks: BlockModel[]): void {
    if (this.batchedBlocks) {
      this.batchedBlocks = blocks
      return
    }
    this.store.update((s) => {
      s.blocks = blocks
    })
  }

  // At turn end, attach the action row (copy/feedback + time) to the final
  // assistant block of the turn.
  private attachTurnActions() {
    if (!this.turnAssistantId) return
    const createdAt = this.turnAssistantTs
    const fb = this.fbAction(this.turnRunID)
    // findIndex+slice like patchBlock, but with a wider kind predicate and an
    // in-place rewriting idempotency guard (map() would allocate for all
    // non-matching blocks).
    const blocks = this.blocksNow()
    const i = blocks.findIndex(
      (b) => b.id === this.turnAssistantId && (b.kind === 'assistant' || b.kind === 'stream'),
    )
    if (i < 0) return
    const b = blocks[i]
    if (b.kind === 'assistant' && b.actions) return // idempotent
    const next = blocks.slice()
    // ...b already carries text (both 'assistant' and 'stream' have it); the
    // explicit re-assignment below only pins the kind/actions/v overrides.
    next[i] = {
      ...b,
      kind: 'assistant',
      actions: { createdAt, ...fb },
      v: b.v + 1,
    } as BlockModel
    this.setBlocks(next)
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
    // Staged rebuild: the whole snapshot's blocks pile up in a local array and commit in
    // one final store.update (previously one update+emit per block — a 1K-message snapshot
    // meant hundreds of O(N) full array copies and render ticks, plus per-block requestFollow stacking into duplicate snap-to-bottom).
    this.batchedBlocks = []
    try {
      this.buildFromSnapshot(snap)
    } finally {
      const staged = this.batchedBlocks
      this.batchedBlocks = null
      if (staged) {
        this.store.update((s) => {
          s.blocks = staged
          s.following = true
        })
      }
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

  private buildFromSnapshot(snap: Snapshot) {
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
      const blocks = this.blocksNow()
      const i = blocks.findIndex((b) => b.kind === 'assistant' && b.id === id)
      if (i >= 0) {
        const b = blocks[i]
        if (b.kind === 'assistant') {
          const next = blocks.slice()
          next[i] = { ...b, actions: { createdAt, ...fb }, v: b.v + 1 }
          this.setBlocks(next)
        }
      }
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
              // tool.prepared payload): rebuild computes them lazily from
              // edit/write arguments when the block becomes visible (see
              // diffArgs on the tool block model; diff.ts diffForToolCall)
              const id = this.append({
                kind: 'tool',
                callId: p.tool_call.id,
                toolName: p.tool_call.name || 'tool',
                target: histTarget(p.tool_call),
                diffArgs: { name: p.tool_call.name || '', args: p.tool_call.arguments },
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
            text: `本轮失败 — ${String(p.error || '').slice(0, 300)}`,
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
          text: `模型请求 ${(p.code as string) || '失败'}，${waitS}s 后重试（第 ${(p.attempt as string) || '?'}/${(p.max_attempts as string) || '?'} 次）`,
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
          // Idempotent: a late/replayed tool.completed must not append output twice.
          // blocksNow() (not store.get()) so the check sees the in-batch state
          // during applySnapshot; single findIndex, patchBlock would re-scan.
          const blocks = this.blocksNow()
          const i = blocks.findIndex((b) => b.id === id)
          const cur = i >= 0 ? blocks[i] : null
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
          (p.actor as string) || '其他客户端',
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
        this.append({ kind: 'notice', text: '正在取消…' })
        break
      case 'run.cancelled':
        this.hideThinking()
        this.append({ kind: 'notice', text: '本轮已取消', warn: true })
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
        this.append({ kind: 'notice', text: (p.text as string) || '预算提醒', warn: true })
        break
      case 'runtime.warning':
        this.append({
          kind: 'notice',
          text: (p.message as string) || '运行警告',
          warn: true,
        })
        break
      case 'runtime.fatal':
        this.hideThinking()
        this.append({ kind: 'fatal', text: (p.message as string) || '严重错误' })
        this.turnErrorShown = true
        this.finalizeReasoning(evt.time || '')
        this.attachTurnActions()
        break
      case 'subagent.started':
        this.append({
          kind: 'notice',
          text: `子代理已启动：${(p.role as string) || (p.session_id as string) || ''}`,
        })
        break
      case 'subagent.finished':
        this.append({
          kind: 'notice',
          text: `子代理已完成：${(p.role as string) || (p.session_id as string) || ''}`,
        })
        break
      default:
        break // unknown kind: ignore (contract clause 2)
    }
  }

  // --- pending steer notice lifecycle (queued → injected / turn.started handoff) ---

  private addSteerNotice(text: string) {
    const id = this.append({ kind: 'notice', text: `已排队干预：“${text}”` })
    this.steers.push({ id, text })
  }

  private addFollowupNotice(text: string) {
    const id = this.append({
      kind: 'notice',
      text: `已排队到下轮：“${text}”，将作为下一轮运行`,
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
    // turn.started's prompt is the remaining steering texts joined by "\n\n":
    // match whole segments so a short text can't substring-hit another notice
    // (e.g. "fix" would consume the notice for "fix the bug").
    const segments = new Set(prompt.split('\n\n').map((t) => t.trim()))
    const kept: { id: string; text: string }[] = []
    for (const s of this.steers) {
      if (s.text && segments.has(s.text.trim())) this.removeBlock(s.id)
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
      this.reasoningBuf = ''
      this.reasoningLastRender = 0
    }
    this.reasoningBuf += delta
    // Merged-frame rendering, symmetric to streamAppend: high-frequency reasoning_delta
    // no longer triggers patchBlock per delta (previously a find+map double O(N) scan per frame).
    if (this.reasoningScheduled) return
    this.reasoningScheduled = true
    const id = this.reasoningId!
    const wait = Math.max(
      0,
      STREAM_RENDER_MIN_INTERVAL_MS - (performance.now() - this.reasoningLastRender),
    )
    setTimeout(() => {
      requestAnimationFrame(() => {
        this.reasoningScheduled = false
        if (this.reasoningId !== id) return // already switched away by finalize/clear
        this.reasoningLastRender = performance.now()
        this.patchBlock(id, { text: this.reasoningBuf })
      })
    }, wait)
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
    const nb = { ...block, id, v: 0 } as BlockModel
    const blocks = this.blocksNow()
    const idx = blocks.findIndex((b) => b.id === streamId)
    this.setBlocks(idx < 0 ? [...blocks, nb] : [...blocks.slice(0, idx), nb, ...blocks.slice(idx)])
    if (!this.batchedBlocks) {
      this.store.update((s) => {
        s.following = true
      })
      this.requestFollow(true)
    }
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
      this.reasoningBuf = ''
      return
    }
    // Fold the not-yet-rendered buffer into this final frame: no further deltas arrive after
    // sealing, and the timer's pending flush is dropped by the reasoningId !== id guard, so it must be applied here.
    const patch: { live: boolean; text?: string; durationMs?: number } = {
      live: false,
      text: this.reasoningBuf,
    }
    if (endTs && this.reasoningStartTs) {
      const ms = Date.parse(endTs) - Date.parse(this.reasoningStartTs)
      if (Number.isFinite(ms) && ms >= 0) patch.durationMs = ms
    }
    this.patchBlock(id, patch)
    this.reasoningStartTs = ''
    this.reasoningBuf = ''
  }

  // Turn-level backstop: any live reasoning block leaked by any path (lost
  // events, reconnect, unknown provider behavior) is force-sealed at the turn
  // boundary — the flicker animation must stop when thinking ends.
  private sweepLiveReasoning() {
    // blocksNow() so the sweep sees the in-batch state during applySnapshot.
    for (const b of this.blocksNow()) {
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
      const tb = this.blocksNow().find((b) => b.id === toolBlockId)
      if (tb && tb.kind === 'tool') {
        // On the reconnect/snapshot path the diff may be lazy (diffArgs): approval
        // cards are low-frequency, so computing once synchronously is an acceptable price
        const d =
          tb.diff ?? (tb.diffArgs ? diffForToolCall(tb.diffArgs.name, tb.diffArgs.args) : '')
        if (d) {
          diff = d
          this.patchBlock(toolBlockId, { diffSuppressed: true })
        }
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
    const block = this.blocksNow().find((b) => b.id === id)
    if (!block || block.kind !== 'approval') return
    this.patchBlock(id, { resolving: true })
    try {
      await this.io.resolveApproval(block.payload, { decision, always, trust })
      this.collapseApproval(approvalId, decision === 'allow', '你')
    } catch (e) {
      const err = e as Error & { code?: string; status?: number }
      // binding_mismatch / not_idle both mean the approval was already handled
      // or expired (e.g. a duplicate request from the same origin was auto-
      // allowed by a remembered rule) — collapse silently
      if (err.code === 'binding_mismatch' || err.code === 'not_idle') {
        this.collapseApproval(approvalId, true, '其他客户端')
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
    const blocks = this.blocksNow()
    const i = blocks.findIndex((b) => b.id === id)
    if (i < 0) return
    const block = blocks[i]
    // Move the diff back to the tool block
    if (block.kind === 'approval' && block.payload.call_id) {
      const toolBlockId = this.tools.get(block.payload.call_id)
      if (toolBlockId) this.patchBlock(toolBlockId, { diffSuppressed: false })
    }
    // Single findIndex+slice in place of the old map(): no allocation when
    // the id isn't present, and only the resolved block gets a new object.
    const next = blocks.slice()
    next[i] = {
      id: block.id,
      v: block.v + 1,
      kind: 'resolved',
      ok: allowed,
      actor,
      what: '审批',
    } as BlockModel
    this.setBlocks(next)
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
    // findIndex+slice (same as collapseApproval / patchBlock): no full-array
    // allocation and goes through setBlocks so applySnapshot batching sees it.
    const blocks = this.blocksNow()
    const i = blocks.findIndex((b) => b.id === id)
    if (i < 0) return
    const next = blocks.slice()
    next[i] = {
      id: next[i].id,
      v: next[i].v + 1,
      kind: 'notice',
      text: skipped ? '问题已跳过' : '问题已回答',
    } as BlockModel
    this.setBlocks(next)
    this.requestFollow(false)
  }

  // --- in-session full-text search ---
  //
  // Under virtual scrolling the browser's Cmd+F can't reach blocks outside the render
  // window — a built-in search must compensate (search BlockModel text and scroll to
  // position). Scope: message text / reasoning / notices / tool name+target+output preview / approval & question card descriptions.

  openSearch() {
    if (this.store.get().search) return
    this.store.update((s) => {
      s.search = { query: '', matches: [], index: 0, navSeq: 0 }
    })
  }

  closeSearch() {
    if (!this.store.get().search) return
    this.store.update((s) => {
      s.search = null
    })
  }

  private computeMatches(query: string): string[] {
    const q = query.trim().toLowerCase()
    if (!q) return []
    return this.store
      .get()
      .blocks.filter((b) => blockSearchText(b).toLowerCase().includes(q))
      .map((b) => b.id)
  }

  // Query change: recompute matches and jump to the first (navSeq bump triggers view-layer scrolling).
  setSearchQuery(query: string) {
    this.store.update((s) => {
      if (!s.search) return
      s.search = {
        query,
        matches: this.computeMatches(query),
        index: 0,
        navSeq: s.search.navSeq + 1,
      }
    })
  }

  // Page through (wraps). Jumping to a match far from the bottom detaches scroll-follow.
  searchNav(delta: 1 | -1) {
    this.store.update((s) => {
      const sh = s.search
      if (!sh || sh.matches.length === 0) return
      const index = (sh.index + delta + sh.matches.length) % sh.matches.length
      s.search = { ...sh, index, navSeq: sh.navSeq + 1 }
    })
  }

  // During streaming new blocks may match the current query: quietly refresh the hit
  // count (no scrolling, don't interrupt the match being viewed — index is kept by id).
  refreshSearch() {
    const sh = this.store.get().search
    if (!sh || !sh.query.trim()) return
    const matches = this.computeMatches(sh.query)
    if (matches.length === sh.matches.length && matches.every((id, i) => id === sh.matches[i]))
      return
    const current = sh.matches[sh.index]
    const index = Math.max(0, matches.indexOf(current))
    this.store.update((s) => {
      if (!s.search) return
      s.search = { ...s.search, matches, index }
    })
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

// blockSearchText extracts a block's searchable text (approval/question cards search by description+target).
export function blockSearchText(b: BlockModel): string {
  switch (b.kind) {
    case 'user':
    case 'assistant':
    case 'stream':
    case 'reasoning':
    case 'notice':
    case 'fatal':
    case 'interrupted':
      return b.text
    case 'tool': {
      // Search is an explicit user action: compute the lazy diff once here (edit content is important corpus)
      const d = b.diff ?? (b.diffArgs ? diffForToolCall(b.diffArgs.name, b.diffArgs.args) : '')
      return [b.toolName, b.target || '', d, b.completion?.preview || ''].join('\n')
    }
    case 'approval':
      return [b.payload.tool_name || '', b.payload.target || '', b.payload.description || ''].join(
        '\n',
      )
    case 'question':
      return b.payload.text || ''
    default:
      return ''
  }
}

// --- history (snapshot rebuild) tool block helpers (same logic as legacy
// blocks.js) ---

// histTarget extracts the display target from tool_call.arguments (already an
// object on the wire): run_cmd's command line lives in `command`; other
// tools are extracted by known keys like path/pattern/query.
export function histTarget(call: { arguments?: Record<string, unknown> }): string {
  const a = call?.arguments
  if (!a || typeof a !== 'object') return ''
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
  if (!err.code && !err.stage) return `本轮失败 — ${detail}`
  const head = `模型请求失败（${err.stage || 'unknown'}）：${err.code || ''}`
  return detail ? `${head} — ${detail}` : head
}

// Message type-guard helper (for snapshot traversal)
export type { Message }
