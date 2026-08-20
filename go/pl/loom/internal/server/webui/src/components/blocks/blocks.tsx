// blocks.tsx — transcript 基础块渲染器（user/assistant/stream/reasoning/
// thinking/notice/resolved/fatal/interrupted/compact）。
// 铁律：一切模型/工具文本只走 textContent；markdown 渲染唯一入口是
// MarkdownView（marked → DOMPurify）。

import { memo, useEffect, useRef } from 'react'
import type { AssistantActionContext, UserImage } from '../../app/transcript'
import { isInlineImage } from '../../app/transcript'
import type { ContextCompactedPayload } from '../../protocol/events'
import { fmtBytes, fmtTokens } from '../../lib/format'
import { Icon } from '../../lib/icons'
import { renderMarkdown } from '../../lib/markdown'
import { MarkdownView } from './MarkdownView'
import { MessageActions } from './MessageActions'
import { ArtifactImage, InlineImage } from './images'

// --- user：右侧气泡，无标签；操作行在气泡下方右对齐 ---

export const UserBlock = memo(function UserBlock({
  text,
  createdAt,
  images,
}: {
  text: string
  createdAt?: string
  images?: UserImage[]
}) {
  return (
    <div className="block block-user">
      <div className="user-bubble">
        {images && images.length > 0 && (
          <div className="user-images">
            {images.map((ref, i) =>
              isInlineImage(ref) ? (
                <InlineImage key={i} mediaType={ref.media_type} data={ref.data} />
              ) : (
                <ArtifactImage key={ref.id || i} artifact={ref} />
              ),
            )}
          </div>
        )}
        {(text || !images || images.length === 0) && <div className="user-text">{text}</div>}
      </div>
      <MessageActions role="user" createdAt={createdAt} getText={() => text} />
    </div>
  )
})

// --- assistant（完成态，markdown 渲染）：操作行由 transcript 在轮结束时
// 一次性挂到末段（actions 字段），避免中间段出现又消失的闪烁 ---

export const AssistantBlock = memo(function AssistantBlock({
  text,
  actions,
  onFeedback,
}: {
  text: string
  actions?: AssistantActionContext
  onFeedback?: (runId: string, value: 0 | 1) => Promise<unknown>
}) {
  return (
    <div className="block block-assistant">
      <MarkdownView text={text} />
      {actions && (
        <MessageActions
          role="assistant"
          createdAt={actions.createdAt}
          getText={() => text}
          fb={
            actions.runId && onFeedback
              ? { runId: actions.runId, feedback: actions.feedback, onFeedback }
              : undefined
          }
        />
      )}
    </div>
  )
})

// --- stream（进行中草稿，markdown 实时渲染） ---
// 渲染节流在 controller 侧（60ms + rAF），组件只负责把当前 buffer 渲染出来。
// 光标：末节点是段落/列表项时嵌入其中（DOM 侧 effect 完成）。

export const StreamBlock = memo(function StreamBlock({ text }: { text: string }) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const md = ref.current
    if (!md) return
    // 光标跟随渲染内容末尾：末节点是段落/列表项时嵌入其中，避免独占一行
    const cursor = document.createElement('span')
    cursor.className = 'stream-cursor'
    cursor.textContent = '▍'
    const last = md.lastElementChild
    if (last && (last.tagName === 'P' || last.tagName === 'LI')) last.appendChild(cursor)
    else md.appendChild(cursor)
    return () => cursor.remove()
  }, [text])
  return (
    <div className="block block-assistant">
      <div
        ref={ref}
        className="md"
        // sanitize 管线见 lib/markdown.ts（全应用唯一 innerHTML 入口）
        dangerouslySetInnerHTML={{ __html: renderMarkdown(text) }}
      />
    </div>
  )
})

// --- thinking（等待模型首个 token / 工具间等待的三点动画） ---

export function ThinkingBlock() {
  return (
    <div className="block block-thinking">
      <span className="t-dot" />
      <span className="t-dot" />
      <span className="t-dot" />
    </div>
  )
}

// --- reasoning（折叠块） ---

export const ReasoningBlock = memo(function ReasoningBlock({ text }: { text: string }) {
  return (
    <details className="block block-reasoning disclosure">
      <summary>{`reasoning · ${text.length} chars`}</summary>
      <div className="body">{text}</div>
    </details>
  )
})

// --- notice / resolved / fatal / interrupted / compact ---

export function NoticeBlock({ text, warn }: { text: string; warn?: boolean }) {
  return <div className={'notice' + (warn ? ' is-warn' : '')}>{text}</div>
}

// resolved 收编 notice（审批被处理后的占位）
export function ResolvedNotice({ ok, actor, what }: { ok: boolean; actor: string; what: string }) {
  return (
    <div className="resolved">
      <span className={ok ? 'ok' : 'no'}>
        <Icon name={ok ? 'check' : 'xmark'} />
      </span>
      <span>
        <b>{(ok ? 'Allowed' : 'Denied') + ' '}</b>
        {`(${actor}) · ${what}`}
      </span>
    </div>
  )
}

export function FatalBlock({ text }: { text: string }) {
  return <div className="block block-fatal">{text}</div>
}

// 中断块：warning 色的持久块（区别于 fatal 的 error 红），用于历史重建时
// 渲染 status === 'interrupted' 的 assistant 消息（模型流中途失败的残段）。
export function InterruptedBlock({ text }: { text: string }) {
  return <div className="block block-interrupted">{text}</div>
}

// context.compacted 明细卡片：压缩前后估值 + 触发原因 + 各级动作明细。
export const CompactBlock = memo(function CompactBlock({
  payload: p,
}: {
  payload: ContextCompactedPayload
}) {
  const before = fmtTokens(p.est_tokens_before) || '?'
  const after = fmtTokens(p.est_tokens_after) || '?'
  const details: string[] = []
  if (p.trigger) details.push('trigger: ' + p.trigger)
  if (p.masked_outputs) {
    const bytes = p.masked_bytes ? ` (${fmtBytes(p.masked_bytes)})` : ''
    details.push(`mask ${p.masked_outputs} outputs${bytes}`)
  }
  if (p.archived_messages) details.push(`archive ${p.archived_messages} msgs`)
  if (p.summarized) details.push('summary handoff')
  return (
    <div className="notice compact">
      <div className="compact-head">
        <Icon name="bolt" />
        {` context compacted · ${before} → ${after}`}
      </div>
      {details.length > 0 && <div className="compact-detail">{details.join(' · ')}</div>}
    </div>
  )
})
