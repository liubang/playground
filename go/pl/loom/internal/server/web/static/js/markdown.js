// markdown.js — marked + highlight.js + DOMPurify 管线（docs/WEB_DESIGN.md §8）。
// 全 SPA 唯一 innerHTML 入口：marked/hljs 输出必须经 DOMPurify 白名单过滤；
// diff.js 的行内语法高亮复用本模块的 sanitizeHtml，不私开 innerHTML 口子。

import { marked } from '../vendor/marked.esm.js'
import DOMPurify from '../vendor/purify.es.mjs'
import hljs from '../vendor/highlight.es.min.js'

marked.setOptions({ gfm: true, breaks: false })

// 代码块语法高亮：lang 已知时走 hljs（输出自带转义），否则纯转义原文。
// 最终 HTML 仍会过 DOMPurify，双保险。
marked.use({
  renderer: {
    code({ text, lang }) {
      const language = (lang || '').trim().split(/\s+/)[0]
      if (language && hljs.getLanguage(language)) {
        try {
          const html = hljs.highlight(text, { language }).value
          return `<pre><code class="hljs language-${language}">${html}</code></pre>`
        } catch {
          // hljs 对个别残缺片段可能抛错；落回纯文本
        }
      }
      return `<pre><code>${escapeHtml(text)}</code></pre>`
    },
  },
})

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

// 外链新窗口打开且不带 opener；DOMPurify 默认不过滤 target。
DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  if (node.tagName === 'A') {
    node.setAttribute('target', '_blank')
    node.setAttribute('rel', 'noopener noreferrer')
  }
})

const PURIFY_OPTS = {
  USE_PROFILES: { html: true },
  FORBID_TAGS: ['style', 'form', 'input', 'button', 'select', 'textarea'],
  FORBID_ATTR: ['style', 'srcset'],
}

// sanitizeHtml sanitizes an HTML fragment through the shared whitelist.
// 供 diff.js 的 hljs 行内高亮复用（hljs 输出只有 span[class]）。
export function sanitizeHtml(html) {
  return DOMPurify.sanitize(html, PURIFY_OPTS)
}

// highlightToHtml highlights one text fragment with an explicit language and
// returns sanitized HTML; callers fall back to textContent when it returns "".
export function highlightToHtml(text, language) {
  if (!text || !language || !hljs.getLanguage(language)) return ''
  try {
    return sanitizeHtml(hljs.highlight(text, { language }).value)
  } catch {
    return ''
  }
}

// renderMarkdownInto renders markdown text into el, sanitized.
export function renderMarkdownInto(el, text) {
  const html = marked.parse(text || '')
  el.innerHTML = DOMPurify.sanitize(html, PURIFY_OPTS)
}
