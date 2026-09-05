// Select.tsx — Generic custom dropdown (replaces the native <select>), a controlled component.
// The native select's option overlay is rendered by the OS, clashing with the dark theme and not customizable;
// this reuses the picker overlay's visual language (dark background, check slot, anchored positioning).
// Interaction logic mirrors the legacy static/js/components/select.js.

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

// Only one overlay is open globally at a time: module-level Esc/blur/scroll dismissal (registered before the
// settings panel's Esc listener, so stopImmediatePropagation can block later listeners on the same node).
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
// External scroll would make the fixed-positioned overlay drift, so dismiss it; scrolling inside the overlay itself is exempt
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
    // If focus was inside the overlay (keyboard/Esc/option click), return it to the trigger button so focus does not drop to body
    const hadFocus = !!popRef.current?.contains(document.activeElement)
    setOpen(false)
    if (closeCurrentPop) closeCurrentPop = null
    if (hadFocus) btnRef.current?.focus()
  }

  // Outside click dismisses (capture phase; the trigger button itself is exempt — its onClick toggles)
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

  // Anchor on open: below the button, flipping above when space is tight; the selected item scrolls into view and takes focus,
  // with ↑/↓ navigating between options (roving focus, same interaction language as PickerMenu).
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
    const active = pop.querySelector<HTMLElement>('.sel-item.is-active')
    ;(active ?? pop.querySelector<HTMLElement>('.sel-item'))?.focus()
    active?.scrollIntoView({ block: 'nearest' })
  }, [open])

  // ↑/↓/Home/End navigate in a loop; Enter/Space fire through the focused option's native click;
  // Esc is closed by the module-level capture listener.
  const onPopKeyDown = (e: React.KeyboardEvent) => {
    const items = [...(popRef.current?.querySelectorAll<HTMLElement>('.sel-item') ?? [])]
    if (items.length === 0) return
    const idx = items.indexOf(document.activeElement as HTMLElement)
    const move = (i: number) => {
      e.preventDefault()
      items[i].focus()
      items[i].scrollIntoView({ block: 'nearest' })
    }
    if (e.key === 'ArrowDown') move(idx < 0 ? 0 : (idx + 1) % items.length)
    else if (e.key === 'ArrowUp')
      move(idx < 0 ? items.length - 1 : (idx - 1 + items.length) % items.length)
    else if (e.key === 'Home') move(0)
    else if (e.key === 'End') move(items.length - 1)
  }

  const hit = options.find((o) => o.value === value)

  return (
    <>
      <button
        ref={btnRef}
        type="button"
        className={(className ? className + ' ' : '') + 'sel'}
        disabled={disabled}
        aria-haspopup="listbox"
        aria-expanded={open}
        onClick={() => (open ? close() : setOpen(true))}
        onKeyDown={(e) => {
          // While closed, ↓/↑ expand it directly (native select muscle memory)
          if (!open && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
            e.preventDefault()
            setOpen(true)
          }
        }}
      >
        <span className="sel-label">{hit ? hit.label : value}</span>
        <Icon name="caret-down" className="sel-caret" />
      </button>
      {open &&
        // The overlay is mounted to body (as in the legacy version) so fixed positioning is unaffected by ancestor transform/overflow
        createPortal(
          <div
            ref={popRef}
            className="sel-pop"
            style={popStyle}
            role="listbox"
            onKeyDown={onPopKeyDown}
          >
            {options.map((o) => (
              <button
                key={o.value}
                type="button"
                role="option"
                aria-selected={o.value === value}
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
