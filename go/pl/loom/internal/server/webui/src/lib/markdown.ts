// markdown.ts — marked + highlight.js + DOMPurify pipeline (docs/WEB_DESIGN.md §8).
// The SPA's only dangerous-HTML entry point: marked/hljs output must pass
// DOMPurify's whitelist; the diff's inline syntax highlighting reuses this
// module's sanitizeHtml — no private innerHTML openings. Logic mirrors the legacy static/js/markdown.js one-to-one.

import { marked } from 'marked'
import DOMPurify from 'dompurify'
// hljs registered on demand: only bundle the languages covered by the diff
// highlight whitelist (lib/diff.ts EXT_TO_LANG) — highlight.js/lib/common's
// full ~40 languages are among the shared chunk's largest single items; registering a subset shaves off roughly another quarter of the size.
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

// Code block syntax highlighting: hljs when lang is known (its output escapes
// itself), otherwise plain-escape the raw text. The final HTML still passes DOMPurify — belt and braces.
marked.use({
  tokenizer: {
    // marked v15+'s GFM del rule accepts a single ~ (del:/^(~~?).../), which
    // misreads range notations like "100ms~1s", "31~33°C" as strikethrough.
    // Intercept here: a lone single ~ is always consumed as plain text (returning
    // a token prevents the built-in del from matching again); ~~ goes back to the
    // built-in strikethrough (false = fall back). Same semantics as the TUI's escapeLoneTildes (internal/ui/markdown.go).
    del(src: string) {
      // marked's Del token type declaration requires a tokens field; a runtime
      // text token doesn't need one (same behavior as the legacy version, same workaround as the official issue)
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
          // hljs may throw on some incomplete fragments; fall back to plain text
        }
      }
      return `<pre><code>${escapeHtml(text)}</code></pre>`
    },
  },
})

// Open external links in a new window without an opener; DOMPurify doesn't filter target by default.
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
// Reused by the diff's hljs inline highlighting (hljs output is only span[class]).
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

// renderMarkdown renders markdown text to sanitized HTML.
// The React side mounts it via dangerouslySetInnerHTML (the app's only entry point, same constraint as the legacy version).
export function renderMarkdown(text: string): string {
  const html = marked.parse(text || '', { async: false })
  return DOMPurify.sanitize(html, PURIFY_OPTS)
}

// renderStreamTail renders the LIVE, still-growing tail of a streaming message
// (everything after markdownStableBoundary). The hot case is a model writing a
// long code block: hljs re-highlights the entire growing fence body on every
// 60ms tick — O(n²) total work per code block. Instead, an unclosed fence's
// opening line is split off and its body is emitted as plain escaped text
// (escaped, then DOMPurify-sanitized like everything else); highlighting
// arrives for free once the fence closes and the stable boundary passes it.
// The final turn-end render is a full renderMarkdown, which is the visual
// ground truth — this is a transient streaming optimization only.
export function renderStreamTail(tail: string): string {
  // Scan fence lines with the same tolerance as markdownStableBoundary: the first
  // fence line in the tail is an opener; a second one closes it (or a later fence's
  // own opener — either way, nothing unclosed remains for us to special-case).
  let idx = 0
  let openFenceAt = -1
  for (;;) {
    const nl = tail.indexOf('\n', idx)
    const end = nl < 0 ? tail.length : nl
    const trimmed = tail.slice(idx, end).trim()
    if (trimmed.startsWith('```') || trimmed.startsWith('~~~')) {
      if (openFenceAt < 0) openFenceAt = idx
      else return renderMarkdown(tail) // fence closed again inside the tail: normal path
    }
    if (nl < 0) break
    idx = nl + 1
  }
  if (openFenceAt < 0) return renderMarkdown(tail)
  const fenceEnd = tail.indexOf('\n', openFenceAt)
  const head = tail.slice(0, openFenceAt)
  const lang = tail
    .slice(openFenceAt, fenceEnd < 0 ? tail.length : fenceEnd)
    .replaceAll(/[`~]/g, '')
    .trim()
  const code = fenceEnd < 0 ? '' : tail.slice(fenceEnd + 1)
  const cls = lang && hljs.getLanguage(lang) ? ` class="hljs language-${escapeHtml(lang)}"` : ''
  const codeHtml = `<pre><code${cls}>${escapeHtml(code)}</code></pre>`
  // Concatenating the two fragments before one sanitize pass keeps the safety
  // contract identical to renderMarkdown (single whitelist boundary).
  return DOMPurify.sanitize(renderMarkdown(head) + codeHtml, PURIFY_OPTS)
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
