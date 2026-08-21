// axis.ts — maze time axis: idle-gap folding + tick computation.
// Intervals with no step/tool activity for over GAP_SECS (e.g. the user
// thinking between turns) collapse into a thin seam labeled ⏸; ticks inside
// activity segments still show true wall-clock seconds.

/** Gaps longer than this many seconds get folded. */
export const GAP_SECS = 60
/** Width a folded seam occupies in display-domain seconds. */
export const FOLD_TO_SECS = 3

export interface FoldGap {
  /** Display-domain left edge of the seam. */
  dStart: number
  /** Display-domain right edge of the seam. */
  dEnd: number
  /** Real seconds elided. */
  skipped: number
}

export interface FoldedAxis {
  /** Real seconds → display seconds. */
  map: (t: number) => number
  /** Display seconds → real seconds (clamps to the seam's left edge inside a gap). */
  unmap: (d: number) => number
  /** Display-domain total length. */
  total: number
  /** Folded seams (display-domain coordinates). */
  gaps: FoldGap[]
  /** Activity segments (display-domain coordinates, i.e. outside seams). */
  segments: [number, number][]
}

/**
 * Build a folded axis from activity ranges (real seconds). Ranges are
 * merged; inter-range gaps longer than GAP_SECS collapse to FOLD_TO_SECS.
 */
export function buildFoldedAxis(ranges: [number, number][], tmax: number): FoldedAxis {
  const sorted = ranges
    .filter(([s, e]) => Number.isFinite(s) && Number.isFinite(e))
    .map(([s, e]): [number, number] => [Math.max(0, Math.min(s, e)), Math.max(0, Math.max(s, e))])
    .sort((a, b) => a[0] - b[0])
  const merged: [number, number][] = []
  for (const r of sorted) {
    const last = merged[merged.length - 1]
    if (last && r[0] <= last[1]) last[1] = Math.max(last[1], r[1])
    else merged.push([r[0], r[1]])
  }

  const gaps: FoldGap[] = []
  const segments: [number, number][] = []
  let offset = 0
  let prevEnd: number | null = null
  const spans: { realStart: number; dStart: number; realEnd: number; dEnd: number }[] = []
  for (const [s, e] of merged) {
    if (prevEnd !== null && s - prevEnd > GAP_SECS) {
      const dStart = offset + prevEnd
      gaps.push({ dStart, dEnd: dStart + FOLD_TO_SECS, skipped: s - prevEnd - FOLD_TO_SECS })
      offset += FOLD_TO_SECS - (s - prevEnd)
    }
    spans.push({ realStart: s, dStart: s + offset, realEnd: e, dEnd: e + offset })
    segments.push([s + offset, e + offset])
    prevEnd = e
  }
  const total = tmax + offset

  const map = (t: number): number => {
    for (let i = 0; i < merged.length; i++) {
      const [s, e] = merged[i]
      if (t <= e) {
        // Inside (or before) this segment: linear within, clamped at its start.
        return t <= s ? spans[i].dStart : t + (spans[i].dStart - s)
      }
      if (i + 1 < merged.length && t < merged[i + 1][0]) {
        // Inside a gap: spread linearly across the seam's display width.
        const gapStart = spans[i].dEnd
        const frac = (t - e) / (merged[i + 1][0] - e)
        return gapStart + frac * FOLD_TO_SECS
      }
    }
    return t + offset
  }

  const unmap = (d: number): number => {
    for (const sp of spans) {
      if (d >= sp.dStart && d <= sp.dEnd) return sp.realStart + (d - sp.dStart)
      if (d < sp.dStart) return sp.realStart
    }
    return tmax
  }

  return { map, unmap, total, gaps, segments }
}

const TICK_STEPS = [1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 43200]

export interface AxisTick {
  /** Display-domain coordinate. */
  d: number
  /** Real seconds (label source). */
  t: number
  label: string
}

export function formatTick(t: number): string {
  if (t < 60) return `${Math.round(t)}s`
  if (t < 3600) return `${Math.floor(t / 60)}m${Math.round(t % 60) ? `${Math.round(t % 60)}s` : ''}`
  const h = Math.floor(t / 3600)
  const m = Math.round((t % 3600) / 60)
  return m ? `${h}h${m}m` : `${h}h`
}

/** Human-readable duration (nodes and stats). */
export function formatDur(t: number): string {
  if (t < 1) return `${Math.round(t * 1000)}ms`
  if (t < 60) return `${Math.round(t * 10) / 10}s`
  if (t < 3600) {
    const m = Math.floor(t / 60)
    const s = Math.round(t % 60)
    return s ? `${m}m${s}s` : `${m}m`
  }
  const h = Math.floor(t / 3600)
  const m = Math.round((t % 3600) / 60)
  return m ? `${h}h${m}m` : `${h}h`
}

/**
 * Visible ticks for a display-domain window: placed inside activity
 * segments only, labels carry real wall-clock seconds.
 */
export function axisTicks(
  axis: FoldedAxis,
  dStart: number,
  dEnd: number,
  maxTicks = 50,
): AxisTick[] {
  const span = dEnd - dStart
  if (span <= 0) return []
  // Pick the coarsest tick step (real seconds) that stays within maxTicks.
  let step = TICK_STEPS[TICK_STEPS.length - 1]
  for (const s of TICK_STEPS) {
    if (span / s <= maxTicks) {
      step = s
      break
    }
  }
  const ticks: AxisTick[] = []
  for (const [segS, segE] of axis.segments) {
    if (segE < dStart || segS > dEnd) continue
    const realStart = axis.unmap(segS)
    const first = Math.ceil(realStart / step) * step
    for (let t = first; ; t += step) {
      const d = axis.map(t)
      if (d > segE + 1e-6) break
      if (d >= dStart - 1e-6 && d <= dEnd + 1e-6) {
        ticks.push({ d, t, label: formatTick(t) })
      }
      if (ticks.length > 200) return ticks // defensive cap
    }
  }
  return ticks.sort((a, b) => a.d - b.d)
}
