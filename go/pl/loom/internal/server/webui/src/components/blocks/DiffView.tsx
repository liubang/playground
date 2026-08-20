// DiffView.tsx — unified diff 渲染（解析见 lib/diff.ts）。
// 行内代码按文件扩展名做语法高亮：hljs 输出经 markdown.ts 的 sanitizeHtml
// 白名单过滤后才进 DOM；未知语言/高亮失败一律回退纯文本。
// 超过 DIFF_COLLAPSE_LINES 折叠为 details，短 diff 平铺（与旧版一致）。

import { memo, useMemo } from 'react'
import { DIFF_COLLAPSE_LINES, parseDiff, type DiffLine } from '../../lib/diff'
import { highlightToHtml } from '../../lib/markdown'

function DiffLineView({ line, lang }: { line: DiffLine; lang: string }) {
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
        // sanitizeHtml 已过滤（markdown.ts）
        <code dangerouslySetInnerHTML={{ __html: html }} />
      ) : (
        <code>{line.text}</code>
      )}
    </div>
  )
}

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
