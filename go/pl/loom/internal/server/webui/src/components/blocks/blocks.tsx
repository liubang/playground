// blocks.tsx — base transcript block renderers (user/assistant/stream/reasoning/
// thinking/notice/resolved/fatal/interrupted/compact).
// Iron rule: all model/tool text goes through textContent only; MarkdownView
// (marked → DOMPurify) is the sole markdown rendering entry.

import { memo, useEffect, useMemo, useRef, useState } from 'react'
import { useRafScroll } from '../../lib/rafScroll'
import type { AssistantActionContext, UserImage } from '../../app/transcript'
import { isInlineImage } from '../../app/transcript'
import type { ContextCompactedPayload, TurnFileChange } from '../../protocol/events'
import type { RunChangeStat } from '../../protocol/types'
import { parseDiff } from '../../lib/diff'
import { fmtBytes, fmtDuration, fmtTokens } from '../../lib/format'
import { Icon } from '../../lib/icons'
import {
  markdownStableBoundaryCached,
  renderMarkdown,
  renderStreamTail,
  type BoundaryCache,
} from '../../lib/markdown'
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
  if (files) parts.push(`${files} ${files === 1 ? 'file/dir' : 'files/dirs'}`)
  if (skills) parts.push(`${skills} ${skills === 1 ? 'skill' : 'skills'}`)
  return (
    <details className="user-ctx disclosure">
      <summary>
        <Icon name="file" /> Injected {parts.join(' · ') || 'referenced content'} (model context)
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
  const cacheRef = useRef<{
    stableText: string
    stableHtml: string
    // Boundary scan cache: append-only streaming makes the full re-scan
    // incremental — see markdownStableBoundaryCached in lib/markdown.
    boundary: BoundaryCache | undefined
  }>({ stableText: '', stableHtml: '', boundary: undefined })

  const scanned = markdownStableBoundaryCached(text, cacheRef.current.boundary)
  const cache = cacheRef.current
  cache.boundary = scanned.cache
  const end = scanned.end
  const stableText = text.slice(0, end)
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
  // An unclosed code fence's growing body is rendered as plain text this tick (see
  // renderStreamTail); full highlighting kicks in the moment the fence closes.
  const tailHtml = renderStreamTail(text.slice(end))

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
  // Collapsed state: the (potentially huge) reasoning text is only inserted into the DOM
  // while expanded. A collapsed block used to receive a full text-node update per streaming
  // delta — tens of KB of DOM churn per tick that was invisible anyway.
  const [open, setOpen] = useState(false)
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
    if (!active || !open || !body || !stickRef.current) return
    body.scrollTop = body.scrollHeight
  }, [text, active, open])
  return (
    <details
      className={'block block-reasoning disclosure' + (active ? ' is-live' : '')}
      onToggle={(e) => {
        const isOpen = (e.target as HTMLDetailsElement).open
        setOpen(isOpen)
        if (isOpen && active) {
          stickRef.current = true
          // The body content mounts this same commit; scroll pinning happens in the
          // effect below once the text is in the DOM.
        }
      }}
    >
      <summary>
        <Icon name="lightbulb" />
        {/* The header breath is gated purely on the .is-live class (continuous CSS
            animation), so no key remount here — remounting per delta both churned
            the DOM each token and made the animation restart-strobe. */}
        <span className="r-head">{head}</span>
        {summary && <span className="r-summary">{summary}</span>}
      </summary>
      {tail && <div className="reasoning-tail">{tail}</div>}
      <div className="body" ref={bodyRef} onScroll={onBodyScroll}>
        {open ? text : ''}
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
        {`${what} · ${actor}`}
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

// --- turn summary (closing review card of a finished turn) ---

// InlineDiff renders the review diff (real unified hunks: @@ headers,
// +/-/space lines) WITHOUT DiffView's own frame/head — the turn-summary
// card supplies that chrome itself (diff rows sit directly under the file
// row, on one shared dark box). Text is COLORED BY LINE KIND (red/green,
// like the prototype) rather than syntax-highlighted — the changes panel
// DiffView stays the hljs surface. Overlong diffs cap at
// INLINE_DIFF_MAX_LINES with a fold note — the changes panel stays the
// canonical full view.
const INLINE_DIFF_MAX_LINES = 80

const InlineDiff = memo(function InlineDiff({
  path,
  diffText,
}: {
  path: string
  diffText: string
}) {
  const parsed = useMemo(() => parseDiff('+++ b/' + path + '\n' + diffText), [path, diffText])
  const lines = parsed.lines
  const shown = lines.length > INLINE_DIFF_MAX_LINES ? lines.slice(0, INLINE_DIFF_MAX_LINES) : lines
  return (
    // .tsm-diff clips the stripes to the rounded box; the inner scroller
    // keeps long lines horizontally reachable without breaking the clip.
    <div className="tsm-diff mono">
      <div className="tsm-diff-scroll">
        {shown.map((l, i) => {
          // Unified-hunk header: a full-width strip with no sign column.
          if (l.kind === 'hunk') {
            return (
              <div key={i} className="tsm-dline d-hunk">
                {l.text}
              </div>
            )
          }
          // The compact format's region separator (kept for compatibility;
          // the review endpoint now emits real @@ hunks) — a subtle
          // ellipsis row rather than code content.
          if (l.kind === 'ctx' && l.text === '...') {
            return (
              <div key={i} className="tsm-dline d-sep">
                ⋯
              </div>
            )
          }
          const kind = l.kind === 'add' ? 'd-add' : l.kind === 'del' ? 'd-del' : 'd-ctx'
          // No hljs here: the prototype colors the whole line red/green.
          return (
            <div key={i} className={'tsm-dline ' + kind}>
              <span className="d-sign">{l.sign}</span>
              <code>{l.text}</code>
            </div>
          )
        })}
        {lines.length > shown.length && (
          <div className="tsm-dline d-ctx tsm-diff-folded">
            ⋯ {lines.length - shown.length} more lines folded — use “View all in Changes” for the
            full diff
          </div>
        )}
      </div>
    </div>
  )
})

// The turn's write-tool file projection, appended as the last block of a
// finished turn (live: turn.finished payload; rebuild: snapshot
// turn_summaries). The file LIST is expanded by default; per-file diffs
// start collapsed and are reset whenever the card itself is collapsed.
//
// Stat +/− and the per-file inline diff come from the per-turn review
// endpoint (fetchRunChanges): ledger-before vs CURRENT workspace content —
// no git involved, so the affordance also works in non-git workspaces (the
// honest cost: any edits made after the turn are part of the comparison;
// after a revert the numbers drop to 0 — "unchanged"). Blocks built before
// the ledger existed, and the share page, leave stats null and keep static
// rows. The revert button restores the recorded before-content (conflicts
// are overwritten and then reported).
export const TurnSummaryBlock = memo(function TurnSummaryBlock({
  changes,
  cancelled,
  failed,
  reverting,
  revertNote,
  revertWarn,
  onRevert,
  onShowChanges,
  fetchRunChanges,
}: {
  changes: TurnFileChange[]
  cancelled?: boolean
  failed?: boolean
  reverting?: boolean
  revertNote?: string
  revertWarn?: boolean
  onRevert?: () => void
  onShowChanges?: () => void
  // Review-projection fetch (one call per run; the controller caches and
  // invalidates it after a revert). Absent on the share page: rows stay
  // static (no chevron, no diff).
  fetchRunChanges?: () => Promise<RunChangeStat[]>
}) {
  // The card opens with its file LIST expanded by default; per-file diffs
  // start collapsed. Multiple diffs may be open at once (prototype
  // behavior); re-collapsing the card resets them all (see toggleCard).
  const [open, setOpen] = useState(true)
  const [openFiles, setOpenFiles] = useState<string[]>([])
  const [stats, setStats] = useState<RunChangeStat[] | null>(null)
  const [statsReady, setStatsReady] = useState(false)

  // Mount-time fetch (the header shows +/− totals even collapsed). The
  // promise is controller-cached, so virtualization re-mounts cost nothing.
  // An empty entry list (turns predating the ledger) or a missing endpoint
  // leaves stats null: the card keeps its static rows with no chevron.
  useEffect(() => {
    if (!fetchRunChanges) return
    let alive = true
    fetchRunChanges()
      .then((entries) => {
        if (!alive) return
        setStats(entries.length ? entries : null)
        setStatsReady(true)
      })
      .catch(() => {
        if (!alive) return
        setStatsReady(true)
      })
    return () => {
      alive = false
    }
  }, [fetchRunChanges])

  const statByPath = useMemo(() => {
    const m = new Map<string, RunChangeStat>()
    for (const s of stats || []) m.set(s.path, s)
    return m
  }, [stats])

  const n = changes.length
  const expandable = !!fetchRunChanges

  // Re-collapsing the card resets per-file diff expansion: reopening shows
  // the file list again, not the previous diff selection.
  const toggleCard = () => {
    const next = !open
    setOpen(next)
    if (!next) setOpenFiles([])
  }
  const toggleFile = (p: string) =>
    setOpenFiles((prev) => (prev.includes(p) ? prev.filter((x) => x !== p) : [...prev, p]))
  const comparable = stats ? stats.filter((s) => !s.not_comparable) : []
  const totals = comparable.length
    ? {
        added: comparable.reduce((n, s) => n + s.added, 0),
        removed: comparable.reduce((n, s) => n + s.removed, 0),
      }
    : null

  return (
    <div className="block block-turn-summary">
      <div
        role="button"
        tabIndex={0}
        className={'tsm-head' + (open ? ' open' : '')}
        onClick={toggleCard}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault()
            toggleCard()
          }
        }}
        aria-expanded={open}
      >
        <Icon name={open ? 'caret-down' : 'caret-right'} />
        <span className="tsm-title">
          Turn changes{' '}
          <b>
            {n} {n === 1 ? 'file' : 'files'}
          </b>
        </span>
        {totals && (
          <span className="tsm-total mono">
            <i>+{totals.added}</i> <b>−{totals.removed}</b>
          </span>
        )}
        {cancelled && (
          <span
            className="tsm-tag"
            title="Turn cancelled by the user; file changes are partial writes made before cancellation"
          >
            <Icon name="ban" /> Cancelled
          </span>
        )}
        {failed && !cancelled && <span className="tsm-tag warn">Failed</span>}
        {onShowChanges && (
          <button
            type="button"
            className="tsm-viewall"
            title="Open the Changes panel (git workspace view)"
            onClick={(e) => {
              e.stopPropagation()
              onShowChanges()
            }}
          >
            View all in Changes ↗
          </button>
        )}
      </div>
      {open && (
        <ul className="tsm-list">
          {changes.map((c, i) => {
            const stat = statByPath.get(c.path)
            const slash = c.path.lastIndexOf('/')
            const base = slash >= 0 ? c.path.slice(slash + 1) : c.path
            const dir = slash >= 0 ? c.path.slice(0, slash) : ''
            const fileOpen = openFiles.includes(c.path)
            const deleted = !!stat && stat.after_size === -1 && !stat.not_comparable
            const editCount = stat?.edits ?? c.edits ?? 0
            const edits = editCount > 1 ? editCount : 0
            return (
              <li key={c.path || i} className={fileOpen ? 'open' : ''}>
                <button
                  type="button"
                  className="tsm-row"
                  disabled={!expandable}
                  aria-expanded={fileOpen}
                  title={
                    expandable
                      ? `${c.path} — click to expand the diff against the current workspace`
                      : c.path
                  }
                  onClick={() => toggleFile(c.path)}
                >
                  <span
                    className={'tsm-badge ' + (c.created ? 'added' : 'modified')}
                    title={c.created ? 'Created this turn' : 'Modified this turn'}
                  >
                    {c.created ? 'A' : 'M'}
                  </span>
                  <span className="tsm-names">
                    <code className="tsm-base mono">{base}</code>
                    {dir && <span className="tsm-dir mono">{dir}</span>}
                  </span>
                  <span className="tsm-right mono">
                    {deleted && <span className="tsm-deleted">Deleted</span>}
                    {edits > 0 && <span className="tsm-edits">{edits} edits</span>}
                    {stat && !stat.not_comparable && (stat.added > 0 || stat.removed > 0) && (
                      <span className="tsm-stat">
                        {stat.added > 0 && <i>+{stat.added}</i>}
                        {stat.removed > 0 && <b>−{stat.removed}</b>}
                      </span>
                    )}
                    {expandable && (
                      <span className="tsm-chev">
                        <Icon name={fileOpen ? 'caret-down' : 'caret-right'} />
                      </span>
                    )}
                  </span>
                </button>
                {fileOpen && (
                  <div className="tsm-file-diff">
                    {!statsReady ? (
                      <div className="tsm-diff-note">Loading…</div>
                    ) : stat?.not_comparable ? (
                      <div className="tsm-diff-note">{stat.not_comparable}</div>
                    ) : stat?.diff ? (
                      <>
                        {stat.diff_truncated && (
                          <div className="tsm-diff-note">
                            Diff truncated; stats cover only the head of the file
                          </div>
                        )}
                        <InlineDiff path={c.path} diffText={stat.diff} />
                      </>
                    ) : stat ? (
                      <div className="tsm-diff-note">
                        Current content matches the pre-turn state (the changes may have been
                        reverted by later operations)
                      </div>
                    ) : (
                      <div className="tsm-diff-note">
                        No ledger record for this file in this turn; diff unavailable
                      </div>
                    )}
                  </div>
                )}
              </li>
            )
          })}
        </ul>
      )}
      {revertNote ? (
        <div className={'tsm-note' + (revertWarn ? ' warn' : '')}>
          <Icon name={revertWarn ? 'triangle-exclamation' : 'check'} /> {revertNote}
        </div>
      ) : (
        <div className="tsm-foot">
          {onRevert && (
            <button
              type="button"
              className="tsm-btn danger"
              disabled={reverting}
              title="Restore files written this turn to their pre-turn contents (external changes made after the turn are confirmed one by one)"
              onClick={onRevert}
            >
              <Icon name="rotate-left" /> {reverting ? 'Reverting…' : 'Revert this turn'}
            </button>
          )}
          <span className="tsm-footnote">
            Only loom write-tool edits are counted; files written inside run_cmd (e.g. sed) are not.{' '}
            {fetchRunChanges
              ? 'Click a file row to expand its workspace diff.'
              : 'Inline diffs are unavailable in shared views.'}
          </span>
        </div>
      )}
    </div>
  )
})

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
  if (p.trigger) details.push('Trigger: ' + p.trigger)
  if (p.masked_outputs) {
    const bytes = p.masked_bytes ? ` (${fmtBytes(p.masked_bytes)})` : ''
    details.push(`Trimmed ${p.masked_outputs} output${p.masked_outputs === 1 ? '' : 's'}${bytes}`)
  }
  if (p.archived_messages)
    details.push(`Archived ${p.archived_messages} message${p.archived_messages === 1 ? '' : 's'}`)
  if (p.summarized) details.push('Summary handoff')
  return (
    <div className="notice compact">
      <div className="compact-head">
        <Icon name="bolt" />
        {` Context compacted · ${before} → ${after}`}
      </div>
      {details.length > 0 && <div className="compact-detail">{details.join(' · ')}</div>}
    </div>
  )
})
