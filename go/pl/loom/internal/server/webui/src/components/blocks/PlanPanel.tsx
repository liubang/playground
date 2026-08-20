// PlanPanel.tsx — plan 面板（plan.updated 驱动；钉在 composer 上方，
// Claude Code 风格清单）。plan 为空（无 items）时隐藏面板。

import { memo } from 'react'
import type { Plan } from '../../protocol/events'
import { Icon, type IconName } from '../../lib/icons'

const PLAN_STATUS_ICON: Record<string, IconName> = {
  todo: 'square-o',
  in_progress: 'square',
  done: 'square-check',
}

export const PlanPanel = memo(function PlanPanel({ plan }: { plan?: Plan | null }) {
  const items = plan?.items || []
  if (items.length === 0) return null
  const done = items.filter((i) => i.status === 'done').length
  return (
    <details id="plan-panel" className="plan-panel disclosure" open>
      <summary>
        <span className="plan-title">{plan?.title || 'plan'}</span>
        <span className="plan-progress">{`${done}/${items.length} done`}</span>
      </summary>
      <ul className="plan-items">
        {items.map((item, i) => (
          <li key={i} className={'plan-item is-' + (item.status || 'todo')}>
            <span className="plan-icon">
              <Icon name={PLAN_STATUS_ICON[item.status || ''] || 'square-o'} />
            </span>
            <span className="plan-goal">{item.goal || ''}</span>
          </li>
        ))}
      </ul>
    </details>
  )
})
