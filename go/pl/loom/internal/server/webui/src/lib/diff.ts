// diff.ts — unified diff 解析（渲染见 components/blocks/DiffView.tsx）。
// 行内代码按文件扩展名做语法高亮：hljs 输出经 markdown.ts 的 sanitizeHtml
// 白名单过滤后才进 DOM；未知语言/高亮失败一律回退纯文本。
// 解析逻辑与旧 static/js/diff.js 一一对应。

// --- 历史重建：从 tool_call 参数本地重算 diff（端口自 render/diff.go） ---
// diff 只存在于实时 tool.prepared/approval 事件载荷，不落盘；snapshot 重建
// 时用同一套算法从 edit/write 参数恢复（write=纯新增免 LCS；edit 双侧 LCS，
// 输入上限 400 行/侧与 Go 版一致）。不截断输出（ToolDiffUnbounded 语义）。

const DIFF_MAX_INPUT_LINES = 400

function splitDiffLines(text: string): string[] {
  if (!text) return []
  return text.replace(/\n$/, '').split('\n')
}

function capLines(lines: string[], max: number): string[] {
  return lines.length > max ? lines.slice(0, max) : lines
}

interface DiffOp {
  kind: ' ' | '-' | '+'
  line: string
}

// lcsDiff 计算行级 diff（LCS 动态规划，与 Go 版同算法同回朔方向）。
function lcsDiff(oldLines: string[], newLines: string[]): DiffOp[] {
  const n = oldLines.length
  const m = newLines.length
  const dp: Uint32Array[] = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1))
  for (let i = n - 1; i >= 0; i--) {
    for (let j = m - 1; j >= 0; j--) {
      dp[i][j] =
        oldLines[i] === newLines[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1])
    }
  }
  const ops: DiffOp[] = []
  let i = 0
  let j = 0
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
function diffTexts(oldText: string, newText: string): string {
  if (oldText === newText) return ''
  let ops: DiffOp[]
  if (oldText === '' || newText === '') {
    ops = [
      ...splitDiffLines(oldText).map((line): DiffOp => ({ kind: '-', line })),
      ...splitDiffLines(newText).map((line): DiffOp => ({ kind: '+', line })),
    ]
  } else {
    ops = lcsDiff(
      capLines(splitDiffLines(oldText), DIFF_MAX_INPUT_LINES),
      capLines(splitDiffLines(newText), DIFF_MAX_INPUT_LINES),
    )
  }
  const show = new Array<boolean>(ops.length).fill(false)
  ops.forEach((op, i) => {
    if (op.kind === ' ') return
    show[i] = true
    if (i > 0) show[i - 1] = true
    if (i + 1 < ops.length) show[i + 1] = true
  })
  const out: string[] = []
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
export function diffForToolCall(toolName: string, args: unknown): string {
  if (!args || typeof args !== 'object') return ''
  const a = args as Record<string, unknown>
  let text = ''
  if (toolName === 'edit' && typeof a.new_string === 'string') {
    text = diffTexts(typeof a.old_string === 'string' ? a.old_string : '', a.new_string)
  } else if (toolName === 'write' && typeof a.content === 'string') {
    text = diffTexts('', a.content)
  }
  if (!text) return ''
  const path = typeof a.path === 'string' && a.path ? `+++ b/${a.path}\n` : ''
  return path + text
}

// 扩展名 → highlight.js 语言（仅列 common 包内置语言；未命中即不高亮）。
const EXT_TO_LANG: Record<string, string> = {
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

function langFromPath(p: string): string {
  const m = /\.([A-Za-z0-9]+)$/.exec(p || '')
  return m ? EXT_TO_LANG[m[1].toLowerCase()] || '' : ''
}

// --- 解析（渲染数据模型；Lines 不匹配的按 context 处理，容错） ---

export type DiffLineKind = 'hunk' | 'add' | 'del' | 'ctx'

export interface DiffLine {
  kind: DiffLineKind
  sign: string
  text: string
}

export interface ParsedDiff {
  file: string
  lang: string
  lines: DiffLine[]
  adds: number
  dels: number
}

export function parseDiff(diffText: string): ParsedDiff {
  const out: ParsedDiff = { file: '', lang: '', lines: [], adds: 0, dels: 0 }
  let sawContent = false

  for (const raw of (diffText || '').split('\n')) {
    if (raw.startsWith('+++ ')) {
      out.file = raw.slice(4).replace(/^b\//, '')
      out.lang = langFromPath(out.file)
      continue
    }
    if (raw.startsWith('--- ') || raw.startsWith('diff ') || raw.startsWith('index ')) {
      continue
    }
    if (raw.startsWith('@@')) {
      out.lines.push({ kind: 'hunk', sign: '', text: raw })
      sawContent = true
      continue
    }
    let kind: DiffLineKind = 'ctx'
    let sign = ' '
    let text = raw
    if (raw.startsWith('+')) {
      kind = 'add'
      sign = '+'
      text = raw.slice(1)
      out.adds++
    } else if (raw.startsWith('-')) {
      kind = 'del'
      sign = '−'
      text = raw.slice(1)
      out.dels++
    } else if (raw.startsWith(' ')) {
      text = raw.slice(1)
    } else if (raw === '' && !sawContent) {
      continue
    }
    out.lines.push({ kind, sign, text })
    sawContent = true
  }
  return out
}

// 超过 DIFF_COLLAPSE_LINES 的 diff 折叠为 details（内容完整保留，展开可见）。
export const DIFF_COLLAPSE_LINES = 30
