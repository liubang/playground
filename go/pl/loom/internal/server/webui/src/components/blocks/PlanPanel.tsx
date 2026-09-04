// PlanPanel.tsx — plan 面板（plan.updated 驱动；钉在 composer 上方）。
//
// V2 甲版形态：裸行 —— 常态是一行无卡片 chrome 的状态条：
//   ▸ 标题 · ●●●○○○○ 3/7 · 正在做：xxx
//   - 分段进度点：每 item 一个 dot，颜色即状态（绿 done / 青 in_progress(breathe) / 灰 todo）
//   - hover 浮现底色提示可点；点击展开时 header 与清单缝合成一整张卡（chrome 只在主动查看时出现）
//   - done 划线用 goal::after 尺规从左划出（text-decoration 无法动画）+ 绿勾弹入
//   - 动效统一 breathe 家族，不再用 pulse
//   - 全部完成后短暂停留展示“✓ 全部完成”，随后自动退场（不留横幅尸体）；
//     展开查看时不退场——等用户收起后再开始计时（不会在阅读时从眼前消失）
//
// plan.updated 是全量快照替换：组件内部对状态做 diff，给“本轮新完成”的行打
// just-done 并播完成过渡，否则 UI 看不出“刚刚是哪一项完成了”。

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

// 拆分出带 hooks 的子组件：PlanPanel 在 plan 为空时直接返回 null，
// hooks 只存在于有内容的分支里，保证调用顺序稳定。
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

  // 状态演进检测：找出本轮新完成的项 → 给行/点打 just-done 播完成过渡；
  // 折叠态下面板边框闪一下 primary，给“快照替换”补一点演进感。
  // 按 (goal, status) 逐项 diff：plan 整体被替换/重排时，不同 goal 的跃迁不算“刚完成”。
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
      // 折叠态下完成反馈：边框闪一下（展开态由行动画承担）
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

  // 退场：全部完成后停留 2.2s 展示“✓ 全部完成”，然后 0.3s 渐出并卸载。
  // 若 agent 中途又追加 plan 项（allDone 翻转回 false），取消退场、原地复活；
  // 若面板处于展开态（用户正在逐项查看），暂停退场，收起后才开始计时。
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
