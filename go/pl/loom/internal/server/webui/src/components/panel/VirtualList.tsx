// VirtualList.tsx — fixed-row-height windowed list.
//
// Only the rows intersecting the viewport (plus a small overscan) are mounted,
// so a directory with 1000 entries renders ~20 DOM nodes instead of 1000. The
// component owns its scroll container: callers pass the flex:1/min-height:0
// panel-body class so it becomes the scrollable region.
//
// Rows must be a uniform height (itemHeight). `imperativeRef` exposes
// scrollToIndex so keyboard navigation can keep the cursor row visible.

import {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useRef,
  useState,
  type KeyboardEvent,
  type ReactNode,
} from 'react'

export interface VirtualListHandle {
  scrollToIndex: (index: number, align?: 'center' | 'nearest') => void
  scrollToTop: () => void
}

interface VirtualListProps {
  count: number
  itemHeight: number
  overscan?: number
  className?: string
  tabIndex?: number
  id?: string
  role?: string
  ariaLabel?: string
  ariaLabelledBy?: string
  onKeyDown?: (e: KeyboardEvent<HTMLDivElement>) => void
  render: (index: number) => ReactNode
}

export const VirtualList = forwardRef<VirtualListHandle, VirtualListProps>(function VirtualList(
  {
    count,
    itemHeight,
    overscan = 8,
    className,
    tabIndex,
    id,
    role,
    ariaLabel,
    ariaLabelledBy,
    onKeyDown,
    render,
  },
  ref,
) {
  const scrollerRef = useRef<HTMLDivElement | null>(null)
  const [scrollTop, setScrollTop] = useState(0)
  const [viewport, setViewport] = useState(0)

  useEffect(() => {
    const el = scrollerRef.current
    if (!el) return
    const measure = () => setViewport(el.clientHeight)
    measure()
    // The first frame can measure 0 (the panel mounts in the same frame its
    // layout settles); re-measure next frame and on window resize so the window
    // count is never stuck at the overscan minimum.
    const raf = requestAnimationFrame(measure)
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    window.addEventListener('resize', measure)
    return () => {
      cancelAnimationFrame(raf)
      ro.disconnect()
      window.removeEventListener('resize', measure)
    }
  }, [])

  const scrollToIndex = useCallback(
    (index: number, align: 'center' | 'nearest' = 'nearest') => {
      const el = scrollerRef.current
      if (!el) return
      const top = index * itemHeight
      const bottom = top + itemHeight
      if (align === 'center') {
        el.scrollTop = Math.max(0, top - el.clientHeight / 2 + itemHeight / 2)
      } else if (top < el.scrollTop) {
        el.scrollTop = top
      } else if (bottom > el.scrollTop + el.clientHeight) {
        el.scrollTop = bottom - el.clientHeight
      }
    },
    [itemHeight],
  )

  useImperativeHandle(
    ref,
    () => ({
      scrollToIndex,
      scrollToTop: () => {
        if (scrollerRef.current) scrollerRef.current.scrollTop = 0
      },
    }),
    [scrollToIndex],
  )

  const total = count * itemHeight
  const start = Math.max(0, Math.floor(scrollTop / itemHeight) - overscan)
  const end = Math.min(count, Math.ceil((scrollTop + viewport) / itemHeight) + overscan)

  const rows: ReactNode[] = []
  for (let i = start; i < end; i++) {
    rows.push(
      <div
        key={i}
        className="vlist-row"
        style={{
          position: 'absolute',
          top: i * itemHeight,
          left: 0,
          right: 0,
          height: itemHeight,
        }}
      >
        {render(i)}
      </div>,
    )
  }

  return (
    <div
      ref={scrollerRef}
      className={className}
      tabIndex={tabIndex}
      id={id}
      role={role}
      aria-label={ariaLabel}
      aria-labelledby={ariaLabelledBy}
      onKeyDown={onKeyDown}
      onScroll={(e) => setScrollTop((e.target as HTMLDivElement).scrollTop)}
    >
      <div className="vlist-inner" style={{ height: total, position: 'relative' }}>
        {rows}
      </div>
    </div>
  )
})
