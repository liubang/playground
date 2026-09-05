// rafScroll.ts — lightweight onScroll rAF-throttling hook.
//
// Raw onScroll reads the DOM synchronously and calls the controller right in
// the event callback: scroll events fire at high frequency during inertial
// scrolling / trackpad / mouse wheel; a layout read plus a store write per
// event saturates the main thread and drops frames. Move the real work to the
// next frame instead, coalescing multiple scroll events in between into one.
//
// Usage:
//   const onScroll = useRafScroll((el) => { ... })
//   <div onScroll={onScroll} />
//
// The callback runs in rAF; pending frames are auto-cancelled on unmount.

import { useCallback, useEffect, useLayoutEffect, useRef } from 'react'

export function useRafScroll<T extends HTMLElement>(
  handler: (el: T) => void,
): (e: { currentTarget: T }) => void {
  const raf = useRef<number | null>(null)
  const elRef = useRef<T | null>(null)
  // Hold the latest handler in a ref: an unstable handler (e.g. an arrow function created
  // in JSX) must not invalidate the useCallback cache — onScroll / flush keep the same
  // reference for the component's whole lifetime, so the DOM listener needn't be rebuilt per render.
  // Written synchronously in a layout effect, not during render: under React 18 concurrent
  // mode renders can be aborted/replayed, and mutating a ref during render reads unpredictable
  // values; layout effects run synchronously at commit, always pointing at the last committed handler.
  const handlerRef = useRef(handler)
  useLayoutEffect(() => {
    handlerRef.current = handler
  }, [handler])

  // Unmount: cancel the pending rAF, avoiding setState captured in the handler firing on an unmounted component
  useEffect(
    () => () => {
      if (raf.current !== null) {
        cancelAnimationFrame(raf.current)
        raf.current = null
      }
      elRef.current = null
    },
    [],
  )

  const flush = useCallback(() => {
    raf.current = null
    const el = elRef.current
    if (el) handlerRef.current(el)
    elRef.current = null
  }, [])

  const onScroll = useCallback(
    (e: { currentTarget: T }) => {
      elRef.current = e.currentTarget
      if (raf.current !== null) return // a frame is already pending: coalesce
      raf.current = requestAnimationFrame(flush)
    },
    [flush],
  )

  return onScroll
}
