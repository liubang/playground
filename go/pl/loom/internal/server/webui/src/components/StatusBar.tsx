// StatusBar.tsx — 状态栏：token usage / turn 数 / 版本。
// 与旧 components/statusbar.js 对应。

import { memo } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { fmtTokens } from '../lib/format'

export const StatusBar = memo(function StatusBar({ controller }: { controller: AppController }) {
  const usage = useStore(controller.store, (s) => s.usage)
  const turnCount = useStore(controller.store, (s) => s.turnCount)
  const version = useStore(controller.store, (s) => s.version)

  // snapshot.usage / budget.updated 事件驱动（均为会话累计口径）。
  // cache 命中率 = cached_input_tokens / context_tokens，分子分母均为
  // provider 实测，有实测调用后才显示。
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
