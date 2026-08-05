// statusbar.js — 状态栏：ctx 占用 / token usage / turn 数 / 版本。

import { fmtTokens } from "../format.js";

export class Statusbar {
  constructor({ ctxEl, usageEl, turnEl, versionEl }) {
    this.ctxEl = ctxEl;
    this.usageEl = usageEl;
    this.turnEl = turnEl;
    this.versionEl = versionEl;
  }

  setVersion(v) { this.versionEl.textContent = v || ""; }

  // snapshot.usage 与 snapshot.context_window 驱动
  setUsage(usage, contextWindow) {
    if (!usage) { this.usageEl.textContent = ""; this.ctxEl.textContent = ""; return; }
    this.usageEl.textContent = `${fmtTokens(usage.input_tokens)} in / ${fmtTokens(usage.output_tokens)} out`;
    if (contextWindow > 0 && usage.input_tokens != null) {
      const pct = Math.round((usage.input_tokens / contextWindow) * 100);
      this.ctxEl.textContent = `ctx ${pct}%`;
      this.ctxEl.className = pct > 90 ? "ctx-crit" : pct > 80 ? "ctx-warn" : "";
    } else {
      this.ctxEl.textContent = "";
    }
  }

  setTurns(n) {
    this.turnEl.textContent = n ? `turn ${n}` : "";
  }
}
