// MessageActions.tsx — 消息操作行（复制 / 点赞 / 踩） + 时间。
// 挂在 block 末尾、对话结束后常显：既是反馈/复制入口，也是本条消息
// 已结束的标志。role: "user" 仅显示复制；"assistant" 显示复制 + 点赞/踩。
// 逻辑与旧 blocks.js messageActions 一一对应。

import { memo, useState } from 'react'
import { fmtMsgTime, fmtMsgTimeTitle, copyText } from '../../lib/format'
import { Icon } from '../../lib/icons'

export interface MessageActionsProps {
  role: 'user' | 'assistant'
  createdAt?: string
  getText: () => string | Promise<string>
  // fb（可选反馈上下文）：runId 空则不渲染赞/踩（旧消息无 trace 可投）
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

  const tip = createdAt ? (
    <span className="msg-time-tip" title={fmtMsgTimeTitle(createdAt)}>
      {fmtMsgTime(createdAt)}
    </span>
  ) : null

  const onCopy = async (e: React.MouseEvent) => {
    e.stopPropagation()
    try {
      const text = await getText()
      // copyText 内置非安全上下文（内网 IP）降级；失败抛错走 is-fail
      if (!(await copyText(text))) throw new Error('clipboard unavailable')
      setCopyState('is-done')
    } catch {
      setCopyState('is-fail')
    }
    setTimeout(() => setCopyState(''), 1500)
  }

  const onVote = (value: 0 | 1) => async (e: React.MouseEvent) => {
    e.stopPropagation()
    if (!fb?.runId || !fb.onFeedback) return
    const want = value === 1 ? 'up' : 'down'
    if (vote === want) return // 已投该票：no-op
    const prev = vote
    setVote(want) // 点另一个按钮覆盖上一票（后端按确定性 score id 幂等覆盖）
    try {
      await fb.onFeedback(fb.runId, value)
    } catch {
      setVote(prev) // 提交失败：回滚到点击前的选中态
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
