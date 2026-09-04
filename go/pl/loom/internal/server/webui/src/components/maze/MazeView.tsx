// MazeView.tsx — execution-trace maze renderer: paints MazeData as a
// timeline of main-path capsules plus detour arcs. Single-lane (trace tab,
// share page) and two-lane (compare view) share this renderer; compare adds
// turn-alignment lines and the per-turn detour audit.
//
// Visual language (isomorphic to dsh-trace-compare, styled via app.css
// tokens):
//   - solid main path: steps that advanced the task (duration capsules
//     filled by verdict color) and answer nodes;
//   - dashed detour arcs: error (red ✗) / dead-end (grey ·) / blind-retry
//     (amber ↻) steps, hanging off the nearest main step;
//   - sub-agent branches: aggregated nodes (purple) whose sub-bars are the
//     child's judged tool calls;
//   - idle folding: >60s of no activity collapses into a ⏸ seam while
//     ticks keep real wall-clock seconds;
//   - drag selects a range to zoom into (Grafana-style brush), Shift-drag
//     or a trackpad horizontal swipe pans, wheel zooms around the cursor,
//     double-click/button resets. Touch: one-finger horizontal drag
//     brushes (vertical drags stay native scrolls via touch-action),
//     two-finger pinch zooms and pans.

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { MazeData, MazeLane, MazeNode, MazeTool, MazeVerdict } from '../../protocol/types'
import { axisTicks, buildFoldedAxis, formatDur, type FoldedAxis } from './axis'
import { Icon } from '../../lib/icons'
import { copyText, fmtDuration } from '../../lib/format'

// --- layout constants ---
const LANE_HEADER_H = 34 // lane header (model / title / stats)
const MAIN_H = 30 // main-path row height
const PAR_BAR_H = 8 // parallel tool sub-bar row height
const DETOUR_H = 30 // detour row height
const AXIS_H = 26 // bottom tick strip
const GAP_BAND_H = 16 // folded-seam caption row (reserved only when gaps exist)
const PAD_X = 12
const MIN_BAR_W = 5

// Touch devices have no hover; hover cards stick on tap there, so node
// hover handlers check this first (tap still opens the detail panel).
const CAN_HOVER = typeof window !== 'undefined' && window.matchMedia('(hover: hover)').matches

const VERDICT_META: Record<MazeVerdict, { cls: string; label: string }> = {
  ok: { cls: 'v-ok', label: '成功' },
  answer: { cls: 'v-answer', label: '回答' },
  error: { cls: 'v-error', label: '失败' },
  deadend: { cls: 'v-deadend', label: '扑空' },
  retry: { cls: 'v-retry', label: '无效重试' },
  pending: { cls: 'v-pending', label: '进行中' },
}

interface HoverCard {
  x: number
  y: number
  title: string
  lines: string[]
}

interface SelectedNode {
  laneKey: string
  node: MazeNode
}

/** Parallel tool sub-bar rows a step occupies (waterfall convention: each
 * call sits at its own real span). */
function parRows(n: MazeNode): number {
  return n.tools.length > 1 ? n.tools.length : 0
}

/** Pack detours into rows greedily so detour nodes never overlap on the
 * axis. */
function packDetourRows(detours: MazeNode[]): number[] {
  const rowEnds: number[] = []
  return detours.map((d) => {
    for (let r = 0; r < rowEnds.length; r++) {
      if (rowEnds[r] <= d.s) {
        rowEnds[r] = d.e
        return r
      }
    }
    rowEnds.push(d.e)
    return rowEnds.length - 1
  })
}

function nodeMatches(n: MazeNode, q: string, failOnly: boolean): boolean {
  if (failOnly && n.v !== 'error' && n.v !== 'retry' && n.v !== 'deadend') return false
  if (!q) return true
  const hay = [
    n.label,
    n.rz_txt,
    n.why,
    ...n.tools.flatMap((t) => [t.name, t.args, t.args_full ?? '', t.res, t.res_full ?? '']),
  ]
    .join('\n')
    .toLowerCase()
  return hay.includes(q)
}

/** Estimated rendered width of a 10px label: CJK glyphs ~10px, ASCII ~6px. */
function estTextWidth(s: string): number {
  return [...s].reduce((w, ch) => w + (ch.charCodeAt(0) > 0xff ? 10 : 6), 0)
}

/** Ellipsize a label to fit a pixel budget (estimated). */
function fitLabel(s: string, budget: number): string {
  if (estTextWidth(s) <= budget) return s
  let w = 0
  let i = 0
  for (const ch of s) {
    w += ch.charCodeAt(0) > 0xff ? 10 : 6
    if (w > budget - 8) break
    i++
  }
  return s.slice(0, i) + '…'
}

// useRafState coalesces high-frequency setter calls (per-event pointer/wheel
// updates during pan/zoom/brush) into one state commit per animation frame —
// the whole SVG subtree re-renders at most once per frame during gestures,
// instead of once per raw input event (trackpads can fire >100/s).
function useRafState<S>(initial: S): [S, (v: S) => void] {
  const [state, setState] = useState(initial)
  const pendingRef = useRef<{ has: boolean; value: S }>({ has: false, value: initial })
  const rafRef = useRef<number | null>(null)
  useEffect(
    () => () => {
      if (rafRef.current !== null) cancelAnimationFrame(rafRef.current)
    },
    [],
  )
  const set = useCallback((v: S) => {
    pendingRef.current = { has: true, value: v }
    if (rafRef.current !== null) return // 已有挂起帧：合并为最新值
    rafRef.current = requestAnimationFrame(() => {
      rafRef.current = null
      const p = pendingRef.current
      if (!p.has) return
      p.has = false
      setState(p.value)
    })
  }, [])
  return [state, set]
}

function tokLabel(n: MazeNode): string {
  const parts: string[] = []
  if (n.rz_ms != null && n.rz_ms > 0) parts.push(`推理 ${fmtDuration(n.rz_ms)}`)
  if (n.rz_tok != null) parts.push(`${n.rz_tok} tok`)
  else if (n.rz > 0) parts.push(`${n.rz} 段推理`)
  if (n.out_tok != null) parts.push(`输出 ${n.out_tok} tok`)
  return parts.join(' · ')
}

function nodeTitle(n: MazeNode): string {
  if (n.sub) return `⤴ ${n.label || '子代理'}`
  return `S${n.step} · 第${n.turn}轮`
}

/** Defensive normalization: tolerate null arrays from JSON (Go nil slices)
 * so every iteration site can trust the shape. */
function normalizeMaze(d: MazeData): MazeData {
  const normNode = (n: MazeNode): MazeNode => (n.tools ? n : { ...n, tools: [] })
  return {
    ...d,
    lanes: (d.lanes ?? []).map((l) => ({
      ...l,
      main: (l.main ?? []).map(normNode),
      detours: (l.detours ?? []).map(normNode),
    })),
  }
}

export const MazeView = memo(function MazeView({
  data: rawData,
  compare = false,
  onLocateStep,
}: {
  data: MazeData
  compare?: boolean
  onLocateStep?: (node: MazeNode) => void
}) {
  const data = useMemo(() => normalizeMaze(rawData), [rawData])
  const wrapRef = useRef<HTMLDivElement>(null)
  // win/brush are updated by per-event pointer/wheel handlers: rAF-coalesced
  // so a gesture costs one SVG re-render per frame, not one per input event.
  const [win, setWin] = useRafState<[number, number] | null>(null) // display-domain window; null = whole map
  const [hover, setHover] = useState<HoverCard | null>(null)
  const [selected, setSelected] = useState<SelectedNode | null>(null)
  const [failOnly, setFailOnly] = useState(false)
  const [query, setQuery] = useState('')
  const [showAudit, setShowAudit] = useState(false)
  const dragRef = useRef<
    | { mode: 'pan'; x: number; win: [number, number] }
    | { mode: 'brush'; x0: number; active: boolean; win: [number, number] }
    | null
  >(null)
  // Brush selection in canvas-relative pixels; non-null while a range drag
  // is past the click threshold.
  const [brush, setBrush] = useRafState<{ x0: number; x1: number } | null>(null)
  // A completed brush suppresses the trailing click so it doesn't select
  // whatever node happens to sit under the mouse-up point.
  const suppressClickRef = useRef(false)
  const [panning, setPanning] = useState(false)

  // Esc closes the detail panel first. Capture phase + stopPropagation so
  // the keypress never reaches outer overlays (the compare view closes
  // itself on Esc only when no panel is open).
  useEffect(() => {
    if (!selected) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      e.stopPropagation()
      setSelected(null)
    }
    window.addEventListener('keydown', onKey, true)
    return () => window.removeEventListener('keydown', onKey, true)
  }, [selected])
  // Canvas pixel width, tracked with a ResizeObserver: x coordinates are
  // absolute pixels (no viewBox), so a container resize must re-render.
  const [canvasW, setCanvasW] = useState(800)
  // Cached canvas rect: nav/brush math only consumes x-coordinates (left /
  // width), and ResizeObserver catches the cases that change them (sidebar /
  // panel toggles, window resize) — pointerdown refreshes as a backstop. This
  // keeps getBoundingClientRect (a forced-layout read) off the per-event
  // wheel/pointermove hot path.
  const rectRef = useRef<DOMRect | null>(null)
  const getRect = useCallback(() => {
    const el = wrapRef.current
    if (!el) return null
    const r = rectRef.current ?? el.getBoundingClientRect()
    rectRef.current = r
    return r
  }, [])
  useEffect(() => {
    const el = wrapRef.current
    if (!el) return
    setCanvasW(el.clientWidth)
    rectRef.current = el.getBoundingClientRect()
    const ro = new ResizeObserver(() => {
      setCanvasW(el.clientWidth)
      rectRef.current = el.getBoundingClientRect()
    })
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  // Folded axis: every lane's activity ranges merge into one construction
  // (lanes share the same timeline).
  const axis: FoldedAxis = useMemo(() => {
    const ranges: [number, number][] = []
    for (const lane of data.lanes) {
      for (const n of [...lane.main, ...lane.detours]) {
        ranges.push([n.s, n.e])
        for (const t of n.tools) ranges.push([t.s, t.e ?? n.e])
      }
    }
    return buildFoldedAxis(ranges, data.tmax)
  }, [data])

  const q = query.trim().toLowerCase()
  const filtering = failOnly || q !== ''

  // Layout: each lane's height budget (header + main row + parallel
  // sub-bar area + detour rows).
  const layout = useMemo(() => {
    const lanes = data.lanes.map((lane) => {
      const maxPar = lane.main.reduce((m, n) => Math.max(m, parRows(n)), 0)
      const rows = packDetourRows(lane.detours)
      const detourRows = rows.length ? Math.max(...rows) + 1 : 0
      const parH = maxPar * PAR_BAR_H
      const h = LANE_HEADER_H + MAIN_H + parH + detourRows * DETOUR_H + 8
      return { lane, rows, parH, h, detourRows }
    })
    // Dedicated caption row for folded-seam labels, between the lanes and
    // the tick strip — the only band guaranteed free of capsules and sub
    // labels. Reserved only when gaps exist.
    const bandH = axis.gaps.length > 0 ? GAP_BAND_H : 0
    const totalH = lanes.reduce((s, l) => s + l.h, 0) + bandH + AXIS_H
    return { lanes, totalH }
  }, [data, axis.gaps.length])

  const total = Math.max(axis.total, 1)
  const [dStart, dEnd] = win ?? [0, total]

  const toX = useCallback(
    (d: number, w: number) =>
      PAD_X + ((d - dStart) / Math.max(dEnd - dStart, 1e-6)) * (w - PAD_X * 2),
    [dStart, dEnd],
  )

  // React registers wheel handlers as passive, so an onWheel prop cannot
  // preventDefault — the canvas would scroll vertically while zooming.
  // Register a native non-passive listener instead: vertical wheel zooms
  // around the cursor, dominant horizontal deltas (trackpad swipe) pan;
  // vertical page scroll stays available via the scrollbar (tall lanes).
  const navRef = useRef<(clientX: number, deltaX: number, deltaY: number) => void>(() => {})
  navRef.current = (clientX, deltaX, deltaY) => {
    const rect = getRect()
    if (!rect) return
    const span = dEnd - dStart
    if (Math.abs(deltaX) > Math.abs(deltaY)) {
      // pan: positive deltaX (swipe left) advances the window forward
      const dd = (deltaX / Math.max(rect.width - PAD_X * 2, 1)) * span
      let ns = dStart + dd
      let ne = dEnd + dd
      if (ns < 0) {
        ne -= ns
        ns = 0
      }
      if (ne > total) {
        ns -= ne - total
        ne = total
      }
      if (ns <= 0 && ne >= total) setWin(null)
      else setWin([ns, ne])
      return
    }
    const frac = Math.min(Math.max((clientX - rect.left - PAD_X) / (rect.width - PAD_X * 2), 0), 1)
    const center = dStart + frac * span
    const scale = deltaY > 0 ? 1.25 : 0.8
    let ns = center - (center - dStart) * scale
    let ne = center + (dEnd - center) * scale
    if (ne - ns < 0.5) return // finest zoom: 0.5 display seconds
    if (ne - ns >= total) {
      setWin(null)
      return
    }
    ns = Math.max(0, ns)
    ne = Math.min(total, ne)
    setWin([ns, ne])
  }
  useEffect(() => {
    const el = wrapRef.current
    if (!el) return
    const handler = (e: WheelEvent) => {
      e.preventDefault()
      navRef.current(e.clientX, e.deltaX, e.deltaY)
    }
    el.addEventListener('wheel', handler, { passive: false })
    return () => el.removeEventListener('wheel', handler)
  }, [])

  // Pointer-based navigation: one handler set covers mouse and touch.
  // Pointer capture on the canvas keeps the gesture ours even when the
  // pointer leaves the element mid-drag.
  const pointersRef = useRef(new Map<number, number>()) // pointerId → clientX
  const pinchRef = useRef<{ d: number; mid: number } | null>(null)

  // Capture must NOT happen on pointerdown: while captured, the trailing
  // click retargets to the canvas and node onClick handlers never fire.
  // Defer capture until the gesture proves to be a brush/pan/pinch.
  const capturePointer = (e: React.PointerEvent) => {
    try {
      e.currentTarget.setPointerCapture(e.pointerId)
    } catch {
      // Synthetic events in tests: capture is a best-effort robustness
      // aid, never worth breaking the gesture.
    }
  }

  const onPointerDown = useCallback(
    (e: React.PointerEvent) => {
      if (e.pointerType === 'mouse' && e.button !== 0) return
      suppressClickRef.current = false
      pointersRef.current.set(e.pointerId, e.clientX)
      if (pointersRef.current.size === 2) {
        // Second finger down: abandon any brush/pan, switch to pinch.
        // Unambiguous gesture intent — capture both fingers now.
        dragRef.current = null
        setBrush(null)
        setPanning(false)
        capturePointer(e)
        const firstId = [...pointersRef.current.keys()][0]
        if (firstId !== undefined && firstId !== e.pointerId) {
          try {
            e.currentTarget.setPointerCapture(firstId)
          } catch {
            /* best-effort, see capturePointer */
          }
        }
        const [a, b] = [...pointersRef.current.values()]
        pinchRef.current = { d: Math.abs(b - a), mid: (a + b) / 2 }
        return
      }
      if (e.pointerType === 'mouse' && e.shiftKey) {
        // Shift-drag pans: unambiguous drag intent, capture immediately.
        capturePointer(e)
        dragRef.current = { mode: 'pan', x: e.clientX, win: [dStart, dEnd] }
        setPanning(true)
        return
      }
      const rect = e.currentTarget.getBoundingClientRect()
      rectRef.current = rect // gesture 起点：刷新缓存，随后走 getRect
      dragRef.current = {
        mode: 'brush',
        x0: e.clientX - rect.left,
        active: false,
        win: [dStart, dEnd],
      }
    },
    [dStart, dEnd],
  )
  const onPointerMove = useCallback(
    (e: React.PointerEvent) => {
      if (!pointersRef.current.has(e.pointerId)) return
      pointersRef.current.set(e.pointerId, e.clientX)
      // Pinch: iterative zoom (distance ratio) + pan (midpoint travel)
      // around the midpoint's domain point.
      if (pinchRef.current && pointersRef.current.size >= 2) {
        const rect = getRect()
        if (!rect) return
        const [a, b] = [...pointersRef.current.values()]
        const d = Math.abs(b - a)
        if (d < 1) return
        const mid = (a + b) / 2
        const prev = pinchRef.current
        pinchRef.current = { d, mid }
        const w = Math.max(rect.width - PAD_X * 2, 1)
        const span = dEnd - dStart
        let ns = dStart - ((mid - prev.mid) / w) * span
        let ne = dEnd - ((mid - prev.mid) / w) * span
        const scale = prev.d / d
        const frac = Math.min(Math.max((mid - rect.left - PAD_X) / w, 0), 1)
        const center = ns + frac * (ne - ns)
        ns = center - (center - ns) * scale
        ne = center + (ne - center) * scale
        if (ne - ns < 0.5) return
        if (ne - ns >= total) {
          setWin(null)
          return
        }
        setWin([Math.max(0, ns), Math.min(total, ne)])
        return
      }
      const drag = dragRef.current
      if (!drag) return
      if (drag.mode === 'pan') {
        const dd =
          ((e.clientX - drag.x) / Math.max(canvasW - PAD_X * 2, 1)) * (drag.win[1] - drag.win[0])
        let ns = drag.win[0] - dd
        let ne = drag.win[1] - dd
        if (ns < 0) {
          ne -= ns
          ns = 0
        }
        if (ne > total) {
          ns -= ne - total
          ne = total
        }
        if (ns <= 0 && ne >= total) setWin(null)
        else setWin([Math.max(0, ns), Math.min(total, ne)])
        return
      }
      // brush: a sub-threshold drag is still a click (node select)
      const rect = getRect()
      if (!rect) return
      const x1 = e.clientX - rect.left
      if (!drag.active && Math.abs(x1 - drag.x0) < 4) return
      if (!drag.active) {
        drag.active = true
        capturePointer(e) // drag proven: keep the gesture ours off-canvas
        setHover(null) // don't leave a tooltip stuck under the selection
      }
      setBrush({ x0: drag.x0, x1 })
    },
    [dStart, dEnd, total, canvasW],
  )
  const endDrag = useCallback((e: React.PointerEvent) => {
    pointersRef.current.delete(e.pointerId)
    if (pinchRef.current) {
      // A lifted finger ends the pinch; the remaining finger stays inert
      // until it lifts too (its drag was cancelled when the pinch began).
      if (pointersRef.current.size < 2) pinchRef.current = null
      return
    }
    const drag = dragRef.current
    dragRef.current = null
    setPanning(false)
    if (!drag || drag.mode !== 'brush') return
    setBrush(null)
    if (!drag.active) return // plain click: let node click handlers run
    suppressClickRef.current = true
    const rect = getRect()
    if (!rect) return
    const w = Math.max(rect.width - PAD_X * 2, 1)
    const span = drag.win[1] - drag.win[0]
    const toD = (px: number) => drag.win[0] + Math.min(Math.max((px - PAD_X) / w, 0), 1) * span
    const lo = toD(Math.min(drag.x0, e.clientX - rect.left))
    const hi = toD(Math.max(drag.x0, e.clientX - rect.left))
    if (hi - lo < 0.5) return // accidental micro-drag: keep the window
    setWin([lo, hi])
  }, [])

  // Time span covered by the in-progress brush, shown on the selection.
  const brushLabel = useMemo(() => {
    if (!brush) return ''
    const rect = getRect()
    if (!rect) return ''
    const w = Math.max(rect.width - PAD_X * 2, 1)
    const span = dEnd - dStart
    const toD = (px: number) => dStart + Math.min(Math.max((px - PAD_X) / w, 0), 1) * span
    const lo = toD(Math.min(brush.x0, brush.x1))
    const hi = toD(Math.max(brush.x0, brush.x1))
    return formatDur(hi - lo)
  }, [brush, dStart, dEnd, getRect])

  const ticks = useMemo(() => axisTicks(axis, dStart, dEnd), [axis, dStart, dEnd])

  // Turn-alignment lines (two lanes): turns present on both sides link
  // their closing main nodes.
  const alignments = useMemo(() => {
    if (!compare || data.lanes.length !== 2) return []
    const endByTurn = (lane: MazeLane) => {
      const m = new Map<number, MazeNode>()
      for (const n of lane.main) m.set(n.turn, n)
      return m
    }
    const a = endByTurn(data.lanes[0])
    const b = endByTurn(data.lanes[1])
    const detourCount = (lane: MazeLane, turn: number) =>
      lane.detours.filter((d) => d.turn === turn).length
    const out: { turn: number; e1: number; e2: number; d1: number; d2: number }[] = []
    for (const [turn, n1] of a) {
      const n2 = b.get(turn)
      if (!n2) continue
      out.push({
        turn,
        e1: n1.e,
        e2: n2.e,
        d1: detourCount(data.lanes[0], turn),
        d2: detourCount(data.lanes[1], turn),
      })
    }
    return out.sort((x, y) => x.turn - y.turn)
  }, [compare, data.lanes])

  // Hit count (shown while filtering).
  const hitCount = useMemo(() => {
    if (!filtering) return -1
    let n = 0
    for (const lane of data.lanes) {
      for (const node of [...lane.main, ...lane.detours]) if (nodeMatches(node, q, failOnly)) n++
    }
    return n
  }, [data, q, failOnly, filtering])

  const svgW = canvasW

  // Folded-seam labels: one per visible gap, x-clamped into the canvas and
  // thinned against each other so dense idle periods don't paint a row of
  // overlapping text. The seam rect always carries a <title> tooltip as
  // the fallback for skipped labels.
  const gapMarks = useMemo(() => {
    const GAP_LABEL_MIN_SPACING = 12
    let lastRight = -Infinity
    return axis.gaps.map((g) => {
      const x1 = toX(g.dStart, svgW)
      const x2 = toX(g.dEnd, svgW)
      const label = `⏸ 省略 ${formatDur(g.skipped)}`
      if (x2 < 0 || x1 > svgW) return { x1, x2, label, cx: 0, show: false }
      const w = estTextWidth(label)
      const lo = PAD_X + w / 2
      const hi = svgW - PAD_X - w / 2
      const cx = lo <= hi ? Math.min(Math.max((x1 + x2) / 2, lo), hi) : svgW / 2
      const show = cx - w / 2 >= lastRight + GAP_LABEL_MIN_SPACING
      if (show) lastRight = cx + w / 2
      return { x1, x2, label, cx, show }
    })
  }, [axis, toX, svgW])

  let laneY = 0

  return (
    <div className="maze-wrap">
      <div className="maze-toolbar">
        <span className="maze-legend">
          <i className="lg v-ok" /> 主干
          <i className="lg v-answer" /> 回答
          <i className="lg v-error" /> 失败
          <i className="lg v-deadend" /> 扑空
          <i className="lg v-retry" /> 重试
          <i className="lg v-sub" /> 子代理
        </span>
        <span className="maze-toolbar-right">
          <label className={'maze-filter' + (failOnly ? ' is-on' : '')}>
            <input
              type="checkbox"
              checked={failOnly}
              onChange={(e) => setFailOnly(e.target.checked)}
            />
            只看失败/重试
          </label>
          <input
            className="maze-search"
            type="search"
            placeholder="搜索命令/返回内容…"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
          />
          {hitCount >= 0 && <span className="maze-hits">{hitCount} 步命中</span>}
          {compare && (
            <button
              type="button"
              className={'maze-btn' + (showAudit ? ' is-on' : '')}
              title="按轮次盘点两边支路差额"
              onClick={() => setShowAudit((v) => !v)}
            >
              支路盘点
            </button>
          )}
          <button type="button" className="maze-btn" title="复位缩放" onClick={() => setWin(null)}>
            ⤢ 整图
          </button>
        </span>
      </div>

      <div className="maze-body">
        <div
          ref={wrapRef}
          className={'maze-canvas' + (panning ? ' is-panning' : '')}
          onPointerDown={onPointerDown}
          onPointerMove={onPointerMove}
          onPointerUp={endDrag}
          onPointerCancel={endDrag}
          onClickCapture={(e) => {
            if (!suppressClickRef.current) return
            suppressClickRef.current = false
            e.stopPropagation()
            e.preventDefault()
          }}
          onMouseLeave={() => setHover(null)}
          onDoubleClick={(e) => {
            if (e.target === e.currentTarget || (e.target as Element).tagName === 'svg')
              setWin(null)
          }}
        >
          <svg width="100%" height={layout.totalH} className="maze-svg">
            {/* folded seams: the label lives in the dedicated caption row
                above the tick strip (reserved via GAP_BAND_H), so it can
                never collide with lane headers, capsules or sub labels */}
            {gapMarks.map((g, i) =>
              g.x2 < 0 || g.x1 > svgW ? null : (
                <g key={'gap' + i} className="maze-gap">
                  <rect
                    x={g.x1}
                    y={0}
                    width={Math.max(g.x2 - g.x1, 2)}
                    height={layout.totalH - AXIS_H}
                  >
                    <title>{g.label}</title>
                  </rect>
                  {g.show && (
                    <text x={g.cx} y={layout.totalH - AXIS_H - GAP_BAND_H + 12} textAnchor="middle">
                      {g.label}
                    </text>
                  )}
                </g>
              ),
            )}
            {/* ticks */}
            {ticks.map((tk, i) => (
              <g key={'tk' + i} className="maze-tick">
                <line
                  x1={toX(tk.d, svgW)}
                  y1={layout.totalH - AXIS_H}
                  x2={toX(tk.d, svgW)}
                  y2={layout.totalH - AXIS_H + 6}
                />
                <text x={toX(tk.d, svgW)} y={layout.totalH - 8} textAnchor="middle">
                  {tk.label}
                </text>
              </g>
            ))}
            {/* lanes */}
            {layout.lanes.map(({ lane, rows, parH, h }, li) => {
              const top = laneY
              laneY += h
              const mainY = top + LANE_HEADER_H + MAIN_H / 2
              const detourTop = top + LANE_HEADER_H + MAIN_H + parH + 4
              const st = lane.stats
              return (
                <g key={lane.key}>
                  {/* lane identity: the dot color ties the lane back to its
                      session picker in the compare header */}
                  {compare && (
                    <circle
                      cx={PAD_X + 4}
                      cy={top + 12}
                      r={4}
                      className={`maze-lane-dot lane-${li + 1}`}
                    />
                  )}
                  <text x={compare ? PAD_X + 14 : PAD_X} y={top + 16} className="maze-lane-title">
                    {lane.model || 'session'}
                    {lane.title ? ` · ${lane.title}` : ''}
                  </text>
                  <text x={svgW - PAD_X} y={top + 16} textAnchor="end" className="maze-lane-stats">
                    {st.steps} 步 · {st.tools} 工具 · {st.detours} 支路 · {formatDur(st.t)}
                    {st.out_tok > 0 ? ` · ${st.in_tok + st.out_tok} tok` : ''}
                    {st.rz_ms ? ` · 推理 ${fmtDuration(st.rz_ms)}` : ''}
                  </text>
                  {/* main line */}
                  <line
                    x1={PAD_X}
                    y1={mainY}
                    x2={svgW - PAD_X}
                    y2={mainY}
                    className={`maze-mainline${compare ? ` lane-${li + 1}` : ''}`}
                  />
                  {/* main nodes */}
                  {lane.main.map((n) => (
                    <MainNode
                      key={n.step}
                      n={n}
                      laneKey={lane.key}
                      y={mainY}
                      parH={parH}
                      axis={axis}
                      toX={toX}
                      svgW={svgW}
                      dim={filtering && !nodeMatches(n, q, failOnly)}
                      selected={selected?.laneKey === lane.key && selected.node.step === n.step}
                      onHover={setHover}
                      onSelect={setSelected}
                    />
                  ))}
                  {/* detours */}
                  {lane.detours.map((d, di) => (
                    <DetourNode
                      key={d.step}
                      n={d}
                      laneKey={lane.key}
                      row={rows[di]}
                      mainY={mainY}
                      detourTop={detourTop}
                      attachNode={lane.main.find((m) => m.step === d.attach)}
                      axis={axis}
                      toX={toX}
                      svgW={svgW}
                      dim={filtering && !nodeMatches(d, q, failOnly)}
                      selected={selected?.laneKey === lane.key && selected.node.step === d.step}
                      onHover={setHover}
                      onSelect={setSelected}
                    />
                  ))}
                  {/* turn-alignment lines (two lanes) */}
                  {compare &&
                    li === 0 &&
                    alignments.map((al) => {
                      // laneY already advanced to the second lane's top above.
                      const y1 = mainY
                      const y2 = laneY + LANE_HEADER_H + MAIN_H / 2
                      const x1 = toX(axis.map(al.e1), svgW)
                      const x2 = toX(axis.map(al.e2), svgW)
                      const dt = Math.abs(al.e2 - al.e1)
                      return (
                        <g key={'al' + al.turn} className="maze-align">
                          <line x1={x1} y1={y1} x2={x2} y2={y2} />
                          <text x={(x1 + x2) / 2} y={(y1 + y2) / 2 - 4} textAnchor="middle">
                            第{al.turn}轮 · Δ{formatDur(dt)} · 支路 {al.d1}↔{al.d2}
                          </text>
                        </g>
                      )
                    })}
                </g>
              )
            })}
          </svg>
          {brush && (
            <div
              className="maze-brush"
              style={{
                left: Math.min(brush.x0, brush.x1),
                width: Math.abs(brush.x1 - brush.x0),
                top: wrapRef.current?.scrollTop ?? 0,
                height: wrapRef.current?.clientHeight ?? 0,
              }}
            >
              <span className="maze-brush-label">{brushLabel}</span>
            </div>
          )}
          {hover && (
            <div className="maze-tip" style={{ left: hover.x, top: hover.y }}>
              <div className="maze-tip-title">{hover.title}</div>
              {hover.lines.map((l, i) => (
                <div key={i} className="maze-tip-line">
                  {l}
                </div>
              ))}
            </div>
          )}
        </div>

        {selected && (
          <DetailPanel
            node={selected.node}
            onClose={() => setSelected(null)}
            onLocateStep={onLocateStep}
          />
        )}
      </div>

      {compare && showAudit && (
        <DetourAudit lanes={data.lanes} onZoom={(s, e) => setWin([axis.map(s), axis.map(e)])} />
      )}
    </div>
  )
})

// --- main nodes ---

function barProps(
  n: MazeNode,
  axis: FoldedAxis,
  toX: (d: number, w: number) => number,
  svgW: number,
): { x: number; w: number } {
  const x1 = toX(axis.map(n.s), svgW)
  const x2 = toX(axis.map(n.e), svgW)
  return { x: x1, w: Math.max(x2 - x1, MIN_BAR_W) }
}

const MainNode = memo(function MainNode({
  n,
  laneKey,
  y,
  parH,
  axis,
  toX,
  svgW,
  dim,
  selected,
  onHover,
  onSelect,
}: {
  n: MazeNode
  laneKey: string
  y: number
  parH: number
  axis: FoldedAxis
  toX: (d: number, w: number) => number
  svgW: number
  dim: boolean
  selected: boolean
  onHover: (h: HoverCard | null) => void
  onSelect: (s: SelectedNode | null) => void
}) {
  const meta = VERDICT_META[n.v] ?? VERDICT_META.ok
  const { x, w } = barProps(n, axis, toX, svgW)
  if (x > svgW || x + w < 0) return null
  const isAnswer = n.v === 'answer'
  // Fit the longest label first, fall back to the step id, then nothing —
  // estimated at ~6px/char (ASCII) plus 8px capsule padding.
  const full = `S${n.step}·${n.turn} ${formatDur(n.e - n.s)}`
  const short = `S${n.step}`
  const label = w >= full.length * 6 + 8 ? full : w >= short.length * 6 + 8 ? short : ''
  return (
    <g
      className={`maze-node main ${meta.cls}${n.live ? ' is-live' : ''}${dim ? ' is-dim' : ''}${selected ? ' is-selected' : ''}`}
      onMouseEnter={(e) => {
        if (!CAN_HOVER) return // touch: tap opens the detail panel instead
        // Tooltip is absolutely positioned in the scrolling canvas: account
        // for scrollTop or it lands above the cursor after scrolling.
        const canvas = e.currentTarget.closest('.maze-canvas') as HTMLElement | null
        if (!canvas) return
        const rect = canvas.getBoundingClientRect()
        onHover({
          x: Math.min(e.clientX - rect.left + 10, rect.width - 240),
          y: e.clientY - rect.top + 12 + canvas.scrollTop,
          title: `${nodeTitle(n)} · ${meta.label}`,
          lines: [
            `耗时 ${formatDur(n.e - n.s)}${n.retries ? ` · 重试等待 ×${n.retries}` : ''}`,
            tokLabel(n),
            n.tools.length > 0 ? `${n.tools.length} 次工具调用` : '无工具调用（回答）',
            n.why || '',
          ].filter(Boolean),
        })
      }}
      onMouseLeave={() => onHover(null)}
      onClick={() => onSelect({ laneKey, node: n })}
    >
      {isAnswer ? (
        <circle cx={x + w / 2} cy={y} r={6} className="maze-answer-dot" />
      ) : (
        <rect x={x} y={y - 9} width={w} height={18} rx={9} />
      )}
      {label && (
        <text x={x + w / 2} y={y + 3.5} textAnchor="middle" className="maze-node-label">
          {label}
        </text>
      )}
      {/* parallel tool sub-bars: with ≥2 calls, each sits at its real
          span below the capsule */}
      {n.tools.length > 1 &&
        n.tools.map((t, i) => {
          const tx1 = toX(axis.map(t.s), svgW)
          const tx2 = toX(axis.map(t.e ?? n.e), svgW)
          const tm = VERDICT_META[t.v] ?? VERDICT_META.ok
          return (
            <rect
              key={t.call_id || i}
              className={`maze-parbar ${tm.cls}`}
              x={tx1}
              y={y + 12 + i * PAR_BAR_H}
              width={Math.max(tx2 - tx1, 3)}
              height={5}
              rx={2.5}
            />
          )
        })}
      {/* base tick marking that this step carries parallel sub-bars */}
      {parH > 0 && <rect className="maze-parbase" x={x} y={y + 10} width={w} height={2} />}
    </g>
  )
})

// --- detour nodes (dashed arc + return path) ---

const DetourNode = memo(function DetourNode({
  n,
  laneKey,
  row,
  mainY,
  detourTop,
  attachNode,
  axis,
  toX,
  svgW,
  dim,
  selected,
  onHover,
  onSelect,
}: {
  n: MazeNode
  laneKey: string
  row: number
  mainY: number
  detourTop: number
  attachNode?: MazeNode
  axis: FoldedAxis
  toX: (d: number, w: number) => number
  svgW: number
  dim: boolean
  selected: boolean
  onHover: (h: HoverCard | null) => void
  onSelect: (s: SelectedNode | null) => void
}) {
  const meta = VERDICT_META[n.v] ?? VERDICT_META.ok
  const { x, w } = barProps(n, axis, toX, svgW)
  if (x > svgW || x + w < 0) return null
  const y = detourTop + row * DETOUR_H + DETOUR_H / 2
  // Dashed arc: from the attach step's bottom edge curving down to the
  // detour's left edge; a return path bends back once the detour ends.
  const ax = attachNode ? toX(axis.map(attachNode.e), svgW) : PAD_X
  const ay = mainY + 10
  const path = `M ${ax} ${ay} C ${ax} ${y - 14}, ${x - 8} ${y - 14}, ${x} ${y}`
  const returnPath = `M ${x + w} ${y} C ${x + w + 6} ${y + 12}, ${ax + 6} ${y + 12}, ${ax} ${ay + 2}`
  const glyph = n.v === 'error' ? '✗' : n.v === 'deadend' ? '·' : n.v === 'retry' ? '↻' : ''
  const labelText = n.sub ? `⤴ ${n.label ?? ''}` : `S${n.step} ${glyph}`
  const estTextW = estTextWidth(labelText) + 8 // +8 capsule padding
  return (
    <g
      className={`maze-node detour ${meta.cls}${n.sub ? ' is-sub' : ''}${n.live ? ' is-live' : ''}${dim ? ' is-dim' : ''}${selected ? ' is-selected' : ''}`}
      onMouseEnter={(e) => {
        if (!CAN_HOVER) return // touch: tap opens the detail panel instead
        const canvas = e.currentTarget.closest('.maze-canvas') as HTMLElement | null
        if (!canvas) return
        const rect = canvas.getBoundingClientRect()
        onHover({
          x: Math.min(e.clientX - rect.left + 10, rect.width - 240),
          y: e.clientY - rect.top + 12 + canvas.scrollTop,
          title: `${nodeTitle(n)} · ${meta.label}`,
          lines: [
            n.sub
              ? `子代理支路 · ${n.tools.length} 次工具调用 · ${formatDur(n.e - n.s)}`
              : `耗时 ${formatDur(n.e - n.s)}`,
            tokLabel(n),
            n.why || '',
          ].filter(Boolean),
        })
      }}
      onMouseLeave={() => onHover(null)}
      onClick={() => onSelect({ laneKey, node: n })}
    >
      <path d={path} className="maze-arc" />
      {!n.live && n.v !== 'ok' && <path d={returnPath} className="maze-arc return" />}
      <rect x={x} y={y - 8} width={w} height={16} rx={8} />
      {n.sub ? (
        // Sub-agent titles are far wider than any capsule: annotate beside
        // the bar — right side, flipping left near the canvas edge, and
        // ellipsizing when neither side fits.
        <SubLabel x={x} w={w} y={y} text={labelText} estW={estTextW} svgW={svgW} />
      ) : (
        w >= estTextW && (
          <text x={x + w / 2} y={y + 3.5} textAnchor="middle" className="maze-node-label">
            {labelText}
          </text>
        )
      )}
      {/* sub-agent node sub-bars: all of the child's judged tool calls */}
      {n.sub &&
        n.tools.slice(0, 24).map((t, i) => {
          const tx1 = toX(axis.map(t.s), svgW)
          const tx2 = toX(axis.map(t.e ?? n.e), svgW)
          const tm = VERDICT_META[t.v] ?? VERDICT_META.ok
          return (
            <rect
              key={t.call_id || i}
              className={`maze-parbar ${tm.cls}`}
              x={tx1}
              y={y + 10 + (i % 3) * 6}
              width={Math.max(tx2 - tx1, 3)}
              height={4}
              rx={2}
            />
          )
        })}
    </g>
  )
})

// SubLabel renders a sub-agent title beside its bar: right side preferred,
// flipping left near the canvas edge, ellipsized when neither side fits.
const SubLabel = memo(function SubLabel({
  x,
  w,
  y,
  text,
  estW,
  svgW,
}: {
  x: number
  w: number
  y: number
  text: string
  estW: number
  svgW: number
}) {
  const spaceRight = svgW - PAD_X - (x + w + 6)
  const spaceLeft = x - 6 - PAD_X
  if (estW <= spaceRight) {
    return (
      <text x={x + w + 6} y={y + 3.5} className="maze-sub-label">
        {text}
      </text>
    )
  }
  if (estW <= spaceLeft) {
    return (
      <text x={x - 6} y={y + 3.5} textAnchor="end" className="maze-sub-label">
        {text}
      </text>
    )
  }
  // Neither side fits whole: take the wider side and ellipsize into it.
  if (spaceRight >= spaceLeft) {
    return (
      <text x={x + w + 6} y={y + 3.5} className="maze-sub-label">
        {fitLabel(text, Math.max(spaceRight, 0))}
      </text>
    )
  }
  return (
    <text x={x - 6} y={y + 3.5} textAnchor="end" className="maze-sub-label">
      {fitLabel(text, Math.max(spaceLeft, 0))}
    </text>
  )
})

// --- detail panel ---

const DetailPanel = memo(function DetailPanel({
  node,
  onClose,
  onLocateStep,
}: {
  node: MazeNode
  onClose: () => void
  onLocateStep?: (node: MazeNode) => void
}) {
  const meta = VERDICT_META[node.v] ?? VERDICT_META.ok
  return (
    <aside className="maze-detail">
      <div className="maze-detail-head">
        <span className={`maze-badge ${meta.cls}`}>{meta.label}</span>
        <span className="maze-detail-title">{nodeTitle(node)}</span>
        <span className="spacer" />
        <button type="button" className="icon-btn" title="关闭" onClick={onClose}>
          <Icon name="xmark" />
        </button>
      </div>
      <div className="maze-detail-meta">
        <span>耗时 {formatDur(node.e - node.s)}</span>
        {node.retries ? <span>模型重试 ×{node.retries}</span> : null}
        {tokLabel(node) && <span>{tokLabel(node)}</span>}
        {node.in_tok != null && <span>输入 {node.in_tok} tok</span>}
      </div>
      {node.why && <div className="maze-detail-why">{node.why}</div>}
      {onLocateStep &&
        !node.sub &&
        ((node.msg_seq != null && node.msg_seq > 0) || node.tools.length > 0) && (
          <button type="button" className="maze-btn maze-jump" onClick={() => onLocateStep(node)}>
            <Icon name="turn-down" /> 在轨迹中定位此步骤
          </button>
        )}
      {node.rz_txt && (
        <details className="maze-detail-rz">
          <summary>
            思考摘要（{node.rz} 段{node.rz_ms ? ` · ${fmtDuration(node.rz_ms)}` : ''}）
          </summary>
          <p>{node.rz_txt}</p>
        </details>
      )}
      <div className="maze-detail-tools">
        {node.tools.length === 0 && <div className="maze-detail-empty">无工具调用</div>}
        {node.tools.map((t, i) => (
          <ToolCard key={t.call_id || i} t={t} />
        ))}
      </div>
    </aside>
  )
})

const ToolCard = memo(function ToolCard({ t }: { t: MazeTool }) {
  const meta = VERDICT_META[t.v] ?? VERDICT_META.ok
  return (
    <div className={`maze-tool ${meta.cls}`}>
      <div className="maze-tool-head">
        <span className="maze-tool-name mono">{t.name}</span>
        <span className="maze-tool-dur">{t.e === null ? '执行中…' : formatDur(t.dur)}</span>
        <span className={`maze-badge sm ${meta.cls}`}>{meta.label}</span>
      </div>
      {t.args_full && (
        <div className="maze-tool-block">
          <div className="maze-tool-block-head">
            <span>参数</span>
            <button
              type="button"
              className="icon-btn sm"
              title="复制参数"
              onClick={() => void copyText(t.args_full!)}
            >
              <Icon name="copy" />
            </button>
          </div>
          <pre className="mono">{t.args_full}</pre>
        </div>
      )}
      {(t.res_full || t.res) && (
        <div className="maze-tool-block">
          <div className="maze-tool-block-head">
            <span>返回</span>
            <button
              type="button"
              className="icon-btn sm"
              title="复制返回内容"
              onClick={() => void copyText(t.res_full || t.res)}
            >
              <Icon name="copy" />
            </button>
          </div>
          <pre className="mono">{t.res_full || t.res}</pre>
        </div>
      )}
      {t.why && <div className="maze-detail-why">{t.why}</div>}
      {t.child_id && <div className="maze-tool-child mono">⤴ 子会话 {t.child_id}</div>}
    </div>
  )
})

// --- per-turn detour audit (two-lane compare) ---

const DetourAudit = memo(function DetourAudit({
  lanes,
  onZoom,
}: {
  lanes: MazeLane[]
  onZoom: (s: number, e: number) => void
}) {
  if (lanes.length !== 2) return null
  const byTurn = (lane: MazeLane) => {
    const m = new Map<
      number,
      { n: number; t: number; by: Record<string, number>; s: number; e: number }
    >()
    for (const d of lane.detours) {
      const g = m.get(d.turn) ?? { n: 0, t: 0, by: {}, s: d.s, e: d.e }
      g.n++
      g.t += Math.max(0, d.e - d.s)
      g.by[d.v] = (g.by[d.v] ?? 0) + 1
      g.s = Math.min(g.s, d.s)
      g.e = Math.max(g.e, d.e)
      m.set(d.turn, g)
    }
    return m
  }
  const a = byTurn(lanes[0])
  const b = byTurn(lanes[1])
  const turns = [...new Set([...a.keys(), ...b.keys()])].sort((x, y) => x - y)
  const fmt = (g?: { n: number; t: number; by: Record<string, number> }) => {
    if (!g) return '—'
    const parts = [
      g.by.error ? `✗${g.by.error}` : '',
      g.by.retry ? `↻${g.by.retry}` : '',
      g.by.deadend ? `·${g.by.deadend}` : '',
    ].filter(Boolean)
    return `${g.n} 支路 ${formatDur(g.t)}${parts.length ? `（${parts.join(' ')}）` : ''}`
  }
  return (
    <div className="maze-audit">
      <div className="maze-audit-head">按轮次支路盘点（点一行缩放到该轮）</div>
      <table>
        <thead>
          <tr>
            <th>轮次</th>
            <th>
              <i className="lane-dot lane-1" /> {lanes[0].model || '会话 1'}
            </th>
            <th>
              <i className="lane-dot lane-2" /> {lanes[1].model || '会话 2'}
            </th>
            <th>差额</th>
          </tr>
        </thead>
        <tbody>
          {turns.map((turn) => {
            const ga = a.get(turn)
            const gb = b.get(turn)
            const dt = (gb?.t ?? 0) - (ga?.t ?? 0)
            const verdict =
              ga && gb
                ? dt > 0.5
                  ? `第 2 会话多耗 ${formatDur(dt)}`
                  : dt < -0.5
                    ? `第 1 会话多耗 ${formatDur(-dt)}`
                    : '持平'
                : '—（缺席本身即信号）'
            const zs = Math.min(ga?.s ?? Infinity, gb?.s ?? Infinity)
            const ze = Math.max(ga?.e ?? 0, gb?.e ?? 0)
            return (
              <tr
                key={turn}
                onClick={() => Number.isFinite(zs) && onZoom(Math.max(0, zs - 5), ze + 5)}
              >
                <td>第 {turn} 轮</td>
                <td>{fmt(ga)}</td>
                <td>{fmt(gb)}</td>
                <td>{verdict}</td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
})
