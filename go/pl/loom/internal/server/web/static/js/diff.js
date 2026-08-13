// diff.js — unified diff 解析与渲染（docs/WEB_DESIGN.md §4.5）。
// 行内代码按文件扩展名做语法高亮：hljs 输出经 markdown.js 的 sanitizeHtml
// 白名单过滤后才进 innerHTML；未知语言/高亮失败一律回退 textContent。

import { highlightToHtml } from './markdown.js'

// --- 历史重建：从 tool_call 参数本地重算 diff（端口自 render/diff.go） ---
// diff 只存在于实时 tool.prepared/approval 事件载荷，不落盘；snapshot 重建
// 时用同一套算法从 edit/write 参数恢复（write=纯新增免 LCS；edit 双侧 LCS，
// 输入上限 400 行/侧与 Go 版一致）。不截断输出（ToolDiffUnbounded 语义）。

const DIFF_MAX_INPUT_LINES = 400

function splitDiffLines(text) {
  if (!text) return []
  return text.replace(/\n$/, '').split('\n')
}

function capLines(lines, max) {
  return lines.length > max ? lines.slice(0, max) : lines
}

// lcsDiff 计算行级 diff（LCS 动态规划，与 Go 版同算法同回朔方向）。
function lcsDiff(oldLines, newLines) {
  const n = oldLines.length,
    m = newLines.length
  const dp = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1))
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      dp[i][j] =
        oldLines[i] === newLines[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1])
    }
  }
  const ops = []
  let i = 0,
    j = 0
  while (i < n && j < m) {
    if (oldLines[i] === newLines[j]) {
      ops.push({ kind: ' ', line: oldLines[i] })
      i++
      j++
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      ops.push({ kind: '-', line: oldLines[i] })
      i++
    } else {
      ops.push({ kind: '+', line: newLines[j] })
      j++
    }
  }
  while (i < n) ops.push({ kind: '-', line: oldLines[i++] })
  while (j < m) ops.push({ kind: '+', line: newLines[j++] })
  return ops
}

// diffTexts 渲染紧凑行 diff：变更行保留上下各 1 行上下文，未变更段折叠为 "…"。
function diffTexts(oldText, newText) {
  if (oldText === newText) return ''
  let ops
  if (oldText === '' || newText === '') {
    ops = [
      ...splitDiffLines(oldText).map((line) => ({ kind: '-', line })),
      ...splitDiffLines(newText).map((line) => ({ kind: '+', line })),
    ]
  } else {
    ops = lcsDiff(
      capLines(splitDiffLines(oldText), DIFF_MAX_INPUT_LINES),
      capLines(splitDiffLines(newText), DIFF_MAX_INPUT_LINES),
    )
  }
  const show = new Array(ops.length).fill(false)
  ops.forEach((op, i) => {
    if (op.kind === ' ') return
    show[i] = true
    if (i > 0) show[i - 1] = true
    if (i + 1 < ops.length) show[i + 1] = true
  })
  const out = []
  let skipped = false
  ops.forEach((op, i) => {
    if (!show[i]) {
      skipped = true
      return
    }
    if (skipped && out.length > 0) out.push('...')
    skipped = false
    out.push((op.kind === ' ' ? '  ' : op.kind + ' ') + op.line)
  })
  return out.join('\n')
}

// diffForToolCall 从 edit/write 的调用参数重建展示用 diff（含 +++ 文件头，
// 供文件名展示与语法高亮语言探测）；其他工具或无意义输入返回 ""。
export function diffForToolCall(toolName, args) {
  if (!args || typeof args !== 'object') return ''
  let text = ''
  if (toolName === 'edit' && typeof args.new_string === 'string') {
    text = diffTexts(typeof args.old_string === 'string' ? args.old_string : '', args.new_string)
  } else if (toolName === 'write' && typeof args.content === 'string') {
    text = diffTexts('', args.content)
  }
  if (!text) return ''
  const path = typeof args.path === 'string' && args.path ? `+++ b/${args.path}\n` : ''
  return path + text
}

function el(tag, cls, text) {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text != null) e.textContent = text
  return e
}

// 扩展名 → highlight.js 语言（仅列 common 包内置语言；未命中即不高亮）。
const EXT_TO_LANG = {
  go: 'go',
  rs: 'rust',
  py: 'python',
  rb: 'ruby',
  java: 'java',
  kt: 'kotlin',
  js: 'javascript',
  mjs: 'javascript',
  jsx: 'javascript',
  ts: 'typescript',
  tsx: 'typescript',
  c: 'c',
  h: 'c',
  cc: 'cpp',
  cpp: 'cpp',
  cxx: 'cpp',
  hpp: 'cpp',
  cs: 'csharp',
  sh: 'bash',
  bash: 'bash',
  zsh: 'bash',
  lua: 'lua',
  pl: 'perl',
  php: 'php',
  r: 'r',
  swift: 'swift',
  json: 'json',
  yml: 'yaml',
  yaml: 'yaml',
  toml: 'ini',
  ini: 'ini',
  sql: 'sql',
  graphql: 'graphql',
  md: 'markdown',
  html: 'xml',
  htm: 'xml',
  xml: 'xml',
  css: 'css',
  scss: 'scss',
  less: 'less',
  mk: 'makefile',
  makefile: 'makefile',
  diff: 'diff',
}

function langFromPath(p) {
  const m = /\.([A-Za-z0-9]+)$/.exec(p || '')
  return m ? EXT_TO_LANG[m[1].toLowerCase()] || '' : ''
}

// renderDiff parses a unified diff and returns a .diff element.
// Lines not matching the grammar are rendered as context (loss-tolerant).
// 超过 DIFF_COLLAPSE_LINES 的 diff 折叠为 details（内容完整保留，展开可见），
// 短 diff 直接平铺。
const DIFF_COLLAPSE_LINES = 30

export function renderDiff(diffText) {
  const body = el('div', 'd-body mono')
  let file = ''
  let lang = ''
  let sawContent = false
  let lineCount = 0,
    adds = 0,
    dels = 0

  for (const raw of (diffText || '').split('\n')) {
    if (raw.startsWith('+++ ')) {
      file = raw.slice(4).replace(/^b\//, '')
      lang = langFromPath(file)
      continue
    }
    if (raw.startsWith('--- ') || raw.startsWith('diff ') || raw.startsWith('index ')) {
      continue
    }
    if (raw.startsWith('@@')) {
      body.appendChild(el('div', 'd-line d-hunk', raw))
      sawContent = true
      lineCount++
      continue
    }
    let cls = 'd-ctx',
      sign = ' ',
      text = raw
    if (raw.startsWith('+')) {
      cls = 'd-add'
      sign = '+'
      text = raw.slice(1)
      adds++
    } else if (raw.startsWith('-')) {
      cls = 'd-del'
      sign = '−'
      text = raw.slice(1)
      dels++
    } else if (raw.startsWith(' ')) {
      text = raw.slice(1)
    } else if (raw === '' && !sawContent) {
      continue
    }
    const line = el('div', 'd-line ' + cls)
    line.appendChild(el('span', 'd-sign', sign))
    const code = el('code')
    const html = highlightToHtml(text, lang)
    if (html)
      code.innerHTML = html // sanitizeHtml 已过滤（markdown.js）
    else code.textContent = text
    line.appendChild(code)
    body.appendChild(line)
    sawContent = true
    lineCount++
  }

  if (lineCount > DIFF_COLLAPSE_LINES) {
    const d = document.createElement('details')
    d.className = 'diff disclosure'
    const summary = el('summary', 'd-head mono')
    summary.appendChild(el('span', '', file || 'diff'))
    summary.appendChild(el('span', 'd-stat', `${lineCount} lines · +${adds} −${dels}`))
    d.appendChild(summary)
    d.appendChild(body)
    return d
  }

  const root = el('div', 'diff')
  const head = el('div', 'd-head mono', file || 'diff')
  root.appendChild(head)
  root.appendChild(body)
  return root
}
