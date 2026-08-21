// TraceView.tsx — the session's execution trace as a dense, scannable
// event list: one line per prompt / assistant message / tool call,
// grouped into turns by a gutter rail. Sits between the chat tab (full
// fidelity) and the maze tab (macro shape) — this is the debugging view.
//
// TraceView itself is presentational (blocks + maze in, interactions
// out) so the share page can reuse it; TracePage binds it to the main
// app's AppController (live maze fetch, chat jump, log export).
//
// Rows derive from the transcript block model, so the list grows with
// the run; the rhythm strip on top reuses the maze payload for step/tool
// time spans (both share the same time origin: seconds since the first
// user message).

import { memo, useCallback, useLayoutEffect, useMemo, useRef, useState } from 'react'
import type { AppController } from '../../app/controller'
import type { BlockModel, ToolCompletion } from '../../app/transcript'
import { useStore } from '../../store/store'
import type { MazeData } from '../../protocol/types'
import { useMazeData } from '../maze/useMazeData'
import { formatDur } from '../maze/axis'
import { seekToAnchor } from '../../lib/jump'
import { MarkdownView } from '../blocks/MarkdownView'
import { Icon } from '../../lib/icons'
import { toast } from '../ui/Toast'

// --- row model ---

type TraceRow =
  | { id: string; kind: 'user'; turn: number; text: string; ts: number }
  | { id: string; kind: 'assistant'; turn: number; text: string; live: boolean; ts: number }
  | { id: string; kind: 'reasoning'; turn: number; text: string }
  | {
      id: string
      kind: 'tool'
      turn: number
      callId: string
      toolName: string
      target: string
      diff?: string
      completion?: ToolCompletion
    }
  | { id: string; kind: 'notice'; turn: number; text: string; warn: boolean }
  | { id: string; kind: 'error'; turn: number; text: string }

interface TurnGroup {
  turn: number
  inputTs: number // seconds since the first prompt; -1 when unknown
  rows: TraceRow[]
}

const tsOf = (iso: string | undefined): number => {
  if (!iso) return -1
  const t = Date.parse(iso)
  return Number.isFinite(t) ? t : -1
}

function buildGroups(blocks: BlockModel[]): TurnGroup[] {
  const groups: TurnGroup[] = []
  let turn = 0
  const push = (row: TraceRow) => {
    if (groups.length === 0) groups.push({ turn: 0, inputTs: -1, rows: [] })
    groups[groups.length - 1].rows.push(row)
  }
  let firstUserMs = -1
  for (const b of blocks) {
    switch (b.kind) {
      case 'user': {
        turn++
        const ms = tsOf(b.createdAt)
        if (firstUserMs < 0 && ms >= 0) firstUserMs = ms
        groups.push({
          turn,
          inputTs: ms,
          rows: [{ id: b.id, kind: 'user', turn, text: b.text, ts: ms }],
        })
        break
      }
      case 'assistant':
        push({
          id: b.id,
          kind: 'assistant',
          turn,
          text: b.text,
          live: false,
          ts: tsOf(b.actions?.createdAt),
        })
        break
      case 'stream':
        push({ id: b.id, kind: 'assistant', turn, text: b.text, live: true, ts: -1 })
        break
      case 'reasoning':
        if (b.text.trim()) push({ id: b.id, kind: 'reasoning', turn, text: b.text })
        break
      case 'tool':
        push({
          id: b.id,
          kind: 'tool',
          turn,
          callId: b.callId || '',
          toolName: b.toolName,
          target: b.target || '',
          ...(b.diff ? { diff: b.diff } : {}),
          ...(b.completion ? { completion: b.completion } : {}),
        })
        break
      case 'notice':
        push({ id: b.id, kind: 'notice', turn, text: b.text, warn: !!b.warn })
        break
      case 'resolved':
        push({
          id: b.id,
          kind: 'notice',
          turn,
          text: `${b.what} ${b.ok ? 'allowed' : 'denied'} · ${b.actor}`,
          warn: !b.ok,
        })
        break
      case 'fatal':
      case 'interrupted':
        push({ id: b.id, kind: 'error', turn, text: b.text })
        break
      case 'approval':
        push({ id: b.id, kind: 'notice', turn, text: '等待审批…', warn: true })
        break
      case 'question':
        push({ id: b.id, kind: 'notice', turn, text: '等待回答提问…', warn: true })
        break
      case 'compact':
        push({ id: b.id, kind: 'notice', turn, text: '上下文已压缩', warn: false })
        break
      default:
        break // thinking / image / artifact: no trace row
    }
  }
  // Rebase timestamps to seconds since the first prompt (maze's origin).
  for (const g of groups) {
    g.inputTs = g.inputTs >= 0 && firstUserMs >= 0 ? (g.inputTs - firstUserMs) / 1000 : -1
    for (const r of g.rows) {
      if ((r.kind === 'user' || r.kind === 'assistant') && r.ts >= 0 && firstUserMs >= 0) {
        r.ts = (r.ts - firstUserMs) / 1000
      }
    }
  }
  return groups
}

function rowHaystack(r: TraceRow): string {
  switch (r.kind) {
    case 'tool':
      return [r.toolName, r.target, r.completion?.preview ?? '', r.completion?.full_text ?? '']
        .join('\n')
        .toLowerCase()
    default:
      return r.text.toLowerCase()
  }
}

const firstLine = (s: string) => s.split('\n', 1)[0]

function fmtMs(ms: number): string {
  return ms < 1000 ? `${Math.round(ms)}ms` : formatDur(ms / 1000)
}

// --- presentational view ---

export interface TraceViewProps {
  blocks: BlockModel[]
  maze: MazeData | null
  // scrollerOut exposes the list's scroll container for cross-view jumps.
  scrollerOut?: { el: HTMLDivElement | null }
  onLocateInChat?: (callId: string) => void
  onExportLog?: () => void
}

export function TraceView({
  blocks,
  maze,
  scrollerOut,
  onLocateInChat,
  onExportLog,
}: TraceViewProps) {
  const [query, setQuery] = useState('')
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set())
  const listRef = useRef<HTMLDivElement>(null)

  useLayoutEffect(() => {
    if (!scrollerOut) return
    scrollerOut.el = listRef.current
    return () => {
      scrollerOut.el = null
    }
  }, [scrollerOut])

  const groups = useMemo(() => buildGroups(blocks), [blocks])

  const metrics = useMemo(() => {
    let turns = 0
    let calls = 0
    let minTs = -1
    let maxTs = -1
    for (const g of groups) {
      for (const r of g.rows) {
        if (r.kind === 'user') turns++
        if (r.kind === 'tool') calls++
        if ((r.kind === 'user' || r.kind === 'assistant') && r.ts >= 0) {
          if (minTs < 0 || r.ts < minTs) minTs = r.ts
          if (r.ts > maxTs) maxTs = r.ts
        }
      }
    }
    return { turns, calls, duration: minTs >= 0 ? maxTs - minTs : -1 }
  }, [groups])

  const inputs = useMemo(() => groups.filter((g) => g.inputTs >= 0).map((g) => g.inputTs), [groups])

  const q = query.trim().toLowerCase()
  const visible = useMemo(() => {
    if (!q) return { groups, hits: -1 }
    let hits = 0
    const filtered = groups
      .map((g) => {
        const rows = g.rows.filter((r) => rowHaystack(r).includes(q))
        hits += rows.length
        return { ...g, rows }
      })
      .filter((g) => g.rows.length > 0)
    return { groups: filtered, hits }
  }, [groups, q])

  const toggle = useCallback((id: string) => {
    setExpanded((prev) => {
      const next = new Set(prev)
      if (next.has(id)) next.delete(id)
      else next.add(id)
      return next
    })
  }, [])

  // Strip click → scroll the list to the turn owning that moment.
  const seekTurn = useCallback((turn: number) => {
    const el = listRef.current
    if (!el) return
    if (turn <= 0) {
      el.scrollTo({ top: 0, behavior: 'smooth' })
      return
    }
    seekToAnchor(() => listRef.current, `[data-turn="${turn}"]`)
  }, [])

  const rowCount = groups.reduce((n, g) => n + g.rows.length, 0)

  return (
    <div className="trace-page">
      <div className="trace-toolbar">
        <span className="trace-metrics">
          <span title="首条消息到最近一次活动">
            ⏱ {metrics.duration >= 0 ? formatDur(metrics.duration) : '—'}
          </span>
          <span>⚇ {metrics.turns} 轮</span>
          <span>⌗ {metrics.calls} 次调用</span>
        </span>
        <span className="trace-toolbar-right">
          <input
            className="maze-search"
            type="search"
            placeholder="搜索轨迹…"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
          />
          {visible.hits >= 0 && <span className="maze-hits">{visible.hits} 条命中</span>}
          {onExportLog && (
            <button
              type="button"
              className="maze-btn"
              title="下载原始事件日志（NDJSON）"
              onClick={onExportLog}
            >
              <Icon name="download" /> Session log
            </button>
          )}
        </span>
      </div>
      {maze && maze.lanes.length > 0 && maze.lanes[0].stats.steps > 0 && (
        <RhythmStrip data={maze} inputs={inputs} onSeek={seekTurn} />
      )}
      {rowCount === 0 ? (
        <div className="maze-empty">暂无轨迹——发起一轮对话后这里会列出完整执行过程</div>
      ) : (
        <div className="trace-list" ref={listRef}>
          {visible.groups.map((g) => (
            <div key={g.turn} className="trace-turn" data-turn={g.turn}>
              {g.rows.map((r) => (
                <TraceRowView
                  key={r.id}
                  row={r}
                  expanded={expanded.has(r.id)}
                  onToggle={toggle}
                  onLocateInChat={onLocateInChat}
                />
              ))}
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

// --- main-app binding ---

export function TracePage({ controller }: { controller: AppController }) {
  const { sessionId, data: maze } = useMazeData(controller)
  const blocks = useStore(controller.transcript.store, (s) => s.blocks)

  const locateInChat = useCallback(
    (callId: string) => {
      controller.setMainView('chat')
      seekToAnchor(() => controller.scrollerRef.el, `[data-call-id="${CSS.escape(callId)}"]`)
    },
    [controller],
  )

  const downloadLog = useCallback(async () => {
    if (!sessionId) return
    try {
      const res = await fetch(`/v1/sessions/${sessionId}/export`, {
        headers: { Authorization: 'Bearer ' + controller.token },
      })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const blob = await res.blob()
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = `loom-session-${sessionId}.jsonl`
      a.click()
      URL.revokeObjectURL(url)
    } catch (e) {
      toast('导出 session log 失败: ' + (e as Error).message)
    }
  }, [controller, sessionId])

  if (!sessionId) return <div className="maze-empty">未选择会话</div>
  return (
    <TraceView
      blocks={blocks}
      maze={maze}
      scrollerOut={controller.traceScrollerRef}
      onLocateInChat={locateInChat}
      onExportLog={() => void downloadLog()}
    />
  )
}

// --- rhythm strip: Input / Model / Tools spans on the session timeline ---

const STRIP_ROW_H = 13
const STRIP_BAR_H = 9

const RhythmStrip = memo(function RhythmStrip({
  data,
  inputs,
  onSeek,
}: {
  data: MazeData
  inputs: number[]
  onSeek: (turn: number) => void
}) {
  const lane = data.lanes[0]
  const total = Math.max(data.tmax, 1)
  const toX = (s: number) => (s / total) * 1000
  const bar = (s: number, e: number) => {
    const x = toX(s)
    return { x, w: Math.max(toX(e) - x, 4) }
  }
  const nodes = [...lane.main, ...lane.detours]

  const onClick = (e: React.MouseEvent<SVGSVGElement>) => {
    const rect = e.currentTarget.getBoundingClientRect()
    const t = ((e.clientX - rect.left) / rect.width) * total
    let turn = 0
    for (const it of inputs) {
      if (it <= t) turn++
      else break
    }
    onSeek(turn)
  }

  return (
    <div className="trace-strip" title="点击跳转到对应轮次">
      <div className="trace-strip-labels">
        <span>Input</span>
        <span>Model</span>
        <span>Tools</span>
      </div>
      <svg
        className="trace-strip-svg"
        viewBox={`0 0 1000 ${STRIP_ROW_H * 3}`}
        preserveAspectRatio="none"
        onClick={onClick}
      >
        {inputs.map((t, i) => {
          const { x, w } = bar(t, t + total * 0.004)
          return <rect key={i} className="strip-input" x={x} y={2} width={w} height={STRIP_BAR_H} />
        })}
        {nodes.map((n) => {
          const { x, w } = bar(n.s, n.e)
          return (
            <rect
              key={n.step + (n.sub ? 's' : '')}
              className="strip-model"
              x={x}
              y={STRIP_ROW_H + 2}
              width={w}
              height={STRIP_BAR_H}
            />
          )
        })}
        {nodes.flatMap((n) =>
          n.tools.map((t, i) => {
            const { x, w } = bar(t.s, t.e ?? n.e)
            return (
              <rect
                key={t.call_id || n.step + '-' + i}
                className="strip-tool"
                x={x}
                y={STRIP_ROW_H * 2 + 2}
                width={w}
                height={STRIP_BAR_H}
              />
            )
          }),
        )}
      </svg>
    </div>
  )
})

// --- event rows ---

const BADGE: Record<TraceRow['kind'], [string, string]> = {
  user: ['t-user', 'USER'],
  assistant: ['t-assistant', 'ASSISTANT'],
  reasoning: ['t-think', 'THINK'],
  tool: ['t-tool', 'TOOL'],
  notice: ['t-sys', 'SYS'],
  error: ['t-error', 'ERROR'],
}

const TraceRowView = memo(function TraceRowView({
  row,
  expanded,
  onToggle,
  onLocateInChat,
}: {
  row: TraceRow
  expanded: boolean
  onToggle: (id: string) => void
  onLocateInChat?: (callId: string) => void
}) {
  const [badgeCls, badge] = BADGE[row.kind]
  return (
    <div className="trace-item" data-call-id={row.kind === 'tool' ? row.callId : undefined}>
      <div
        className={`trace-row${row.kind === 'error' ? ' is-error' : ''}${row.kind === 'notice' && row.warn ? ' is-warn' : ''}`}
        onClick={() => onToggle(row.id)}
      >
        <span className={`trace-dot ${badgeCls}`} />
        <span className={`trace-badge ${badgeCls}`}>{badge}</span>
        <RowText row={row} />
        {row.kind === 'tool' && (
          <span className="trace-dur">
            {row.completion
              ? row.completion.duration_ms != null
                ? fmtMs(row.completion.duration_ms)
                : ''
              : '执行中…'}
          </span>
        )}
        {row.kind === 'assistant' && row.live && <span className="trace-dur">生成中…</span>}
        <Icon name={expanded ? 'caret-down' : 'caret-right'} className="trace-caret" />
      </div>
      {expanded && <RowDetail row={row} onLocateInChat={onLocateInChat} />}
    </div>
  )
})

const RowText = memo(function RowText({ row }: { row: TraceRow }) {
  if (row.kind === 'tool') {
    const failed = row.completion?.status === 'error'
    const result = row.completion
      ? firstLine(row.completion.error_message || row.completion.preview || '')
      : ''
    return (
      <span className="trace-text mono">
        <span className="trace-toolname">{row.toolName}</span> {firstLine(row.target)}
        {result && (
          <span className={failed ? 'trace-result is-error' : 'trace-result'}> → {result}</span>
        )}
      </span>
    )
  }
  return <span className="trace-text">{firstLine(row.text) || '（空）'}</span>
})

const RowDetail = memo(function RowDetail({
  row,
  onLocateInChat,
}: {
  row: TraceRow
  onLocateInChat?: (callId: string) => void
}) {
  return (
    <div className="trace-expand">
      {row.kind === 'assistant' && <MarkdownView text={row.text} />}
      {row.kind === 'tool' && (
        <>
          {row.target && (
            <div className="trace-expand-block">
              <div className="trace-expand-head">参数</div>
              <pre className="mono">{row.target}</pre>
            </div>
          )}
          {row.diff && (
            <div className="trace-expand-block">
              <div className="trace-expand-head">变更</div>
              <pre className="mono">{row.diff}</pre>
            </div>
          )}
          {row.completion && (
            <div className="trace-expand-block">
              <div className="trace-expand-head">返回</div>
              {row.completion.error_message && (
                <div className="trace-expand-error">{row.completion.error_message}</div>
              )}
              <pre className="mono">
                {row.completion.full_text || row.completion.preview || '（无输出）'}
              </pre>
            </div>
          )}
          {onLocateInChat && (
            <button
              type="button"
              className="maze-btn maze-jump"
              onClick={() => onLocateInChat(row.callId)}
            >
              <Icon name="turn-down" /> 在对话中定位
            </button>
          )}
        </>
      )}
      {(row.kind === 'user' || row.kind === 'reasoning' || row.kind === 'error') && (
        <pre className="mono trace-expand-pre">{row.text}</pre>
      )}
      {row.kind === 'notice' && <pre className="mono trace-expand-pre">{row.text}</pre>}
    </div>
  )
})
