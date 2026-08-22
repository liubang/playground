// diff.ts — unified diff parsing (rendering: see components/blocks/DiffView.tsx).
// Inline code is syntax-highlighted by file extension: hljs output enters the
// DOM only after passing markdown.ts's sanitizeHtml whitelist; unknown languages
// or highlight failures fall back to plain text.
// Parsing logic is one-to-one with the old static/js/diff.js.

// --- History rebuild: recompute diffs locally from tool_call args (ported from
// render/diff.go) ---
// Diffs exist only in the live tool.prepared/approval event payloads and are
// never persisted; snapshot rebuilds recover them from edit/write args using
// the same algorithm (write = pure addition, no LCS; edit = two-sided LCS with
// a 400-line-per-side input cap, same as the Go version). Output is not
// truncated (ToolDiffUnbounded semantics).

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

// lcsDiff computes a line-level diff (LCS dynamic programming; same algorithm
// and backtrack direction as the Go version).
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

// diffTexts renders a compact line diff: changed lines keep 1 line of context
// above and below; unchanged runs collapse into "…".
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

// diffForToolCall rebuilds a display diff from edit/write call args (with a +++
// file header for filename display and highlight-language detection); other
// tools or meaningless input return "".
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

// Extension → highlight.js language (only languages bundled in the common
// package; no match means no highlighting).
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

export function langFromPath(p: string): string {
  const m = /\.([A-Za-z0-9]+)$/.exec(p || '')
  return m ? EXT_TO_LANG[m[1].toLowerCase()] || '' : ''
}

// --- Parsing (render data model; lines that don't match are treated as context —
// lenient) ---

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

// Diffs longer than DIFF_COLLAPSE_LINES collapse into a details element (content
// fully preserved, visible when expanded).
export const DIFF_COLLAPSE_LINES = 30
