// SessionTabs.tsx — per-session view switcher: chat (full-fidelity
// reading) / trace (scannable event list) / maze (macro shape). Rendered
// between the header and the main content when a session is selected.

import { memo } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'

const TABS = [
  { key: 'chat', label: 'Chat' },
  { key: 'trace', label: 'Trace' },
  { key: 'maze', label: 'Maze' },
] as const

export const SessionTabs = memo(function SessionTabs({
  controller,
}: {
  controller: AppController
}) {
  const mainView = useStore(controller.store, (s) => s.mainView)
  return (
    <div className="sess-tabs" role="tablist">
      {TABS.map((t) => (
        <button
          key={t.key}
          type="button"
          role="tab"
          aria-selected={mainView === t.key}
          className={'sess-tab' + (mainView === t.key ? ' is-active' : '')}
          onClick={() => controller.setMainView(t.key)}
        >
          {t.label}
        </button>
      ))}
    </div>
  )
})
