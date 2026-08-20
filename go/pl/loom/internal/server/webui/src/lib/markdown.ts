// markdown.ts — marked + highlight.js + DOMPurify 管线（docs/WEB_DESIGN.md §8）。
// 全 SPA 唯一危险 HTML 入口：marked/hljs 输出必须经 DOMPurify 白名单过滤；
// diff 的行内语法高亮复用本模块的 sanitizeHtml，不私开 innerHTML 口子。
// 逻辑与旧 static/js/markdown.js 一一对应。

import { marked } from 'marked'
import DOMPurify from 'dompurify'
import hljs from 'highlight.js/lib/common'

marked.setOptions({ gfm: true, breaks: false })

function escapeHtml(s: string): string {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

// 代码块语法高亮：lang 已知时走 hljs（输出自带转义），否则纯转义原文。
// 最终 HTML 仍会过 DOMPurify，双保险。
marked.use({
  tokenizer: {
    // marked v15+ 的 GFM del 规则接受单个 ~（del:/^(~~?).../），会把
    // "100ms~1s"、"31~33°C" 这类区间写法误判成删除线。这里拦截：孤立单 ~
    // 一律按普通文本消费（返回 token 即阻止内置 del 再匹配），~~ 交回内置
    // strikethrough 处理（false = 回落内置）。与 TUI 的 escapeLoneTildes
    // （internal/ui/markdown.go）同语义。
    del(src: string) {
      // marked 的 Del token 类型声明要求 tokens 字段；运行时 text token 无需
      // （与旧版行为一致，官方 issue 同款绕过）
      if (src[0] === '~' && src[1] !== '~')
        return { type: 'text', raw: '~', text: '~' } as unknown as never
      return false
    },
  },
  renderer: {
    code({ text, lang }: { text: string; lang?: string }) {
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

// 外链新窗口打开且不带 opener；DOMPurify 默认不过滤 target。
DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  if ((node as Element).tagName === 'A') {
    ;(node as Element).setAttribute('target', '_blank')
    ;(node as Element).setAttribute('rel', 'noopener noreferrer')
  }
})

const PURIFY_OPTS = {
  USE_PROFILES: { html: true },
  FORBID_TAGS: ['style', 'form', 'input', 'button', 'select', 'textarea'],
  FORBID_ATTR: ['style', 'srcset'],
}

// sanitizeHtml sanitizes an HTML fragment through the shared whitelist.
// 供 diff 的 hljs 行内高亮复用（hljs 输出只有 span[class]）。
export function sanitizeHtml(html: string): string {
  return DOMPurify.sanitize(html, PURIFY_OPTS)
}

// highlightToHtml highlights one text fragment with an explicit language and
// returns sanitized HTML; callers fall back to textContent when it returns "".
export function highlightToHtml(text: string, language: string): string {
  if (!text || !language || !hljs.getLanguage(language)) return ''
  try {
    return sanitizeHtml(hljs.highlight(text, { language }).value)
  } catch {
    return ''
  }
}

// renderMarkdown renders markdown text to sanitized HTML。
// React 侧经 dangerouslySetInnerHTML 挂载（全应用唯一入口，与旧版同约束）。
export function renderMarkdown(text: string): string {
  const html = marked.parse(text || '', { async: false })
  return DOMPurify.sanitize(html, PURIFY_OPTS)
}
