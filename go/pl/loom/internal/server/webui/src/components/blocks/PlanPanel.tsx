// PlanPanel.tsx — plan panel (driven by plan.updated; pinned above the composer).
//
// V2 variant A form: bare row — normally a single status line without card chrome:
//   ▸ title · ●●●○○○○ 3/7 · working on: xxx
//   - segmented progress dots: one dot per item; color is the status (green done / teal in_progress(breathe) / gray todo)
//   - a background tint appears on hover to hint it is clickable; on click-to-expand the header and list merge into one card (chrome only appears on active inspection)
//   - done strikethrough is drawn from the left by the goal::after ruler (text-decoration cannot animate) + green check pops in
//   - animations are unified under the breathe family; pulse is no longer used
//   - after everything completes, briefly shows "✓ all done", then exits automatically (no banner corpse left behind);
//     no exit while expanded for viewing — the timer only starts after the user collapses it (won't vanish while being read)
//
// plan.updated is a full-snapshot replacement: the component diffs statuses internally, marking rows "newly done this round" with
// just-done and playing the completion transition; otherwise the UI cannot tell "which item just finished".

import { memo, useEffect, useRef, useState } from 'react'
import type { Plan, PlanItem } from '../../protocol/events'
import { Icon } from '../../lib/icons'

type PlanStatus = 'todo' | 'in_progress' | 'done'

function normStatus(status?: string): PlanStatus {
  return status === 'in_progress' || status === 'done' ? status : 'todo'
}

export const PlanPanel = memo(function PlanPanel({ plan }: { plan?: Plan | null }) {
  const items = plan?.items || []
  if (items.length === 0) return null
  return <PlanBody title={plan?.title || 'plan'} items={items} />
})

// Split out a child component with hooks: PlanPanel returns null directly when plan is empty,
// so hooks only exist in the branch with content, keeping the call order stable.
const PlanBody = memo(function PlanBody({ title, items }: { title: string; items: PlanItem[] }) {
  const panelRef = useRef<HTMLDetailsElement>(null)
  const listRef = useRef<HTMLUListElement>(null)
  const dotsRef = useRef<HTMLSpanElement>(null)
  const prevRef = useRef<{ goal: string; st: PlanStatus }[]>([])
  const animTimer = useRef<number>(0)
  const flashTimer = useRef<number>(0)
  const [retiring, setRetiring] = useState(false)
  const [gone, setGone] = useState(false)
  const [open, setOpen] = useState(false)

  const statuses = items.map((it) => normStatus(it.status))
  const done = statuses.filter((s) => s === 'done').length
  const curIdx = statuses.indexOf('in_progress')
  const allDone = done === items.length
  const sig = statuses.join('|')

  // Status progression detection: find items newly done this round → mark the row/dot just-done and play the completion transition;
  // in the collapsed state the panel border flashes primary once, adding a sense of progression to the "snapshot replacement".
  // Diff item by item on (goal, status): when the whole plan is replaced/reordered, transitions of a different goal don't count as "just done".
  useEffect(() => {
    const cur = items.map((it) => ({ goal: it.goal || '', st: normStatus(it.status) }))
    const prev = prevRef.current
    const newlyDone: number[] = []
    if (prev.length > 0) {
      const n = Math.min(prev.length, cur.length)
      for (let i = 0; i < n; i++) {
        if (prev[i].st !== 'done' && cur[i].st === 'done' && prev[i].goal === cur[i].goal) {
          newlyDone.push(i)
        }
      }
    }
    if (newlyDone.length > 0) {
      clearTimeout(animTimer.current)
      clearTimeout(flashTimer.current)
      for (const i of newlyDone) {
        listRef.current?.querySelector(`li[data-i="${i}"]`)?.classList.add('just-done')
        dotsRef.current?.querySelector(`.pp-dot[data-i="${i}"]`)?.classList.add('just')
      }
      const clearClasses = () => {
        for (const i of newlyDone) {
          listRef.current?.querySelector(`li[data-i="${i}"]`)?.classList.remove('just-done')
          dotsRef.current?.querySelector(`.pp-dot[data-i="${i}"]`)?.classList.remove('just')
        }
      }
      animTimer.current = window.setTimeout(clearClasses, 900)
      // Completion feedback in collapsed state: flash the border once (in expanded state the row animation carries it)
      if (panelRef.current && !panelRef.current.open) {
        const p = panelRef.current
        p.classList.remove('flash')
        void p.offsetWidth
        p.classList.add('flash')
        flashTimer.current = window.setTimeout(() => p.classList.remove('flash'), 500)
      }
    }
    prevRef.current = cur
    return () => {
      clearTimeout(animTimer.current)
      clearTimeout(flashTimer.current)
    }
  }, [items])

  // Exit: after all items complete, linger 2.2s showing "✓ all done", then fade out over 0.3s and unmount.
  // If the agent appends plan items midway (allDone flips back to false), cancel the exit and revive in place;
  // if the panel is expanded (user is reviewing item by item), pause the exit and only start the timer after collapsing.
  useEffect(() => {
    if (!allDone) {
      setRetiring(false)
      setGone(false)
      return
    }
    if (open) return
    const linger = window.setTimeout(() => setRetiring(true), 2200)
    const vanish = window.setTimeout(() => setGone(true), 2200 + 360)
    return () => {
      clearTimeout(linger)
      clearTimeout(vanish)
    }
  }, [allDone, open])

  if (gone) return null

  return (
    <details
      ref={panelRef}
      id="plan-panel"
      className={'plan-panel' + (retiring ? ' is-retiring' : '')}
      onToggle={(e) => setOpen(e.currentTarget.open)}
    >
      <summary>
        <span className="pp-title" title={title}>
          {title}
        </span>
        <span className="pp-dots" ref={dotsRef}>
          {items.map((_, i) => (
            <span
              key={i}
              data-i={i}
              className={
                'pp-dot' +
                (statuses[i] === 'done'
                  ? ' is-done'
                  : statuses[i] === 'in_progress'
                    ? ' is-cur'
                    : '')
              }
            />
          ))}
        </span>
        <span className={'pp-count' + (allDone ? ' is-done-all' : '')}>
          {done}/{items.length}
        </span>
        <span className={'pp-now' + (curIdx >= 0 ? ' is-live' : allDone ? ' is-done-all' : '')}>
          {curIdx >= 0 ? (
            <>
              正在做：<b key={sig}>{items[curIdx].goal || ''}</b>
            </>
          ) : (
            <b key={'s' + (allDone ? '1' : '0')}>{allDone ? '全部完成 ✓' : '等待开始…'}</b>
          )}
        </span>
      </summary>
      <div className="pp-body">
        <ul className="pp-list" ref={listRef}>
          {items.map((it, i) => {
            const st = statuses[i]
            return (
              <li key={i} data-i={i} className={'pp-item is-' + st}>
                <span className="pp-num">
                  {st === 'done' ? (
                    <span className="pp-check">
                      <Icon name="check" />
                    </span>
                  ) : (
                    <span className="pp-num-txt">{i + 1}</span>
                  )}
                </span>
                <span className="pp-goal" title={it.goal || ''}>
                  {it.goal || ''}
                </span>
              </li>
            )
          })}
        </ul>
      </div>
    </details>
  )
})
