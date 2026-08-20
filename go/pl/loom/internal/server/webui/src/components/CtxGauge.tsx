// CtxGauge.tsx — composer 旁的 context 占用环（ring gauge）。
// 数据源：snapshot.window/occupancy（首屏）+ context.usage 事件（均经
// AppController 投影进 store）。occupancy 与后端压缩触发器同口径，前端不做
// 任何本地推算。渐进披露：<60% 安静 muted；≥60% warning；≥ compact
// trigger 比例 error + 呼吸动画。

import { memo } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { fmtTokens } from '../lib/format'

// 压缩触发比例兜底：仅当服务端未给 compact_trigger 时用于着色分级。
const DEFAULT_TRIGGER_RATIO = 0.8
// 第一档 notice level（domain 默认 notice_levels[0]）
const WARM_RATIO = 0.6

const R = 7
const CIRC = 2 * Math.PI * R

export const CtxGauge = memo(function CtxGauge({ controller }: { controller: AppController }) {
  const win = useStore(controller.store, (s) => s.window)
  const occupancy = useStore(controller.store, (s) => s.occupancy)

  if (!win || !win.effective) {
    return (
      <span id="ctx-gauge" className="ctx-gauge" hidden>
        <svg className="ctx-ring" viewBox="0 0 18 18" aria-hidden="true">
          <circle className="ctx-ring-bg" cx="9" cy="9" r="7" />
          <circle className="ctx-ring-fg" cx="9" cy="9" r="7" />
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
      <svg className="ctx-ring" viewBox="0 0 18 18" aria-hidden="true">
        <circle className="ctx-ring-bg" cx="9" cy="9" r="7" />
        <circle
          className="ctx-ring-fg"
          cx="9"
          cy="9"
          r="7"
          style={{
            strokeDasharray: CIRC.toFixed(2),
            strokeDashoffset: (CIRC * (1 - ratio)).toFixed(2),
          }}
        />
      </svg>
      <span className="ctx-gauge-pct">{pct + '%'}</span>
    </span>
  )
})
