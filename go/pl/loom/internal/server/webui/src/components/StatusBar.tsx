// StatusBar.tsx — status bar: token usage / turn count / version.
// Corresponds to the old components/statusbar.js.

import { memo } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { fmtTokens } from '../lib/format'

export const StatusBar = memo(function StatusBar({ controller }: { controller: AppController }) {
  const usage = useStore(controller.store, (s) => s.usage)
  const turnCount = useStore(controller.store, (s) => s.turnCount)
  const version = useStore(controller.store, (s) => s.version)

  // Driven by snapshot.usage / budget.updated events (both use per-session cumulative figures).
  // cache hit rate = cached_input_tokens / context_tokens; both numerator and denominator are
  // provider-measured, and it's shown only after a measured call happens.
  let usageText = ''
  if (usage) {
    usageText = `${fmtTokens(usage.input_tokens)} in / ${fmtTokens(usage.output_tokens)} out`
    if ((usage.context_tokens || 0) > 0) {
      const pct = Math.min(
        100,
        Math.round(((usage.cached_input_tokens || 0) / (usage.context_tokens || 1)) * 100),
      )
      usageText += ` · cache ${pct}%`
    }
  }

  return (
    <footer className="statusbar">
      <span id="sb-usage" className="mono">
        {usageText}
      </span>
      <span id="sb-turn">{turnCount ? `turn ${turnCount}` : ''}</span>
      <span className="spacer" />
      <span id="sb-version">{version}</span>
    </footer>
  )
})
