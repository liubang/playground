// MazePage.tsx — the maze tab: the current session's execution trace as
// a pill-timeline. Data comes from the shared useMazeData hook (live
// refetch on activity signals); locating a step switches to the trace
// tab and scrolls to its tool-call row.

import { useCallback } from 'react'
import type { AppController } from '../../app/controller'
import type { MazeNode } from '../../protocol/types'
import { seekToAnchor } from '../../lib/jump'
import { useMazeData } from './useMazeData'
import { MazeView } from './MazeView'

export function MazePage({ controller }: { controller: AppController }) {
  const { sessionId, data, error, loading } = useMazeData(controller)

  // Locate the step in the trace list: switch tab, then scroll-highlight
  // the first tool call's row; a tool-less answer lands on its turn.
  const locateStep = useCallback(
    (node: MazeNode) => {
      controller.setMainView('trace')
      const callId = node.tools[0]?.call_id
      seekToAnchor(
        () => controller.traceScrollerRef.el,
        callId ? `[data-call-id="${CSS.escape(callId)}"]` : `[data-turn="${node.turn}"]`,
      )
    },
    [controller],
  )

  if (!sessionId) return <div className="maze-empty">No session selected</div>
  if (error) return <div className="maze-error">Failed to load trace: {error}</div>
  if (!data || loading) return <div className="maze-empty">Building trace…</div>
  if (data.lanes.length === 0 || data.lanes[0].stats.steps === 0) {
    return (
      <div className="maze-empty">
        No execution trace yet — start a conversation and the exploration maze appears here
      </div>
    )
  }
  return (
    <div className="maze-page">
      <MazeView data={data} onLocateStep={locateStep} />
    </div>
  )
}
