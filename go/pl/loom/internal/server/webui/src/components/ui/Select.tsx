// Select.tsx — 通用自绘下拉（替代原生 <select>），受控组件。
// 原生 select 的选项浮层由操作系统渲染，与暗色主题割裂且不可定制；
// 这里复用 picker 浮层的视觉语言（暗色底、check 槽位、锚定定位）。
// 交互逻辑与旧 static/js/components/select.js 一一对应。

import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { Icon } from '../../lib/icons'

export interface SelectOption {
  value: string
  label: string
}

export interface SelectProps {
  className?: string
  options: SelectOption[]
  value: string
  onChange: (value: string) => void
  disabled?: boolean
}

// 全局同时只开一个浮层：模块级注册 Esc/失焦/滚动收起（早于设置面板的
// Esc 监听注册，stopImmediatePropagation 可挡住同节点后续监听器）。
let closeCurrentPop: (() => void) | null = null

document.addEventListener(
  'keydown',
  (e) => {
    if (e.key === 'Escape' && closeCurrentPop) {
      e.stopImmediatePropagation()
      closeCurrentPop()
    }
  },
  true,
)
window.addEventListener('blur', () => closeCurrentPop?.())
window.addEventListener('resize', () => closeCurrentPop?.())
// 外部滚动会让 fixed 定位漂移，关闭浮层；浮层自身的滚动除外
document.addEventListener(
  'scroll',
  (e) => {
    if (!closeCurrentPop) return
    const pop = document.querySelector('.sel-pop')
    if (pop && !pop.contains(e.target as Node)) closeCurrentPop()
  },
  true,
)

export function Select({ className = '', options, value, onChange, disabled }: SelectProps) {
  const [open, setOpen] = useState(false)
  const btnRef = useRef<HTMLButtonElement>(null)
  const popRef = useRef<HTMLDivElement>(null)
  const [popStyle, setPopStyle] = useState<React.CSSProperties>({})

  const close = () => {
    setOpen(false)
    if (closeCurrentPop) closeCurrentPop = null
  }

  // 外部点击关闭（捕获阶段，触发按钮自身除外——按钮 onClick 会 toggle）
  useEffect(() => {
    if (!open) return
    closeCurrentPop = close
    const onDocClick = (e: MouseEvent) => {
      const t = e.target as Node
      if (popRef.current?.contains(t) || btnRef.current?.contains(t)) return
      close()
    }
    document.addEventListener('click', onDocClick, true)
    return () => {
      document.removeEventListener('click', onDocClick, true)
      if (closeCurrentPop === close) closeCurrentPop = null
    }
  }, [open])

  // 打开时锚定：按钮下方，空间不足翻转到上方；选中项滚入可视区
  useEffect(() => {
    if (!open || !btnRef.current || !popRef.current) return
    const btn = btnRef.current
    const pop = popRef.current
    const r = btn.getBoundingClientRect()
    const style: React.CSSProperties = { minWidth: r.width + 'px' }
    style.left = Math.max(8, Math.min(r.left, innerWidth - pop.offsetWidth - 8)) + 'px'
    if (r.bottom + pop.offsetHeight + 8 > innerHeight && r.top - pop.offsetHeight - 6 > 0) {
      style.top = Math.max(8, r.top - pop.offsetHeight - 6) + 'px'
    } else {
      style.top = r.bottom + 6 + 'px'
    }
    setPopStyle(style)
    pop.querySelector('.sel-item.is-active')?.scrollIntoView({ block: 'nearest' })
  }, [open])

  const hit = options.find((o) => o.value === value)

  return (
    <>
      <button
        ref={btnRef}
        type="button"
        className={(className ? className + ' ' : '') + 'sel'}
        disabled={disabled}
        onClick={() => (open ? close() : setOpen(true))}
      >
        <span className="sel-label">{hit ? hit.label : value}</span>
        <Icon name="caret-down" className="sel-caret" />
      </button>
      {open &&
        // 浮层挂到 body（与旧版一致）：fixed 定位不受祖先 transform/overflow 影响
        createPortal(
          <div ref={popRef} className="sel-pop" style={popStyle}>
            {options.map((o) => (
              <button
                key={o.value}
                type="button"
                className={'sel-item' + (o.value === value ? ' is-active' : '')}
                onClick={() => {
                  close()
                  if (o.value !== value) onChange(o.value)
                }}
              >
                <span className="sel-item-label">{o.label}</span>
                <span className="check">{o.value === value && <Icon name="check" />}</span>
              </button>
            ))}
          </div>,
          document.body,
        )}
    </>
  )
}
