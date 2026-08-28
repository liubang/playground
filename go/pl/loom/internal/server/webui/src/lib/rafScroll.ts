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

import { useCallback, useEffect, useLayoutEffect, useRef } from 'react'

export function useRafScroll<T extends HTMLElement>(
  handler: (el: T) => void,
): (e: { currentTarget: T }) => void {
  const raf = useRef<number | null>(null)
  const elRef = useRef<T | null>(null)
  // 用 ref 持有最新 handler：unstable handler（如在 JSX 中创建的箭头函数）
  // 不会让 useCallback 缓存失效，onScroll / flush 在整个组件生命周期内
  // 保持同一引用，因此挂在 DOM 上的监听器不需要每次渲染重建。
  // 同步写在 layout effect 里而不是渲染期：React 18 并发模式下渲染可能被
  // 中止/重放，渲染期变异 ref 会读到不可预测的值；layout effect 在 commit
  // 阶段同步执行，始终指向最后一个已提交的 handler。
  const handlerRef = useRef(handler)
  useLayoutEffect(() => {
    handlerRef.current = handler
  }, [handler])

  // 组件卸载：取消挂起的 rAF，避免对已卸载组件触发 handler 里捕获的 setState
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
      if (raf.current !== null) return // 已有挂起帧：合并
      raf.current = requestAnimationFrame(flush)
    },
    [flush],
  )

  return onScroll
}
