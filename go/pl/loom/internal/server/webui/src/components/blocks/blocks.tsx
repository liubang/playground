// blocks.tsx — base transcript block renderers (user/assistant/stream/reasoning/
// thinking/notice/resolved/fatal/interrupted/compact).
// Iron rule: all model/tool text goes through textContent only; MarkdownView
// (marked → DOMPurify) is the sole markdown rendering entry.

import { memo, useEffect, useRef } from 'react'
import type { AssistantActionContext, UserImage } from '../../app/transcript'
import { isInlineImage } from '../../app/transcript'
import type { ContextCompactedPayload } from '../../protocol/events'
import { fmtBytes, fmtDuration, fmtTokens } from '../../lib/format'
import { Icon } from '../../lib/icons'
import { renderMarkdown } from '../../lib/markdown'
import { MarkdownView } from './MarkdownView'
import { MessageActions } from './MessageActions'
import { ArtifactImage, InlineImage } from './images'

// --- user: right-side bubble, no label; action row right-aligned below the bubble ---

// Mirrors the server-side app.LoomContextMark: the context block appended on
// submit after resolving @file and /skill refs. The bubble shows only the user's
// original text; injected content collapses into a chip (this is exactly what
// the model sees).
const LOOM_CONTEXT_MARK = '<loom-context>'

function LoomContextChip({ ctx }: { ctx: string }) {
  const files =
    (ctx.match(/<file path=/g) || []).length + (ctx.match(/<directory path=/g) || []).length
  const skills = (ctx.match(/<skill name=/g) || []).length
  const parts: string[] = []
  if (files) parts.push(`${files} 个文件/目录`)
  if (skills) parts.push(`${skills} 个技能`)
  return (
    <details className="user-ctx disclosure">
      <summary>
        <Icon name="file" /> 已注入{parts.join(' · ') || '引用内容'}（模型上下文）
      </summary>
      <pre className="user-ctx-body mono">{ctx}</pre>
    </details>
  )
}

export const UserBlock = memo(function UserBlock({
  text,
  createdAt,
  images,
}: {
  text: string
  createdAt?: string
  images?: UserImage[]
}) {
  const ctxIdx = text.indexOf(LOOM_CONTEXT_MARK)
  const main = ctxIdx < 0 ? text : text.slice(0, ctxIdx).trimEnd()
  const ctx = ctxIdx < 0 ? '' : text.slice(ctxIdx)
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
        {(main || !images || images.length === 0) && <div className="user-text">{main}</div>}
        {ctx && <LoomContextChip ctx={ctx} />}
      </div>
      <MessageActions role="user" createdAt={createdAt} getText={() => text} />
    </div>
  )
})

// --- assistant (final state, markdown rendered): the transcript attaches the
// action row to the last segment once at turn end (actions field), avoiding the
// flicker of it appearing on and disappearing from intermediate segments ---

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

// --- stream (in-progress draft, live markdown rendering) ---
// Render throttling lives in the controller (60ms + rAF); the component only
// renders the current buffer. Cursor: embedded into the last node when it is a
// paragraph/list item (done by the DOM-side effect).

export const StreamBlock = memo(function StreamBlock({ text }: { text: string }) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const md = ref.current
    if (!md) return
    // Cursor follows the end of rendered content: embedded into the last node
    // when it is a paragraph/list item so it doesn't take its own line
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
        // Sanitize pipeline: see lib/markdown.ts (the app's only innerHTML entry)
        dangerouslySetInnerHTML={{ __html: renderMarkdown(text) }}
      />
    </div>
  )
})

// --- thinking (three-dot animation while awaiting the model's first token / between tools) ---

export function ThinkingBlock() {
  return (
    <div className="block block-thinking">
      <span className="t-dot" />
      <span className="t-dot" />
      <span className="t-dot" />
    </div>
  )
}

// --- reasoning (collapsible block) ---

// Takes the first/last non-empty line as the summary (finished) or streaming tail
// preview (in progress), truncated to ~96 chars.
function reasoningExcerpt(text: string, fromEnd: boolean): string {
  const lines = (text || '')
    .split('\n')
    .map((l) => l.trim())
    .filter(Boolean)
  if (!lines.length) return ''
  const line = fromEnd ? lines[lines.length - 1] : lines[0]
  return line.length > 96 ? line.slice(0, 96) + '…' : line
}

// Collapsed line: while streaming, "thinking… + tail-line preview" (opens the
// black box so drift is caught early); once finished, "thought for Xs + first-line
// summary" (history stays scannable). Char counts have no scan value — dropped.
// active requires both live and no finalized duration: live residue leaked by any
// path (dropped events / reconnects / provider quirks) never makes a finalized
// block keep pulsing or recoloring — a hard view-layer guarantee.
export const ReasoningBlock = memo(function ReasoningBlock({
  text,
  durationMs,
  live,
}: {
  text: string
  durationMs?: number
  live?: boolean
}) {
  const active = !!live && durationMs == null
  const head = active
    ? 'thinking…'
    : durationMs != null
      ? `thought for ${fmtDuration(durationMs)}`
      : 'reasoning'
  const summary = !active ? reasoningExcerpt(text, false) : ''
  const tail = active ? reasoningExcerpt(text, true) : ''
  return (
    <details className={'block block-reasoning disclosure' + (active ? ' is-live' : '')}>
      <summary>
        <Icon name="lightbulb" />
        {/* key=text.length: each delta remounts the element, refilling the bounded
            pulse; once deltas stop the animation drains naturally and can never
            flash forever */}
        <span className="r-head" key={text.length}>
          {head}
        </span>
        {summary && <span className="r-summary">{summary}</span>}
      </summary>
      {tail && (
        <div className="reasoning-tail" key={text.length}>
          {tail}
        </div>
      )}
      <div className="body">{text}</div>
    </details>
  )
})

// --- notice / resolved / fatal / interrupted / compact ---

export function NoticeBlock({ text, warn }: { text: string; warn?: boolean }) {
  return <div className={'notice' + (warn ? ' is-warn' : '')}>{text}</div>
}

// resolved supersedes notice (placeholder once an approval has been handled)
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

// Interrupted block: a persistent warning-colored block (distinct from fatal's
// error red) that renders assistant messages with status === 'interrupted'
// (truncated remnants of a failed model stream) during history rebuilds.
export function InterruptedBlock({ text }: { text: string }) {
  return <div className="block block-interrupted">{text}</div>
}

// context.compacted detail card: before/after estimates + trigger reason +
// per-level action details.
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
