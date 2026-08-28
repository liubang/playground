// useMazeData.ts — shared maze fetch for the maze tab and the trace
// view's rhythm strip. Fetches /maze once on session switch; activity
// signals (usage deltas, turn progress, state transitions) trigger a
// debounced refetch so the picture grows live with the run.

import { useCallback, useEffect, useRef, useState } from 'react'
import type { AppController } from '../../app/controller'
import { useStore } from '../../store/store'
import type { MazeData } from '../../protocol/types'

const REFRESH_DEBOUNCE_MS = 800

export function useMazeData(controller: AppController) {
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
        // The latest request always owns the loading flag — not just the one
        // that set it. A showLoading=true initial fetch superseded by a
        // debounced refetch would otherwise never be cleared (the superseded
        // request's guard fails, the superseding one had showLoading=false),
        // leaving the UI spinning forever.
        if (seqRef.current === seq) setLoading(false)
      }
    },
    [controller],
  )

  // Session switch: full refetch and view-state reset. data is cleared first
  // — otherwise the previous session's maze flashes under the new one (and,
  // worse, its non-null leftover would arm the activity-refetch effect below
  // during the initial fetch window).
  useEffect(() => {
    setData(null)
    setError('')
    if (!sessionId) return
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

  return { sessionId, data, error, loading }
}
