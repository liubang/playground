// MarkdownView.tsx — markdown 渲染组件。
// dangerouslySetInnerHTML 全应用唯一入口：HTML 来自 lib/markdown.ts 的
// marked → DOMPurify 白名单管线（与旧版同约束）。memo 按 text 比较，
// 未变化的块不参与重渲染。

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
