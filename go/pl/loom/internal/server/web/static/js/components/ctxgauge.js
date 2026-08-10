// ctxgauge.js — composer 旁的 context 占用环（ring gauge）。
// 数据源：snapshot.window/occupancy（首屏）+ context.usage / context.compacted 事件。
// 渐进披露：<60% 安静 muted；≥60% warning；≥ compact trigger 比例 error + 呼吸动画。

import { fmtTokens } from '../format.js'

// 与后端 ContextConfig 默认值一致（internal/domain/budget.go），仅在
// snapshot 未带 window 字段（旧服务端）时按名义窗口推导兜底。
const DEFAULT_UTILIZATION = 0.95
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

  // snapshot.window 优先；缺省时用名义窗口 + 默认比率推导（旧服务端回退）。
  // nominalFallback ≤ 0 表示无可用窗口，组件隐藏。
  setWindow(w, nominalFallback) {
    if (w && w.effective > 0) {
      this.window = {
        nominal: w.nominal || 0,
        effective: w.effective,
        compactTrigger: w.compact_trigger || 0,
        compactTarget: w.compact_target || 0,
      }
    } else if (nominalFallback > 0) {
      const effective = Math.round(nominalFallback * DEFAULT_UTILIZATION)
      this.window = {
        nominal: nominalFallback,
        effective,
        compactTrigger: Math.round(effective * DEFAULT_TRIGGER_RATIO),
        compactTarget: 0,
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

  // context.usage：实测 input 优先（覆盖 system prompt 等 transcript 外开销）
  onContextUsage(p) {
    if (!p) return
    this.setOccupancy(Math.max(p.est_tokens || 0, p.last_call_input_tokens || 0))
  }

  // context.compacted：占用立即回落到压缩后估值，不等下一次模型调用
  onCompacted(p) {
    if (!p) return
    this.setOccupancy(p.est_tokens_after || 0)
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
