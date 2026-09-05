// MessageActions.tsx — message action row (copy / thumbs up / thumbs down) + time.
// Attached at the end of a block, always visible after the turn ends: it is both the feedback/copy entry and
// the marker that this message has finished. role: "user" shows copy only; "assistant" shows copy + thumbs up/down.
// Logic corresponds one-to-one with the old blocks.js messageActions.

import { memo, useEffect, useRef, useState } from 'react'
import { fmtMsgTime, fmtMsgTimeTitle, copyText } from '../../lib/format'
import { Icon } from '../../lib/icons'

export interface MessageActionsProps {
  role: 'user' | 'assistant'
  createdAt?: string
  getText: () => string | Promise<string>
  // fb (optional feedback context): if runId is empty, don't render up/down (old messages have no trace to vote on)
  fb?: {
    runId?: string
    feedback?: string // "up" | "down" | ""
    onFeedback?: (runId: string, value: 0 | 1) => Promise<unknown>
  }
}

export const MessageActions = memo(function MessageActions({
  role,
  createdAt,
  getText,
  fb,
}: MessageActionsProps) {
  const [copyState, setCopyState] = useState<'' | 'is-done' | 'is-fail'>('')
  const [vote, setVote] = useState(fb?.feedback || '')
  // Copy-state reset timer: tracked and cleaned up on unmount/rapid clicks (a bare setTimeout would setState on an unmounted
  // component; rapid clicks would leave multiple timers racing each other)
  const copyTimer = useRef(0)
  useEffect(() => () => window.clearTimeout(copyTimer.current), [])

  const tip = createdAt ? (
    <span className="msg-time-tip" title={fmtMsgTimeTitle(createdAt)}>
      {fmtMsgTime(createdAt)}
    </span>
  ) : null

  const onCopy = async (e: React.MouseEvent) => {
    e.stopPropagation()
    try {
      const text = await getText()
      // copyText has a built-in fallback for non-secure contexts (intranet IPs); failures throw and go down the is-fail path
      if (!(await copyText(text))) throw new Error('clipboard unavailable')
      setCopyState('is-done')
    } catch {
      setCopyState('is-fail')
    }
    window.clearTimeout(copyTimer.current)
    copyTimer.current = window.setTimeout(() => setCopyState(''), 1500)
  }

  const onVote = (value: 0 | 1) => async (e: React.MouseEvent) => {
    e.stopPropagation()
    if (!fb?.runId || !fb.onFeedback) return
    const want = value === 1 ? 'up' : 'down'
    if (vote === want) return // already cast that vote: no-op
    const prev = vote
    setVote(want) // clicking the other button overrides the previous vote (the backend idempotently overrides by deterministic score id)
    try {
      await fb.onFeedback(fb.runId, value)
    } catch {
      setVote(prev) // submission failed: roll back to the pre-click selection
    }
  }

  return (
    <div className="msg-actions">
      {tip && role === 'user' && tip}
      <button
        type="button"
        className={'msg-action msg-copy' + (copyState ? ' ' + copyState : '')}
        title="复制该条消息"
        aria-label="复制该条消息"
        onClick={onCopy}
      >
        <Icon name={copyState === 'is-done' ? 'check' : 'copy'} />
      </button>
      {role === 'assistant' && fb?.runId && fb.onFeedback && (
        <>
          <button
            type="button"
            className={'msg-action msg-up' + (vote === 'up' ? ' is-active' : '')}
            title="赞"
            aria-label="赞"
            onClick={onVote(1)}
          >
            <Icon name="thumbs-up" />
          </button>
          <button
            type="button"
            className={'msg-action msg-down' + (vote === 'down' ? ' is-active' : '')}
            title="踩"
            aria-label="踩"
            onClick={onVote(0)}
          >
            <Icon name="thumbs-down" />
          </button>
        </>
      )}
      {tip && role !== 'user' && tip}
    </div>
  )
})
