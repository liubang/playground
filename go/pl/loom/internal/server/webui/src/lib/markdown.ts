// markdown.ts — marked + highlight.js + DOMPurify 管线（docs/WEB_DESIGN.md §8）。
// 全 SPA 唯一危险 HTML 入口：marked/hljs 输出必须经 DOMPurify 白名单过滤；
// diff 的行内语法高亮复用本模块的 sanitizeHtml，不私开 innerHTML 口子。
// 逻辑与旧 static/js/markdown.js 一一对应。

import { marked } from 'marked'
import DOMPurify from 'dompurify'
// hljs 按需注册：只打包 diff 高亮白名单（lib/diff.ts EXT_TO_LANG）覆盖的
// 语言——highlight.js/lib/common 全量约 40 种语言是共享 chunk 的最大单项
// 之一，按子集注册可再减约四分之一体积。
import hljs from 'highlight.js/lib/core'
import type { LanguageFn } from 'highlight.js'
import bash from 'highlight.js/lib/languages/bash'
import c from 'highlight.js/lib/languages/c'
import cpp from 'highlight.js/lib/languages/cpp'
import csharp from 'highlight.js/lib/languages/csharp'
import css from 'highlight.js/lib/languages/css'
import diff from 'highlight.js/lib/languages/diff'
import go from 'highlight.js/lib/languages/go'
import graphql from 'highlight.js/lib/languages/graphql'
import ini from 'highlight.js/lib/languages/ini'
import java from 'highlight.js/lib/languages/java'
import javascript from 'highlight.js/lib/languages/javascript'
import json from 'highlight.js/lib/languages/json'
import kotlin from 'highlight.js/lib/languages/kotlin'
import less from 'highlight.js/lib/languages/less'
import lua from 'highlight.js/lib/languages/lua'
import makefile from 'highlight.js/lib/languages/makefile'
import markdownLang from 'highlight.js/lib/languages/markdown'
import perl from 'highlight.js/lib/languages/perl'
import php from 'highlight.js/lib/languages/php'
import python from 'highlight.js/lib/languages/python'
import r from 'highlight.js/lib/languages/r'
import ruby from 'highlight.js/lib/languages/ruby'
import rust from 'highlight.js/lib/languages/rust'
import scss from 'highlight.js/lib/languages/scss'
import sql from 'highlight.js/lib/languages/sql'
import swift from 'highlight.js/lib/languages/swift'
import typescript from 'highlight.js/lib/languages/typescript'
import xml from 'highlight.js/lib/languages/xml'
import yaml from 'highlight.js/lib/languages/yaml'

const HLJS_LANGS: [string, LanguageFn][] = [
  ['bash', bash],
  ['c', c],
  ['cpp', cpp],
  ['csharp', csharp],
  ['css', css],
  ['diff', diff],
  ['go', go],
  ['graphql', graphql],
  ['ini', ini],
  ['java', java],
  ['javascript', javascript],
  ['json', json],
  ['kotlin', kotlin],
  ['less', less],
  ['lua', lua],
  ['makefile', makefile],
  ['markdown', markdownLang],
  ['perl', perl],
  ['php', php],
  ['python', python],
  ['r', r],
  ['ruby', ruby],
  ['rust', rust],
  ['scss', scss],
  ['sql', sql],
  ['swift', swift],
  ['typescript', typescript],
  ['xml', xml],
  ['yaml', yaml],
]
for (const [name, fn] of HLJS_LANGS) hljs.registerLanguage(name, fn)

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

// markdownStableBoundary returns the byte offset of the longest prefix of text
// that consists of fully closed markdown blocks. Everything after this offset
// is the "live tail" that may still be growing (an open paragraph / list /
// fenced code block). Streaming rendering caches the prefix and only re-renders
// the tail — the dominant cost in a long stream is re-sanitizing and re-parsing
// the whole buffer on every tick, which this avoids.
//
// Implemented as a lightweight line scan (not marked.lexer): lexer is O(n) and
// ~200x slower than this scan, and it would dominate the per-tick cost it is
// meant to eliminate. The scan is deliberately conservative — a boundary is
// only advanced past text that is provably closed (a blank line outside a code
// fence, or a closed fence). Underestimating the stable prefix only costs a
// slightly longer tail re-render; overestimating would drop content, so the
// conservative direction is the safe one. The final turn-end render is still a
// full parse, which is the correctness backstop.
export function markdownStableBoundary(text: string): number {
  if (!text) return 0
  let inFence = false
  let lastBoundary = 0
  let i = 0
  const n = text.length
  while (i <= n) {
    const nl = text.indexOf('\n', i)
    const lineEnd = nl < 0 ? n : nl
    const line = text.slice(i, lineEnd)
    const trimmed = line.trim()
    // Fenced code block: ``` or ~~~ (marked allows up to 3 spaces of indent;
    // trim() covers the common cases).
    const isFence = trimmed.startsWith('```') || trimmed.startsWith('~~~')
    if (isFence) {
      inFence = !inFence
      if (!inFence) {
        // The fence closed: the whole code block is stable.
        lastBoundary = nl < 0 ? n : nl + 1
      }
    } else if (!inFence && trimmed === '') {
      // A blank line outside a code fence is a block boundary.
      lastBoundary = nl < 0 ? n : nl + 1
    }
    if (nl < 0) break
    i = nl + 1
  }
  return lastBoundary
}
