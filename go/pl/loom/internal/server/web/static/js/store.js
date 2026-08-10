// store.js — 微内核状态容器（docs/WEB_DESIGN.md §3.2）。
// 单一事实源：组件 subscribe，action 以 set/update 触发广播。

export function createStore(initial) {
  let state = initial
  const listeners = new Set()
  return {
    get: () => state,
    // Shallow-merge patch and notify.
    set(patch) {
      state = { ...state, ...patch }
      for (const fn of listeners) fn(state)
    },
    // Mutate draft in place and notify (for nested maps).
    update(fn) {
      fn(state)
      for (const fn2 of listeners) fn2(state)
    },
    subscribe(fn) {
      listeners.add(fn)
      return () => listeners.delete(fn)
    },
  }
}
