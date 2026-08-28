// rafScroll.ts — 轻量 onScroll rAF 节流 hook。
//
// 原始 onScroll 直接在事件回调里同步读 DOM + 调 controller：滚动事件在
// 惯性滚动 / 触控板 / 鼠标滚轮高频触发，每个事件都做 layout 读取与 store
// 写入，会把主线程拉满、掉帧。把实际工作搬到下一帧执行，期间多次 scroll
// 事件合并为一次。
//
// 用法：
//   const onScroll = useRafScroll((el) => { ... })
//   <div onScroll={onScroll} />
//
// 回调在 rAF 里执行；组件卸载时自动取消挂起的帧。

import { useCallback, useRef } from 'react'

export function useRafScroll<T extends HTMLElement>(
  handler: (el: T) => void,
): (e: { currentTarget: T }) => void {
  const raf = useRef<number | null>(null)
  const elRef = useRef<T | null>(null)

  const flush = useCallback(() => {
    raf.current = null
    const el = elRef.current
    if (el) handler(el)
    elRef.current = null
  }, [handler])

  const onScroll = useCallback(
    (e: { currentTarget: T }) => {
      elRef.current = e.currentTarget
      if (raf.current !== null) return // 已有挂起帧：合并
      raf.current = requestAnimationFrame(flush)
    },
    [flush],
  )

  return onScroll
}
