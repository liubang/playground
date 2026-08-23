// CtxGauge.tsx — context occupancy ring gauge next to the composer.
// Data sources: snapshot.window/occupancy (first paint) + context.usage events
// (both projected into the store by AppController). occupancy uses the same
// measure as the backend compaction trigger; the frontend does no local
// estimation. Progressive disclosure: muted below 60%; warning at ≥60%; error +
// breathing animation at ≥ the compact-trigger ratio.

import { memo } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { fmtTokens } from '../lib/format'

// Fallback compact-trigger ratio: used for color grading only when the server
// provides no compact_trigger.
const DEFAULT_TRIGGER_RATIO = 0.8
// First notice level (domain default notice_levels[0])
const WARM_RATIO = 0.6

const R = 15
const CIRC = 2 * Math.PI * R

export const CtxGauge = memo(function CtxGauge({ controller }: { controller: AppController }) {
  const win = useStore(controller.store, (s) => s.window)
  const occupancy = useStore(controller.store, (s) => s.occupancy)

  if (!win || !win.effective) {
    return (
      <span id="ctx-gauge" className="ctx-gauge" hidden>
        <svg className="ctx-ring" viewBox="0 0 36 36" aria-hidden="true">
          <circle className="ctx-ring-bg" cx="18" cy="18" r="15" />
          <circle className="ctx-ring-fg" cx="18" cy="18" r="15" />
        </svg>
        <span className="ctx-gauge-pct" />
      </span>
    )
  }

  const ratio = Math.min(1, Math.max(0, occupancy) / win.effective)
  const pct = Math.round(ratio * 100)
  const triggerRatio =
    win.compact_trigger && win.compact_trigger > 0
      ? win.compact_trigger / win.effective
      : DEFAULT_TRIGGER_RATIO
  const cls = ratio >= triggerRatio ? 'is-hot' : ratio >= WARM_RATIO ? 'is-warm' : ''

  const parts = [`context ${fmtTokens(occupancy)} / ${fmtTokens(win.effective)} (${pct}%)`]
  if (win.compact_trigger && win.compact_trigger > 0) {
    parts.push(`compact 触发于 ~${fmtTokens(win.compact_trigger)}`)
  }
  if (win.compact_target && win.compact_target > 0) {
    parts.push(`压缩目标 ${fmtTokens(win.compact_target)}`)
  }
  if (win.nominal && win.nominal > 0 && win.nominal !== win.effective) {
    parts.push(`名义窗口 ${fmtTokens(win.nominal)}`)
  }

  return (
    <span id="ctx-gauge" className={'ctx-gauge' + (cls ? ' ' + cls : '')} title={parts.join(' · ')}>
      <svg className="ctx-ring" viewBox="0 0 36 36" aria-hidden="true">
        <circle className="ctx-ring-bg" cx="18" cy="18" r="15" />
        <circle
          className="ctx-ring-fg"
          cx="18"
          cy="18"
          r="15"
          style={{
            strokeDasharray: CIRC.toFixed(2),
            strokeDashoffset: (CIRC * (1 - ratio)).toFixed(2),
          }}
        />
      </svg>
      {/* The number sits inside the ring and appears on demand: below the warm
          level only the ring shows (the number would be noise); it lights up once
          the level is reached — its very appearance is the signal. Full numbers
          in the title tooltip. */}
      <span className="ctx-gauge-pct">{ratio >= WARM_RATIO ? pct + '%' : ''}</span>
    </span>
  )
})
