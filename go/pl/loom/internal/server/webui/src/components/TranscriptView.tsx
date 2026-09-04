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

// 未测高块的估计高度：按块类型+文本量分级（此前一律 ESTIMATED_H=140，大块
// 估低/小块估高，快速上翻穿过历史时滚动条跳动明显）。估宁可保守：
// ResizeObserver 实测后 offsets 会在下一帧收敛。
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
      // diff 可能是惰性的（快照重建）：用 new_string/content 体量粗估
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
  // scrollerOut：调用方持有滚动容器引用（resync 保留滚动位置用）
  scrollerOut?: { el: HTMLDivElement | null }
  // sessionLoading（snapshot fetch 进行中）：有旧内容时半透明压住，空块时
  // 摆骨架屏——避免大快照加载期间出现“点了没反应”的空白等待
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
  const [measureTick, setMeasureTick] = useState(0)
  const [viewport, setViewport] = useState({ top: 0, height: 0 })

  // --- search state (opened/closed by the TranscriptSearch bar) ---
  const search = useStore(controller.store, (s) => s.search)
  const searchNavSeq = search?.navSeq ?? 0

  // block id 全局单调递增、永不复用：heights/seenIds 不随会话切换清理就是
  // 缓慢泄漏（旧会话 id 永久占着 Map）。blocks 清空时（clear/重建前）直接
  // 清空；运行期超过阈值时渐进剔出已不存在的 id。
  useLayoutEffect(() => {
    const heights = heightsRef.current
    const seen = seenIdsRef.current
    if (blocks.length === 0) {
      heights.clear()
      seen.clear()
      return
    }
    if (seen.size <= blocks.length + 200) return
    const ids = new Set(blocks.map((b) => b.id))
    for (const id of [...heights.keys()]) if (!ids.has(id)) heights.delete(id)
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

  // 搜索定位：navSeq 变化（查询变更/翻页）→ 滚动到当前命中块并闪烁提示。
  // 只认 navSeq 的递增：流式输出每帧都重建 blocks/search 对象，若不限定
  // navSeq，用户看命中结果时会被每个新 token 拽回命中处。
  const lastNavSeqRef = useRef(0)
  useLayoutEffect(() => {
    // 搜索关闭（Esc/会话切换）后归零：下个会话的 navSeq 从 1 重新计数，
    // 不与残留值碰撞
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
    // 目标块可能从未进入渲染窗口（未测高）：用与虚拟化同一套估计值累加
    let offset: number | null = null
    let acc = 0
    for (const b of blocks) {
      if (b.id === targetId) {
        offset = acc
        break
      }
      acc += (heightsRef.current.get(b.id) ?? estimateHeight(b)) + BLOCK_GAP_PX
    }
    if (offset == null) return
    // 跳到历史命中：脱离底部跟随（搜索浏览时流式输出不该拔走视线）
    controller.store.update((s) => {
      s.following = false
    })
    el.scrollTop = Math.max(0, offset - 80)
    setViewport({ top: el.scrollTop, height: el.clientHeight })
    // 快闪烁提醒命中位置。目标块可能刚进入渲染窗口：等两帧（虚拟化提交 +
    // 浏览器渲染）后再找 DOM。
    let timer = 0
    const raf = requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        const wrap = el.querySelector(`[data-block-id="${targetId}"]`)
        if (!wrap) return
        wrap.classList.remove('search-flash')
        void (wrap as HTMLElement).offsetWidth // reflow：连续跳到同一块时重播动画
        wrap.classList.add('search-flash')
        timer = window.setTimeout(() => wrap.classList.remove('search-flash'), 1300)
      })
    })
    return () => {
      cancelAnimationFrame(raf)
      if (timer) clearTimeout(timer)
    }
  }, [searchNavSeq, search, blocks, controller])

  // 流式输出期间：安静刷新命中总数（新块可能命中当前查询；不触发滚动）。
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
      acc += (heightsRef.current.get(blocks[i].id) ?? estimateHeight(blocks[i])) + BLOCK_GAP_PX
    }
    // total content height = sum(heights) + (n-1)*GAP
    const lastH = heightsRef.current.get(blocks[n - 1].id) ?? estimateHeight(blocks[n - 1])
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

// TranscriptSearch — 会话内全文搜索条（挂在 App 的 chat-pane 顶部，
// Cmd/Ctrl+F 唤起）。输入文本是本地 state，防抖 150ms 写入 controller；
// 滚动定位与闪烁在 TranscriptView 内部完成（虚拟化几何只有它自己知道）。
export function TranscriptSearch({ controller }: { controller: TranscriptController }) {
  const search = useStore(controller.store, (s) => s.search)
  const [q, setQ] = useState('')
  const lastSentRef = useRef('')
  const inputRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    inputRef.current?.focus()
  }, [])

  // 防抖写入 controller；只看 q 的实际变化（search 对象在提交后会重建，
  // 不能作为触发源，否则会多出一次重复提交+滚动）
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
          if (e.nativeEvent.isComposing) return // Esc/Enter 先让位给输入法候选窗
          if (e.key === 'Enter') {
            e.preventDefault()
            controller.searchNav(e.shiftKey ? -1 : 1)
          } else if (e.key === 'Escape') {
            e.stopPropagation() // 不冒泡到全局 Esc（取消 turn）处理器
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
