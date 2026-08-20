// Toast.tsx — 全局 toast（右下角浮层，5s 自动消失）。
// 用法：任意处调用 toast('...', info?)；<ToastHost /> 挂在 App 根部。

import { Store, useStore } from '../../store/store'

export interface ToastItem {
  id: number
  msg: string
  info: boolean
}

interface ToastState {
  items: ToastItem[]
}

const toastStore = new Store<ToastState>({ items: [] })
let nextId = 1

export function toast(msg: string, info = false) {
  const id = nextId++
  toastStore.update((s) => {
    s.items = [...s.items, { id, msg, info }]
  })
  setTimeout(() => {
    toastStore.update((s) => {
      s.items = s.items.filter((t) => t.id !== id)
    })
  }, 5000)
}

export function ToastHost() {
  const items = useStore(toastStore, (s) => s.items)
  return (
    <div id="toasts">
      {items.map((t) => (
        <div key={t.id} className={'toast' + (t.info ? ' is-info' : '')}>
          {t.msg}
        </div>
      ))}
    </div>
  )
}
