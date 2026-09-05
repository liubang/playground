// TranscriptView.tsx — message stream view: renders the block model it subscribes to from the TranscriptController.
// Scroll following: streaming deltas respect `following` (never disturb a user reading up-scroll); new blocks/cards
// force a snap to bottom (controller-side forceFollow semantics). Block components are memoized by reference — the
// controller keeps object identity for unchanged blocks, so re-renders only happen on blocks whose version changed.
//
// Virtual scrolling: for long sessions only blocks inside the viewport (+ overscan) are rendered, reducing DOM nodes from O(N) to
// O(viewport). Block heights are measured with ResizeObserver (unmeasured ones use estimates); top/bottom
// padding placeholders keep the scroll height intact; scroll-following and preserveScroll semantics are unchanged.

import {
  memo,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react'
import type { BlockModel, TranscriptController } from '../app/transcript'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import {
  AssistantBlock,
  CompactBlock,
  FatalBlock,
  InterruptedBlock,
  NoticeBlock,
  ReasoningBlock,
  ResolvedNotice,
  StreamBlock,
  ThinkingBlock,
  UserBlock,
} from './blocks/blocks'
import { ToolBlock } from './blocks/ToolBlock'
import { ApprovalCard, QuestionCard } from './blocks/cards'
import { ArtifactBlock, InlineImage } from './blocks/images'
import { useRafScroll } from '../lib/rafScroll'

const FOLLOW_THRESHOLD_PX = 80

// Inter-block spacing (kept in sync with .block-wrap { margin-bottom }).
const BLOCK_GAP_PX = 20
// Extra blocks rendered above/below the viewport to avoid blank flashes while
// scrolling fast.
const OVERSCAN = 6

// Estimated height for unmeasured blocks: tiered by block type + text volume (previously a flat ESTIMATED_H=140,
// underestimating large blocks / overestimating small ones, so the scrollbar jumped noticeably when flipping up fast
// through history). Estimates should err conservative: offsets converge on the next frame once ResizeObserver measures.
function estimateHeight(b: BlockModel): number {
  const wrapLines = (text: string, cols: number) =>
    text.split('\n').reduce((n, l) => n + Math.max(1, Math.ceil(Math.max(l.length, 1) / cols)), 0)
  switch (b.kind) {
    case 'user':
      return 40 + wrapLines(b.text, 60) * 23 + (b.images?.length ?? 0) * 200
    case 'assistant':
    case 'stream': {
      const lines = wrapLines(b.text, 90)
      const fences = (b.text.match(/```/g) || []).length
      return 16 + lines * 26 + Math.ceil(fences / 2) * 20
    }
    case 'reasoning':
      return 46
    case 'thinking':
      return 32
    case 'tool': {
      // The diff may be lazy (snapshot rebuild): estimate roughly from the new_string/content volume
      let diffLines = b.diff ? Math.min(b.diff.split('\n').length, 30) : 0
      if (!b.diff && b.diffArgs) {
        const a = b.diffArgs.args as { new_string?: unknown; content?: unknown }
        const body = typeof a?.new_string === 'string' ? a.new_string : a?.content
        if (typeof body === 'string') diffLines = Math.min(Math.ceil(body.length / 60), 30)
      }
      const prevLines = b.completion?.preview
        ? Math.min(b.completion.preview.split('\n').length, 12)
        : 0
      return 60 + (diffLines + (diffLines ? 8 : 0) + prevLines) * 19
    }
    case 'approval':
      return 300
    case 'question':
      return 220
    case 'image':
    case 'artifact':
      return 340
    default:
      return 48 // notice / resolved / compact / fatal / interrupted
  }
}

// View-layer callback collection (injected by App; the share page passes only the fetchToolOutput subset —
// approvals/questions/feedback don't appear there; pass undefined to skip rendering those interactions)
export interface TranscriptViewIO {
  onResolveApproval?: (
    approvalId: string,
    decision: 'allow' | 'deny',
    always: boolean,
    trust?: string,
  ) => void
  onAnswerQuestion?: (
    questionId: string,
    answer: { selected: string[]; custom_text: string; skipped: boolean },
  ) => void
  onFeedback?: (runId: string, value: 0 | 1) => Promise<unknown>
  fetchToolOutput?: (callId: string) => Promise<string>
}

const BlockView = memo(
  function BlockView({ block, io }: { block: BlockModel; io: TranscriptViewIO }) {
    switch (block.kind) {
      case 'user':
        return <UserBlock text={block.text} createdAt={block.createdAt} images={block.images} />
      case 'assistant':
        return (
          <AssistantBlock text={block.text} actions={block.actions} onFeedback={io.onFeedback} />
        )
      case 'stream':
        return <StreamBlock text={block.text} />
      case 'reasoning':
        return <ReasoningBlock text={block.text} durationMs={block.durationMs} live={block.live} />
      case 'thinking':
        return <ThinkingBlock />
      case 'tool':
        return (
          <ToolBlock
            callId={block.callId}
            toolName={block.toolName}
            target={block.target}
            diff={block.diff}
            diffArgs={block.diffArgs}
            diffSuppressed={block.diffSuppressed}
            completion={block.completion}
            fetchToolOutput={
              io.fetchToolOutput && block.callId
                ? () => io.fetchToolOutput!(block.callId!)
                : undefined
            }
          />
        )
      case 'approval':
        return (
          <ApprovalCard
            payload={block.payload}
            diff={block.diff}
            resolving={block.resolving}
            onResolve={(decision, always, trust) =>
              io.onResolveApproval?.(block.payload.approval_id || '', decision, always, trust)
            }
          />
        )
      case 'question': {
        const qid = block.payload.question_id || block.payload.id || ''
        return (
          // key on question_id: force unmount+remount if the payload identity
          // ever changes, so the controlled selected/custom state can't leak
          // across distinct questions.
          <QuestionCard
            key={qid}
            payload={block.payload}
            resolving={block.resolving}
            onAnswer={(answer) => io.onAnswerQuestion?.(qid, answer)}
          />
        )
      }
      case 'notice':
        return <NoticeBlock text={block.text} warn={block.warn} />
      case 'resolved':
        return <ResolvedNotice ok={block.ok} actor={block.actor} what={block.what} />
      case 'fatal':
        return <FatalBlock text={block.text} />
      case 'interrupted':
        return <InterruptedBlock text={block.text} />
      case 'compact':
        return <CompactBlock payload={block.payload} />
      case 'image':
        return (
          <div className="block block-image">
            <InlineImage mediaType={block.mediaType} data={block.data} />
          </div>
        )
      case 'artifact':
        return <ArtifactBlock artifact={block.artifact} />
      default:
        return null
    }
  },
  (prev, next) => prev.block === next.block && prev.io === next.io,
)

// MeasuredBlock wraps a rendered block and reports its height via ResizeObserver
// so the virtualizer can maintain per-block offsets. The wrapper carries the
// flex-column context (so .block-user's align-self:flex-end still works) and the
// inter-block margin.
//
// seenIds tracks ids that have already been mounted once: the .block fadein
// entrance animation is meant for genuinely new blocks, but virtualization
// remounts blocks every time they re-enter the render window — without this
// gate the animation replays on every scroll, visible as flickering.
function MeasuredBlock({
  id,
  measure,
  seenIds,
  children,
}: {
  id: string
  measure: (id: string, h: number) => void
  seenIds: Set<string>
  children: ReactNode
}) {
  const ref = useRef<HTMLDivElement>(null)
  const seen = seenIds.has(id)
  useLayoutEffect(() => {
    seenIds.add(id)
    const el = ref.current
    if (!el) return
    const ro = new ResizeObserver(() => {
      measure(id, el.getBoundingClientRect().height)
    })
    ro.observe(el)
    return () => ro.disconnect()
  }, [id, measure, seenIds])
  return (
    <div ref={ref} className={seen ? 'block-wrap seen' : 'block-wrap'} data-block-id={id}>
      {children}
    </div>
  )
}

function lowerBound(offsets: number[], value: number): number {
  let lo = 0
  let hi = offsets.length
  while (lo < hi) {
    const mid = (lo + hi) >>> 1
    if (offsets[mid] < value) lo = mid + 1
    else hi = mid
  }
  return lo
}

export interface EmptyState {
  hidden: boolean
  hint: string
  showAddWs: boolean
  onAddWs: () => void
}

export function TranscriptView({
  controller,
  io,
  empty,
  className,
  scrollerOut,
  loading,
  children,
}: {
  controller: TranscriptController
  io: TranscriptViewIO
  empty?: EmptyState
  className?: string
  // scrollerOut: lets the caller hold the scroll container reference (used to preserve scroll position on resync)
  scrollerOut?: { el: HTMLDivElement | null }
  // sessionLoading (snapshot fetch in flight): when old content exists, overlay it semi-transparently; when there are
  // no blocks, show a skeleton screen — avoids a "click did nothing" blank wait while a large snapshot loads
  loading?: boolean
  children?: ReactNode
}) {
  const blocks = useStore(controller.store, (s) => s.blocks)
  const following = useStore(controller.store, (s) => s.following)
  const followSeq = useStore(controller.store, (s) => s.followSeq)
  const scrollerRef = useRef<HTMLDivElement>(null)
  // Ids that have entered the render window at least once (fadein gate — see
  // MeasuredBlock). Lives here so it survives block remounts.
  const seenIdsRef = useRef<Set<string>>(new Set())

  // --- virtual scroll state ---
  const heightsRef = useRef<Map<string, number>>(new Map())
  // Height-estimate cache: estimateHeight does a full-text scan (split/regex) on unmeasured blocks, while the visible-range
  // computation recalculates it for all unmeasured blocks on every scroll/measure frame — when a block's version v is
  // unchanged, reuse the previous estimate directly.
  const estRef = useRef<Map<string, { v: number; h: number }>>(new Map())
  const [measureTick, setMeasureTick] = useState(0)
  const [viewport, setViewport] = useState({ top: 0, height: 0 })

  // Block height: the measured value wins; unmeasured blocks use the estimate cache (a stream block's v grows with text, so its cache entry invalidates automatically).
  const heightOf = useCallback((b: BlockModel) => {
    const measured = heightsRef.current.get(b.id)
    if (measured != null) return measured
    const cached = estRef.current.get(b.id)
    if (cached && cached.v === b.v) return cached.h
    const h = estimateHeight(b)
    estRef.current.set(b.id, { v: b.v, h })
    return h
  }, [])

  // --- search state (opened/closed by the TranscriptSearch bar) ---
  const search = useStore(controller.store, (s) => s.search)
  const searchNavSeq = search?.navSeq ?? 0

  // Block ids are globally monotonically increasing and never reused: not clearing heights/seenIds on session switch
  // is a slow leak (old session ids occupy the Maps forever). Clear them outright when blocks are emptied (before
  // clear/rebuild); while running, grow-prune ids that no longer exist once above the threshold.
  useLayoutEffect(() => {
    const heights = heightsRef.current
    const ests = estRef.current
    const seen = seenIdsRef.current
    if (blocks.length === 0) {
      heights.clear()
      ests.clear()
      seen.clear()
      return
    }
    if (seen.size <= blocks.length + 200) return
    const ids = new Set(blocks.map((b) => b.id))
    for (const id of [...heights.keys()]) if (!ids.has(id)) heights.delete(id)
    for (const id of [...ests.keys()]) if (!ids.has(id)) ests.delete(id)
    for (const id of [...seen]) if (!ids.has(id)) seen.delete(id)
  }, [blocks])

  const measure = useCallback(
    (id: string, h: number) => {
      const map = heightsRef.current
      const prev = map.get(id)
      if (prev == null || Math.abs(prev - h) > 1) {
        map.set(id, h)
        setMeasureTick((t) => t + 1)
        // Pure-DOM resizes of the last block (e.g. the reasoning <details>
        // being toggled) produce no store update, so no follow request is
        // scheduled — re-snap here when following to keep the bottom pinned.
        const st = controller.store.get()
        if (st.following && st.blocks[st.blocks.length - 1]?.id === id) {
          controller.followNow()
        }
      }
    },
    [controller],
  )

  // Track the container size and initial scroll position.
  useLayoutEffect(() => {
    const el = scrollerRef.current
    if (!el) return
    const update = () => setViewport({ top: el.scrollTop, height: el.clientHeight })
    update()
    const ro = new ResizeObserver(update)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  useLayoutEffect(() => {
    if (!scrollerOut) return
    scrollerOut.el = scrollerRef.current
    return () => {
      scrollerOut.el = null
    }
  }, [scrollerOut])

  // Snap-to-bottom requests: the controller's rAF callback bumps followSeq; this consumes it
  useLayoutEffect(() => {
    if (followSeq === 0) return
    const el = scrollerRef.current
    if (!el) return
    el.scrollTop = el.scrollHeight
    // Sync the virtualizer's viewport immediately instead of waiting for the
    // async scroll event: the blocks at the snap target must enter the render
    // range in this same commit, otherwise the bottom block can stay
    // unrendered (blank) until the browser dispatches the scroll event.
    setViewport({ top: el.scrollTop, height: el.clientHeight })
  }, [followSeq])

  // Preserve scroll on resync: AppController writes preserveScrollTop before applySnapshot; it's restored here
  // after the DOM rebuild (content is from the same source, so scrollTop stays approximately valid)
  useLayoutEffect(() => {
    const t = controller.store.get().preserveScrollTop
    if (t != null && scrollerRef.current) {
      scrollerRef.current.scrollTop = t
      setViewport({ top: t, height: scrollerRef.current.clientHeight })
      controller.store.update((s) => {
        s.preserveScrollTop = null
      })
    }
  }, [blocks, controller])

  // Search navigation: navSeq changes (query change / paging) → scroll to the current hit block and flash a hint.
  // Only navSeq increments count: streaming output rebuilds the blocks/search objects every frame; without limiting
  // to navSeq, the user inspecting hits would be yanked back to the match by every new token.
  const lastNavSeqRef = useRef(0)
  useLayoutEffect(() => {
    // Reset to zero after search closes (Esc / session switch): the next session's navSeq recounts from 1
    // and must not collide with a stale value
    if (!search) {
      lastNavSeqRef.current = 0
      return
    }
    if (searchNavSeq === 0 || searchNavSeq === lastNavSeqRef.current) return
    lastNavSeqRef.current = searchNavSeq
    const targetId = search.matches[search.index]
    if (!targetId) return
    const el = scrollerRef.current
    if (!el) return
    // The target block may never have entered the render window (unmeasured): accumulate with the same estimates the virtualizer uses
    let offset: number | null = null
    let acc = 0
    for (const b of blocks) {
      if (b.id === targetId) {
        offset = acc
        break
      }
      acc += heightOf(b) + BLOCK_GAP_PX
    }
    if (offset == null) return
    // Jumping to a historical hit: detach from bottom following (streaming output shouldn't steal the view during search browsing)
    controller.store.update((s) => {
      s.following = false
    })
    el.scrollTop = Math.max(0, offset - 80)
    setViewport({ top: el.scrollTop, height: el.clientHeight })
    // Quick flash to mark the hit position. The target block may just have entered the render window: wait two frames
    // (virtualizer commit + browser render) before looking for the DOM node.
    let timer = 0
    const raf = requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        const wrap = el.querySelector(`[data-block-id="${targetId}"]`)
        if (!wrap) return
        wrap.classList.remove('search-flash')
        void (wrap as HTMLElement).offsetWidth // reflow: replays the animation on consecutive jumps to the same block
        wrap.classList.add('search-flash')
        timer = window.setTimeout(() => wrap.classList.remove('search-flash'), 1300)
      })
    })
    return () => {
      cancelAnimationFrame(raf)
      if (timer) clearTimeout(timer)
    }
  }, [searchNavSeq, search, blocks, controller, heightOf])

  // During streaming output: quietly refresh the hit count (new blocks may match the current query; no scrolling is triggered).
  useEffect(() => {
    if (!search?.query) return
    const t = setTimeout(() => controller.refreshSearch(), 300)
    return () => clearTimeout(t)
  }, [blocks, search, controller])

  // --- visible range computation ---
  const { renderStart, renderEnd, topPad, bottomPad } = useMemo(() => {
    const n = blocks.length
    if (n === 0) return { renderStart: 0, renderEnd: 0, topPad: 0, bottomPad: 0 }
    // offsets[i] = top offset of block i = sum(heights[0..i-1]) + i*GAP
    const offsets: number[] = new Array(n)
    let acc = 0
    for (let i = 0; i < n; i++) {
      offsets[i] = acc
      acc += heightOf(blocks[i]) + BLOCK_GAP_PX
    }
    // total content height = sum(heights) + (n-1)*GAP
    const lastH = heightOf(blocks[n - 1])
    const total = acc - BLOCK_GAP_PX + lastH

    const top = viewport.top
    const bottom = top + viewport.height
    const start = Math.max(0, lowerBound(offsets, top) - 1)
    const end = lowerBound(offsets, bottom)
    const rs = Math.max(0, start - OVERSCAN)
    const re = Math.min(n, end + OVERSCAN)

    const topPad = rs > 0 ? offsets[rs] : 0
    const bottomPad = re < n ? total - offsets[re] : 0
    return { renderStart: rs, renderEnd: re, topPad, bottomPad }
  }, [blocks, viewport, measureTick, heightOf])

  const visible = useMemo(
    () => blocks.slice(renderStart, renderEnd),
    [blocks, renderStart, renderEnd],
  )

  const cls =
    'transcript' +
    (className ? ' ' + className : '') +
    (loading && blocks.length > 0 ? ' is-loading' : '')
  return (
    <div
      id="transcript"
      className={cls}
      ref={scrollerRef}
      onScroll={useRafScroll<HTMLDivElement>((el) => {
        setViewport({ top: el.scrollTop, height: el.clientHeight })
        const gap = el.scrollHeight - el.scrollTop - el.clientHeight
        controller.setFollowing(gap < FOLLOW_THRESHOLD_PX)
      })}
    >
      <div id="blocks" className="transcript-inner">
        {loading && blocks.length === 0 && (
          <div className="skeleton-rows" aria-hidden="true">
            <div className="sk-row sk-w-40" />
            <div className="sk-row sk-w-85" />
            <div className="sk-row sk-w-70" />
            <div className="sk-row sk-w-55" />
            <div className="sk-row sk-w-85" />
          </div>
        )}
        {topPad > 0 && <div style={{ height: topPad }} aria-hidden="true" />}
        {visible.map((b) => (
          <MeasuredBlock key={b.id} id={b.id} measure={measure} seenIds={seenIdsRef.current}>
            <BlockView block={b} io={io} />
          </MeasuredBlock>
        ))}
        {bottomPad > 0 && <div style={{ height: bottomPad }} aria-hidden="true" />}
      </div>
      {children}
      {empty && (
        <div id="empty-state" className="empty-state" hidden={empty.hidden}>
          <div className="brand">◆ loom</div>
          <p id="empty-hint">{empty.hint}</p>
          <p>
            <button
              id="empty-add-ws"
              className="btn"
              type="button"
              hidden={!empty.showAddWs}
              onClick={empty.onAddWs}
            >
              Add workspace&hellip;
            </button>
          </p>
        </div>
      )}
      <button
        id="follow-btn"
        className="follow-btn"
        hidden={following}
        onClick={() => controller.followNow()}
      >
        <Icon name="arrow-down" /> 回到底部
      </button>
    </div>
  )
}

// TranscriptSearch — in-session full-text search bar (mounted at the top of App's chat-pane,
// summoned via Cmd/Ctrl+F). The input text is local state, debounced 150ms before being written to the controller;
// scroll positioning and flash happen inside TranscriptView (it's the only one that knows the virtualization geometry).
export function TranscriptSearch({ controller }: { controller: TranscriptController }) {
  const search = useStore(controller.store, (s) => s.search)
  const [q, setQ] = useState('')
  const lastSentRef = useRef('')
  const inputRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    inputRef.current?.focus()
  }, [])

  // Debounced write to the controller; only react to real q changes (the search object is rebuilt after each submit,
  // so it must not be a trigger source, or it would cause one extra duplicate submit + scroll)
  useEffect(() => {
    if (!search || q === lastSentRef.current) return
    const t = setTimeout(() => {
      lastSentRef.current = q
      controller.setSearchQuery(q)
    }, 150)
    return () => clearTimeout(t)
  }, [q, search, controller])

  if (!search) return null
  const total = search.matches.length
  return (
    <div className="transcript-search" role="search">
      <input
        ref={inputRef}
        className="ts-input"
        type="search"
        placeholder="搜索本会话…"
        value={q}
        onChange={(e) => setQ(e.target.value)}
        onKeyDown={(e) => {
          if (e.nativeEvent.isComposing) return // Esc/Enter yield to the IME candidate window first
          if (e.key === 'Enter') {
            e.preventDefault()
            controller.searchNav(e.shiftKey ? -1 : 1)
          } else if (e.key === 'Escape') {
            e.stopPropagation() // don't bubble to the global Esc (cancel turn) handler
            controller.closeSearch()
          }
        }}
      />
      <span className="ts-count">{total > 0 ? `${search.index + 1}/${total}` : '无结果'}</span>
      <button
        type="button"
        className="icon-btn sm"
        title="上一个（Shift+Enter）"
        disabled={total === 0}
        onClick={() => controller.searchNav(-1)}
      >
        <Icon name="arrow-up" />
      </button>
      <button
        type="button"
        className="icon-btn sm"
        title="下一个（Enter）"
        disabled={total === 0}
        onClick={() => controller.searchNav(1)}
      >
        <Icon name="arrow-down" />
      </button>
      <button
        type="button"
        className="icon-btn sm"
        title="关闭（Esc）"
        onClick={() => controller.closeSearch()}
      >
        <Icon name="xmark" />
      </button>
    </div>
  )
}
