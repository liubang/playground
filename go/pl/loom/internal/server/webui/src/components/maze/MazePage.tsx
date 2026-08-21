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

  if (!sessionId) return <div className="maze-empty">未选择会话</div>
  if (error) return <div className="maze-error">轨迹加载失败：{error}</div>
  if (!data || loading) return <div className="maze-empty">正在构建轨迹…</div>
  if (data.lanes.length === 0 || data.lanes[0].stats.steps === 0) {
    return <div className="maze-empty">暂无执行轨迹——发起一轮对话后这里会画出探索迷宫</div>
  }
  return (
    <div className="maze-page">
      <MazeView data={data} onLocateStep={locateStep} />
    </div>
  )
}
