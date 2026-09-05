// store.ts — micro-kernel state container + React binding.
// Store is framework-agnostic (get/set/subscribe); components subscribe to
// slices via useSyncExternalStore; same semantics as the legacy static/js/store.js, with selector capability added.

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

// useStore subscribes to one slice of the store. The selector should return a
// primitive or a stable reference; for object slices, useMemo in the component or return a shallow-compare-friendly structure from the selector.
export function useStore<T, S>(store: Store<T>, selector: (s: T) => S): S {
  return useSyncExternalStore(store.subscribe, () => selector(store.get()))
}
