// Toast.tsx — 全局 toast（右上角浮层，5s 自动消失，可手动关闭/复制）。
// 用法：任意处调用 toast('...', info?)；<ToastHost /> 挂在 App 根部。
// sticky=true 的错误 toast 不自动消失：错误信息需要足够的时间读完、
// 复制、排查；上限 MAX_TOASTS 防止连续失败糊满屏幕。

import { useState } from 'react'
import { Store, useStore } from '../../store/store'
import { Icon } from '../../lib/icons'
import { copyText } from '../../lib/format'

export interface ToastItem {
  id: number
  msg: string
  info: boolean
  sticky: boolean // 不自动消失（错误信息）
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
    // 超上限丢最老的：连续报错/刷屏时只保留最近几条
    s.items = items.length > MAX_TOASTS ? items.slice(items.length - MAX_TOASTS) : items
  })
  if (!sticky) setTimeout(() => dismiss(id), 5000)
}

function ToastRow({ t }: { t: ToastItem }) {
  const [copied, setCopied] = useState(false)
  return (
    <div className={'toast' + (t.info ? ' is-info' : '')} role={t.info ? 'status' : 'alert'}>
      <span className="toast-msg">{t.msg}</span>
      <button
        type="button"
        className="toast-close"
        title={copied ? '已复制' : '复制内容'}
        onClick={() => {
          void copyText(t.msg).then((ok) => {
            if (!ok) return
            setCopied(true)
            setTimeout(() => setCopied(false), 1200)
          })
        }}
      >
        <Icon name={copied ? 'check' : 'copy'} />
      </button>
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
