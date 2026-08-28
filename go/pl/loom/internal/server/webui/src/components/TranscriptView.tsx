// TranscriptView.tsx — 消息流视图：订阅 TranscriptController 的块模型渲染。
// 滚动跟随：流式 delta 遵循 following（用户上翻阅读时不打扰）；新块/卡片
// 强制回底（controller 侧 forceFollow 语义）。块组件按引用 memo——controller
// 对未变更的块保持对象引用，重渲只发生在版本号变化的块上。
//
// 虚拟滚动：长会话下只渲染视口内（+ overscan）的块，DOM 节点数从 O(N) 降到
// O(视口)。块高度用 ResizeObserver 实测（未测到的用估计值），顶部/底部用
// padding 占位撑起滚动高度；滚动跟随与 preserveScroll 语义不变。

import {
  memo,
  useCallback,
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
// Height used for blocks not yet measured (conservative underestimate — the
// ResizeObserver corrects it on the next frame, so the scrollbar only briefly
// under-estimates on first paint).
const ESTIMATED_H = 140
// Extra blocks rendered above/below the viewport to avoid blank flashes while
// scrolling fast.
const OVERSCAN = 6

// 视图层回调集合（App 注入；分享页只传 fetchToolOutput 的子集——
// 审批/问答/反馈不出现，传 undefined 即不渲染对应交互）
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
  children,
}: {
  controller: TranscriptController
  io: TranscriptViewIO
  empty?: EmptyState
  className?: string
  // scrollerOut：调用方持有滚动容器引用（resync 保留滚动位置用）
  scrollerOut?: { el: HTMLDivElement | null }
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
  const [measureTick, setMeasureTick] = useState(0)
  const [viewport, setViewport] = useState({ top: 0, height: 0 })

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

  // 回底请求：controller 挂的 rAF 回调递增 followSeq，此处消费
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

  // resync 保留滚动：AppController 在 applySnapshot 前写入 preserveScrollTop，
  // DOM 重建后在此恢复（内容同源，scrollTop 近似有效）
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

  // --- visible range computation ---
  const { renderStart, renderEnd, topPad, bottomPad } = useMemo(() => {
    const n = blocks.length
    if (n === 0) return { renderStart: 0, renderEnd: 0, topPad: 0, bottomPad: 0 }
    // offsets[i] = top offset of block i = sum(heights[0..i-1]) + i*GAP
    const offsets: number[] = new Array(n)
    let acc = 0
    for (let i = 0; i < n; i++) {
      offsets[i] = acc
      acc += (heightsRef.current.get(blocks[i].id) ?? ESTIMATED_H) + BLOCK_GAP_PX
    }
    // total content height = sum(heights) + (n-1)*GAP
    const lastH = heightsRef.current.get(blocks[n - 1].id) ?? ESTIMATED_H
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
  }, [blocks, viewport, measureTick])

  const visible = useMemo(
    () => blocks.slice(renderStart, renderEnd),
    [blocks, renderStart, renderEnd],
  )

  return (
    <div
      id="transcript"
      className={className ? 'transcript ' + className : 'transcript'}
      ref={scrollerRef}
      onScroll={useRafScroll<HTMLDivElement>((el) => {
        setViewport({ top: el.scrollTop, height: el.clientHeight })
        const gap = el.scrollHeight - el.scrollTop - el.clientHeight
        controller.setFollowing(gap < FOLLOW_THRESHOLD_PX)
      })}
    >
      <div id="blocks" className="transcript-inner">
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
        <Icon name="arrow-down" /> back to bottom
      </button>
    </div>
  )
}
