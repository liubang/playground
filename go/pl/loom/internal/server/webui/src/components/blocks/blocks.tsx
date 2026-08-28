// blocks.tsx — base transcript block renderers (user/assistant/stream/reasoning/
// thinking/notice/resolved/fatal/interrupted/compact).
// Iron rule: all model/tool text goes through textContent only; MarkdownView
// (marked → DOMPurify) is the sole markdown rendering entry.

import { memo, useEffect, useRef } from 'react'
import { useRafScroll } from '../../lib/rafScroll'
import type { AssistantActionContext, UserImage } from '../../app/transcript'
import { isInlineImage } from '../../app/transcript'
import type { ContextCompactedPayload } from '../../protocol/events'
import { fmtBytes, fmtDuration, fmtTokens } from '../../lib/format'
import { Icon } from '../../lib/icons'
import { markdownStableBoundary, renderMarkdown } from '../../lib/markdown'
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
  const mdRef = useRef<HTMLDivElement>(null)
  const tailRef = useRef<HTMLDivElement>(null)
  // Incremental rendering cache: the closed prefix is rendered once and its
  // HTML string is cached; React's dangerouslySetInnerHTML diff skips the
  // innerHTML write when the string is unchanged, so the prefix DOM is not
  // rebuilt. Only the live tail is re-rendered each tick (it is short).
  const cacheRef = useRef({ stableText: '', stableHtml: '' })

  const end = markdownStableBoundary(text)
  const stableText = text.slice(0, end)
  const cache = cacheRef.current
  let stableHtml: string
  if (stableText === cache.stableText) {
    stableHtml = cache.stableHtml
  } else if (stableText.startsWith(cache.stableText)) {
    // A new block just closed: render only the newly-closed delta and append to
    // the cached prefix. The boundary is always a block boundary, so rendering
    // the delta independently is equivalent to re-rendering the whole prefix.
    const delta = stableText.slice(cache.stableText.length)
    stableHtml = cache.stableHtml + renderMarkdown(delta)
    cache.stableText = stableText
    cache.stableHtml = stableHtml
  } else {
    // Non-prefix (text was edited/rewound mid-stream — rare): full re-render.
    stableHtml = renderMarkdown(stableText)
    cache.stableText = stableText
    cache.stableHtml = stableHtml
  }
  const tailHtml = renderMarkdown(text.slice(end))

  useEffect(() => {
    const md = mdRef.current
    if (!md) return
    // Cursor follows the end of rendered content: embedded into the last node
    // when it is a paragraph/list item so it doesn't take its own line. The
    // tail is wrapped in a display:contents container, so the last content node
    // lives inside the tail wrapper.
    const cursor = document.createElement('span')
    cursor.className = 'stream-cursor'
    cursor.textContent = '▍'
    const tail = tailRef.current
    const last = tail && tail.lastElementChild ? tail.lastElementChild : md.lastElementChild
    if (last && (last.tagName === 'P' || last.tagName === 'LI')) last.appendChild(cursor)
    else md.appendChild(cursor)
    return () => cursor.remove()
  }, [text, tailHtml])

  return (
    <div className="block block-assistant">
      <div ref={mdRef} className="md">
        <div
          className="md-stable"
          style={{ display: 'contents' }}
          dangerouslySetInnerHTML={{ __html: stableHtml }}
        />
        <div
          ref={tailRef}
          className="md-tail"
          style={{ display: 'contents' }}
          dangerouslySetInnerHTML={{ __html: tailHtml }}
        />
      </div>
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
  // The expanded body is a 320px-capped inner scroll container: while live, pin
  // it to the bottom on each delta so the newest thinking is visible (native
  // scroll never follows appended content). stick tracks whether the user has
  // scrolled up inside the body — same semantics as the outer transcript's
  // following flag; expanding always re-pins to the latest first.
  const bodyRef = useRef<HTMLDivElement>(null)
  const stickRef = useRef(true)
  // Hoist hook + handler to the top of the component: the inline arrow in JSX
  // below would otherwise be a new function each render, defeating the hook's
  // internal useCallback caching (and every streaming delta re-renders this
  // component, so the handler churn is per-frame).
  const onBodyScroll = useRafScroll<HTMLDivElement>((el) => {
    stickRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40
  })
  useEffect(() => {
    const body = bodyRef.current
    if (!active || !body || !stickRef.current) return
    body.scrollTop = body.scrollHeight
  }, [text, active])
  return (
    <details
      className={'block block-reasoning disclosure' + (active ? ' is-live' : '')}
      onToggle={(e) => {
        if ((e.target as HTMLDetailsElement).open && active) {
          stickRef.current = true
          const body = bodyRef.current
          if (body) body.scrollTop = body.scrollHeight
        }
      }}
    >
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
      <div className="body" ref={bodyRef} onScroll={onBodyScroll}>
        {text}
      </div>
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
