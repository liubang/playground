import MarkdownIt from 'markdown-it'
import DOMPurify from 'dompurify'

const md = new MarkdownIt({
  html: false, // 不允许原始 HTML，配合 DOMPurify 双保险
  linkify: true,
  breaks: true,
})

md.renderer.rules.link_open = (tokens, idx, options, env, self) => {
  const token = tokens[idx]
  token.attrSet('target', '_blank')
  token.attrSet('rel', 'noopener noreferrer')
  return self.renderToken(tokens, idx, options)
}

export function renderMarkdown(text) {
  if (!text) return ''
  return DOMPurify.sanitize(md.render(String(text)))
}
