// statusbar.js — 状态栏：token usage / turn 数 / 版本。
// ctx 占用已迁移到 composer 旁的 CtxGauge（components/ctxgauge.js）。

import { fmtTokens } from '../format.js'

export class Statusbar {
  constructor({ usageEl, turnEl, versionEl }) {
    this.usageEl = usageEl
    this.turnEl = turnEl
    this.versionEl = versionEl
  }

  setVersion(v) {
    this.versionEl.textContent = v || ''
  }

  // snapshot.usage / usage.updated / turn.finished.usage 驱动（累计口径）
  setUsage(usage) {
    if (!usage) {
      this.usageEl.textContent = ''
      return
    }
    this.usageEl.textContent = `${fmtTokens(usage.input_tokens)} in / ${fmtTokens(usage.output_tokens)} out`
  }

  setTurns(n) {
    this.turnEl.textContent = n ? `turn ${n}` : ''
  }
}
