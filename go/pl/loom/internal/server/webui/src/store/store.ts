// store.ts — 微内核状态容器 + React 绑定。
// Store 是框架无关的（get/set/subscribe），组件经 useSyncExternalStore
// 订阅切片；与旧 static/js/store.js 同语义，增加 selector 能力。

import { useSyncExternalStore } from 'react'

export class Store<T> {
  private state: T
  private listeners = new Set<() => void>()

  constructor(initial: T) {
    this.state = initial
  }

  get = (): T => this.state

  // Shallow-merge patch and notify.
  set(patch: Partial<T>) {
    this.state = { ...this.state, ...patch }
    this.emit()
  }

  // Mutate draft in place and notify (for nested structures).
  update(fn: (s: T) => void) {
    fn(this.state)
    this.emit()
  }

  subscribe = (fn: () => void): (() => void) => {
    this.listeners.add(fn)
    return () => {
      this.listeners.delete(fn)
    }
  }

  protected emit() {
    for (const fn of this.listeners) fn()
  }
}

// useStore 订阅 store 的一个切片。selector 应返回原始值或稳定引用；
// 需要对象切片时在组件内 useMemo，或在 selector 内返回浅比较友好结构。
export function useStore<T, S>(store: Store<T>, selector: (s: T) => S): S {
  return useSyncExternalStore(store.subscribe, () => selector(store.get()))
}
