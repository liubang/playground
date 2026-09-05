// DiffView.tsx — unified diff rendering (parsing in lib/diff.ts).
// Inline code is syntax-highlighted by file extension: hljs output enters the DOM only after
// whitelist filtering by markdown.ts's sanitizeHtml; unknown languages/highlight failures always fall back to plain text.
// Diffs longer than DIFF_COLLAPSE_LINES collapse into details; short diffs render flat (same as the old version).

import { memo, useMemo } from 'react'
import { DIFF_COLLAPSE_LINES, parseDiff, type DiffLine } from '../../lib/diff'
import { highlightToHtml } from '../../lib/markdown'

// Memoized per line: large diffs render hundreds of lines inside the transcript — without
// memo every streaming tick re-rendered (and re-reconciled innerHTML for) every line.
// Line objects stay reference-stable because parseDiff is memoized on diffText above.
const DiffLineView = memo(function DiffLineView({ line, lang }: { line: DiffLine; lang: string }) {
  const html = useMemo(() => highlightToHtml(line.text, lang), [line.text, lang])
  const cls =
    line.kind === 'hunk'
      ? 'd-line d-hunk'
      : line.kind === 'add'
        ? 'd-line d-add'
        : line.kind === 'del'
          ? 'd-line d-del'
          : 'd-line d-ctx'
  if (line.kind === 'hunk') return <div className={cls}>{line.text}</div>
  return (
    <div className={cls}>
      <span className="d-sign">{line.sign}</span>
      {html ? (
        // already filtered by sanitizeHtml (markdown.ts)
        <code dangerouslySetInnerHTML={{ __html: html }} />
      ) : (
        <code>{line.text}</code>
      )}
    </div>
  )
})

export const DiffView = memo(function DiffView({ diffText }: { diffText: string }) {
  const diff = useMemo(() => parseDiff(diffText), [diffText])
  const body = (
    <div className="d-body mono">
      {diff.lines.map((l, i) => (
        <DiffLineView key={i} line={l} lang={diff.lang} />
      ))}
    </div>
  )

  if (diff.lines.length > DIFF_COLLAPSE_LINES) {
    return (
      <details className="diff disclosure">
        <summary className="d-head mono">
          <span>{diff.file || 'diff'}</span>
          <span className="d-stat">
            {diff.lines.length} lines · +{diff.adds} −{diff.dels}
          </span>
        </summary>
        {body}
      </details>
    )
  }

  return (
    <div className="diff">
      <div className="d-head mono">{diff.file || 'diff'}</div>
      {body}
    </div>
  )
})
