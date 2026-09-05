// CompareView.tsx — entry C: the trace-compare view (sidebar 「对比」button),
// filling the main area like the single-session maze. Two sessions on a
// shared axis: each lane's clock starts at its own first user message,
// turn-alignment lines annotate arrival-time deltas and per-turn
// detour-count diffs, and the audit table lists per-turn detour gaps.

import { useEffect, useMemo, useState } from 'react'
import type { AppController } from '../../app/controller'
import { useStore } from '../../store/store'
import type { MazeData } from '../../protocol/types'
import { shortId } from '../../lib/format'
import { Icon } from '../../lib/icons'
import { Select } from '../ui/Select'
import { MazeView } from './MazeView'

export function CompareView({ controller }: { controller: AppController }) {
  const sessions = useStore(controller.store, (s) => s.sessions)
  const activeId = useStore(controller.store, (s) => s.sessionId)
  const [id1, setId1] = useState<string>(activeId ?? '')
  const [id2, setId2] = useState<string>('')
  const [data, setData] = useState<MazeData | null>(null)
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(false)

  // Esc returns to the chat view.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') controller.closeCompare()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [controller])

  // Fetch once both lanes are selected (each side's maze already carries
  // its sub-agent branches and verdicts from the Go builder).
  useEffect(() => {
    if (!id1 || !id2) {
      setData(null)
      return
    }
    let cancelled = false
    setLoading(true)
    void (async () => {
      try {
        const [a, b] = await Promise.all([controller.api.maze(id1), controller.api.maze(id2)])
        if (cancelled) return
        const lane1 = a.lanes[0]
        const lane2 = b.lanes[0]
        if (!lane1 || !lane2) throw new Error('会话还没有执行轨迹')
        // Spread instead of mutating the fetched lane objects: the response
        // belongs to the API layer — writing `key` onto it would leak into
        // any future reuse of the same payload.
        setData({
          tmax: Math.max(a.tmax, b.tmax),
          lanes: [
            { ...lane1, key: 'l1' },
            { ...lane2, key: 'l2' },
          ],
        })
        setError('')
      } catch (e) {
        if (!cancelled) {
          setData(null)
          setError((e as Error).message || '加载失败')
        }
      } finally {
        if (!cancelled) setLoading(false)
      }
    })()
    return () => {
      cancelled = true
    }
  }, [id1, id2, controller])

  // Codebase-wide custom dropdown (native select popovers are OS-rendered, clashing with the dark theme).
  // The session already picked on the other side is excluded from the options (native option disabled).
  const options = useMemo(
    () =>
      sessions.map((s) => ({
        value: s.id,
        label: `${s.title || shortId(s.id)}${s.model_name ? ` · ${s.model_name}` : ''}`,
      })),
    [sessions],
  )
  const toOptions = (placeholder: string, exclude: string) => [
    { value: '', label: placeholder },
    ...options.filter((o) => o.value !== exclude),
  ]

  return (
    <div className="compare-page">
      <div className="compare-head">
        <span className="compare-title">
          <Icon name="layer-group" /> 轨迹对比
        </span>
        <div className="compare-pickers">
          <span className="compare-picker">
            <i className="lane-dot lane-1" />
            <Select
              className="compare-sel"
              options={toOptions('选择会话 1…', id2)}
              value={id1}
              onChange={setId1}
            />
          </span>
          <button
            type="button"
            className="icon-btn compare-swap"
            title="交换两侧"
            onClick={() => {
              setId1(id2)
              setId2(id1)
            }}
          >
            ⇄
          </button>
          <span className="compare-picker">
            <i className="lane-dot lane-2" />
            <Select
              className="compare-sel"
              options={toOptions('选择会话 2…', id1)}
              value={id2}
              onChange={setId2}
            />
          </span>
        </div>
        <button
          type="button"
          className="icon-btn compare-close"
          title="返回对话（Esc）"
          onClick={() => controller.closeCompare()}
        >
          <Icon name="xmark" />
        </button>
      </div>
      <div className="compare-body">
        {error ? (
          <div className="maze-error">对比加载失败：{error}</div>
        ) : loading ? (
          <div className="maze-empty">正在构建对比…</div>
        ) : !data ? (
          <div className="maze-empty">
            选择两个会话开始同轴对比——比如同一任务在不同模型下的跑法差异
          </div>
        ) : (
          <MazeView data={data} compare />
        )}
      </div>
    </div>
  )
}
