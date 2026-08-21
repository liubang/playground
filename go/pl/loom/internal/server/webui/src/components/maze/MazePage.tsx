// MazePage.tsx — entry A: the current session's execution trace, filling
// the main area when the Header trace button toggles out of chat. Fetches
// /maze once on open; session activity signals (usage deltas, turn
// progress, state transitions) trigger a debounced refetch so the maze
// grows live with the run.

import { useCallback, useEffect, useRef, useState } from 'react'
import type { AppController } from '../../app/controller'
import { useStore } from '../../store/store'
import type { MazeData, MazeNode } from '../../protocol/types'
import { MazeView } from './MazeView'

const REFRESH_DEBOUNCE_MS = 800
// Polling window for the chat jump: after switching back to chat, the
// target tool row may still be rendering.
const JUMP_SEEK_TRIES = 40
const JUMP_SEEK_INTERVAL_MS = 100

export function MazePage({ controller }: { controller: AppController }) {
  const sessionId = useStore(controller.store, (s) => s.sessionId)
  const sessionState = useStore(controller.store, (s) => s.sessionState)
  const turnCount = useStore(controller.store, (s) => s.turnCount)
  const usage = useStore(controller.store, (s) => s.usage)
  const [data, setData] = useState<MazeData | null>(null)
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(false)
  const seqRef = useRef(0)

  const refresh = useCallback(
    async (sid: string, showLoading: boolean) => {
      const seq = ++seqRef.current
      if (showLoading) setLoading(true)
      try {
        const d = await controller.api.maze(sid)
        if (seqRef.current !== seq) return // session switched; drop the stale response
        setData(d)
        setError('')
      } catch (e) {
        if (seqRef.current !== seq) return
        setError((e as Error).message || '加载失败')
      } finally {
        if (seqRef.current === seq && showLoading) setLoading(false)
      }
    },
    [controller],
  )

  // Session switch: full refetch and view-state reset.
  useEffect(() => {
    if (!sessionId) {
      setData(null)
      return
    }
    void refresh(sessionId, true)
  }, [sessionId, refresh])

  // Activity signals, debounced: budget.updated (after every model
  // response) / turn progress / state transitions.
  useEffect(() => {
    if (!sessionId || data === null) return
    const t = setTimeout(() => void refresh(sessionId, false), REFRESH_DEBOUNCE_MS)
    return () => clearTimeout(t)
    // data is deliberately not a dependency: refetch only on activity
    // signals, never on our own fetch, avoiding a feedback loop.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [usage, turnCount, sessionState, sessionId, refresh])

  // Locate the step in chat: switch back, then scroll-highlight the first
  // tool call's data-call-id anchor; a tool-less answer degrades to the
  // view switch alone.
  const jumpToChat = useCallback(
    (node: MazeNode) => {
      controller.toggleMainView()
      const callId = node.tools[0]?.call_id
      if (!callId) return
      let tries = 0
      const seek = () => {
        const root = controller.scrollerRef.el
        const target = root?.querySelector(`[data-call-id="${CSS.escape(callId)}"]`)
        if (target instanceof HTMLElement) {
          target.scrollIntoView({ block: 'center', behavior: 'smooth' })
          target.animate(
            [{ boxShadow: '0 0 0 3px var(--primary)' }, { boxShadow: '0 0 0 3px transparent' }],
            { duration: 1800, easing: 'ease-out' },
          )
          return
        }
        if (++tries < JUMP_SEEK_TRIES) setTimeout(seek, JUMP_SEEK_INTERVAL_MS)
      }
      seek()
    },
    [controller],
  )

  if (!sessionId) return <div className="maze-empty">未选择会话</div>
  if (error) return <div className="maze-error">轨迹加载失败：{error}</div>
  if (!data || loading) return <div className="maze-empty">正在构建轨迹…</div>
  if (data.lanes.length === 0 || data.lanes[0].stats.steps === 0) {
    return <div className="maze-empty">暂无执行轨迹——发起一轮对话后这里会画出探索迷宫</div>
  }
  return (
    <div className="maze-page">
      <MazeView data={data} onJumpToChat={jumpToChat} />
    </div>
  )
}
