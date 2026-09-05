// Toast.tsx — Global toast (top-right overlay, auto-dismisses after 5s, manually closable).
// Usage: call toast('...', info?) from anywhere; <ToastHost /> lives at the App root.
// Error toasts with sticky=true never auto-dismiss, leaving enough time to read and investigate;
// the MAX_TOASTS cap prevents a burst of failures from flooding the screen.

import { Store, useStore } from '../../store/store'
import { Icon } from '../../lib/icons'

export interface ToastItem {
  id: number
  msg: string
  info: boolean
  sticky: boolean // never auto-dismisses (error messages)
}

interface ToastState {
  items: ToastItem[]
}

const toastStore = new Store<ToastState>({ items: [] })
let nextId = 1
const MAX_TOASTS = 4

function dismiss(id: number) {
  toastStore.update((s) => {
    s.items = s.items.filter((t) => t.id !== id)
  })
}

export function toast(msg: string, info = false, sticky = false) {
  const id = nextId++
  toastStore.update((s) => {
    const items = [...s.items, { id, msg, info, sticky }]
    // Over the cap, drop the oldest: keep only the most recent ones during error bursts / screen flooding
    s.items = items.length > MAX_TOASTS ? items.slice(items.length - MAX_TOASTS) : items
  })
  if (!sticky) setTimeout(() => dismiss(id), 5000)
}

function ToastRow({ t }: { t: ToastItem }) {
  return (
    <div className={'toast' + (t.info ? ' is-info' : '')} role={t.info ? 'status' : 'alert'}>
      <span className="toast-msg">{t.msg}</span>
      <button
        type="button"
        className="toast-close"
        aria-label="关闭提示"
        onClick={() => dismiss(t.id)}
      >
        <Icon name="xmark" />
      </button>
    </div>
  )
}

export function ToastHost() {
  const items = useStore(toastStore, (s) => s.items)
  return (
    <div id="toasts" aria-live="polite">
      {items.map((t) => (
        <ToastRow key={t.id} t={t} />
      ))}
    </div>
  )
}
