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

  // snapshot.usage / budget.updated 事件驱动（均为会话累计口径）。
  // cache 命中率 = cached_input_tokens / context_tokens，分子分母均为
  // provider 实测（OpenAI prompt_tokens / Anthropic input+cache_read+
  // cache_creation），有实测调用后才显示。
  setUsage(usage) {
    if (!usage) {
      this.usageEl.textContent = ''
      return
    }
    let text = `${fmtTokens(usage.input_tokens)} in / ${fmtTokens(usage.output_tokens)} out`
    if (usage.context_tokens > 0) {
      const pct = Math.min(
        100,
        Math.round((usage.cached_input_tokens / usage.context_tokens) * 100),
      )
      text += ` · cache ${pct}%`
    }
    this.usageEl.textContent = text
  }

  setTurns(n) {
    this.turnEl.textContent = n ? `turn ${n}` : ''
  }
}
