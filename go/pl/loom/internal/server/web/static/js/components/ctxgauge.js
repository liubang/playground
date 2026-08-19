// ctxgauge.js — composer 旁的 context 占用环（ring gauge）。
// 数据源：snapshot.window/occupancy（首屏）+ context.usage 事件。
// occupancy 与后端压缩触发器同口径（provider 实测 + 增量估算），
// 前端不做任何本地推算。渐进披露：<60% 安静 muted；≥60% warning；
// ≥ compact trigger 比例 error + 呼吸动画。

import { fmtTokens } from '../format.js'

// 压缩触发比例兜底：仅当服务端未给 compact_trigger 时用于着色分级。
const DEFAULT_TRIGGER_RATIO = 0.8
// 第一档 notice level（domain 默认 notice_levels[0]）
const WARM_RATIO = 0.6

const R = 7
const CIRC = 2 * Math.PI * R

export class CtxGauge {
  constructor(root) {
    this.root = root
    this.ringFg = root.querySelector('.ctx-ring-fg')
    this.ringFg.style.strokeDasharray = CIRC.toFixed(2)
    this.ringFg.style.strokeDashoffset = CIRC.toFixed(2)
    this.pctEl = root.querySelector('.ctx-gauge-pct')
    this.window = null // {nominal, effective, compactTrigger, compactTarget}
    this.occupancy = 0
    this.root.hidden = true
  }

  reset() {
    this.window = null
    this.occupancy = 0
    this._render()
  }

  // window 来自 snapshot.window 或 SetModel 响应（服务端推导的阈值投影）；
  // 缺省表示模型未声明可用窗口，组件隐藏。
  setWindow(w) {
    if (w && w.effective > 0) {
      this.window = {
        nominal: w.nominal || 0,
        effective: w.effective,
        compactTrigger: w.compact_trigger || 0,
        compactTarget: w.compact_target || 0,
      }
    } else {
      this.window = null
    }
    this._render()
  }

  setOccupancy(tokens) {
    if (tokens == null || isNaN(tokens)) return
    this.occupancy = Math.max(0, tokens)
    this._render()
  }

  // context.usage：occupancy_tokens 与压缩触发器同口径（实测 + 增量估算）。
  onContextUsage(p) {
    if (!p) return
    this.setOccupancy(p.occupancy_tokens)
  }

  _render() {
    if (!this.window || this.window.effective <= 0) {
      this.root.hidden = true
      return
    }
    this.root.hidden = false
    const ratio = Math.min(1, this.occupancy / this.window.effective)
    const pct = Math.round(ratio * 100)
    this.ringFg.style.strokeDashoffset = (CIRC * (1 - ratio)).toFixed(2)
    this.pctEl.textContent = pct + '%'

    const triggerRatio =
      this.window.compactTrigger > 0
        ? this.window.compactTrigger / this.window.effective
        : DEFAULT_TRIGGER_RATIO
    const cls = ratio >= triggerRatio ? 'is-hot' : ratio >= WARM_RATIO ? 'is-warm' : ''
    this.root.className = 'ctx-gauge' + (cls ? ' ' + cls : '')

    const parts = [
      `context ${fmtTokens(this.occupancy)} / ${fmtTokens(this.window.effective)} (${pct}%)`,
    ]
    if (this.window.compactTrigger > 0) {
      parts.push(`compact 触发于 ~${fmtTokens(this.window.compactTrigger)}`)
    }
    if (this.window.compactTarget > 0) {
      parts.push(`压缩目标 ${fmtTokens(this.window.compactTarget)}`)
    }
    if (this.window.nominal > 0 && this.window.nominal !== this.window.effective) {
      parts.push(`名义窗口 ${fmtTokens(this.window.nominal)}`)
    }
    this.root.title = parts.join(' · ')
  }
}
