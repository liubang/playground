// MarkdownView.tsx — markdown rendering component.
// The sole dangerouslySetInnerHTML entry point in the app: HTML comes from lib/markdown.ts's
// marked → DOMPurify whitelist pipeline (same constraints as the old version). memo compares by text,
// so unchanged blocks skip re-rendering.

import { memo, useMemo } from 'react'
import { renderMarkdown } from '../../lib/markdown'

export const MarkdownView = memo(function MarkdownView({
  text,
  className = 'md',
}: {
  text: string
  className?: string
}) {
  const html = useMemo(() => renderMarkdown(text), [text])
  return <div className={className} dangerouslySetInnerHTML={{ __html: html }} />
})
