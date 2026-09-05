// ToolBlock.tsx — tool block renderer (one-to-one with the old blocks.js toolBlock).
// The live event's preview is a bounded excerpt; full content is fetched on demand
// via fetchToolOutput. The snapshot rebuild path has full_text and never needs it.

import { memo, useMemo, useState } from 'react'
import type { ToolCompletion } from '../../app/transcript'
import { fmtDuration, copyText } from '../../lib/format'
import { diffForToolCall } from '../../lib/diff'
import { Icon, type IconName } from '../../lib/icons'
import { DiffView } from './DiffView'
import { ArtifactBlock, InlineImage } from './images'

// st → [icon, label]; className uses English short codes (err/error/canceled), labels are uniformly Chinese
const TOOL_STATUS: Record<string, ['check' | 'xmark' | 'ban', string]> = {
  ok: ['check', '成功'],
  err: ['xmark', '失败'],
  canceled: ['ban', '已取消'],
}

// Tool kind → [icon, plain-language verb]: the header row shows verb + target
// (what the agent is doing at a glance); the raw toolName goes into the tooltip
// for debugging. MCP tools take the mcp__ prefix branch.
const TOOL_META: Record<string, [IconName, string]> = {
  read_file: ['eye', 'read'],
  list_dir: ['folder-open', 'list'],
  glob: ['magnifying-glass', 'glob'],
  search: ['magnifying-glass', 'search'],
  search_text: ['magnifying-glass', 'search'],
  edit: ['pen-to-square', 'edit'],
  write: ['file', 'write'],
  run_cmd: ['terminal', 'run'],
  exec_session: ['terminal', 'exec'],
  write_stdin: ['terminal', 'stdin'],
  delegate_task: ['robot', 'delegate'],
  update_plan: ['square-check', 'plan'],
  update_goal: ['star', 'goal'],
  view_image: ['image', 'view image'],
  present_image: ['image', 'image'],
  generate_image: ['image', 'generate image'],
  web_fetch: ['download', 'fetch'],
  web_search: ['magnifying-glass', 'web search'],
  kb_search: ['database', 'kb search'],
  kb_read: ['database', 'kb read'],
  read_skill: ['puzzle-piece', 'skill'],
}

function toolMeta(toolName: string): [IconName, string] {
  const meta = TOOL_META[toolName]
  if (meta) return meta
  if (toolName.startsWith('mcp__')) return ['plug', toolName.slice(5) || 'mcp']
  return ['gear', toolName || 'tool']
}

export interface ToolBlockProps {
  callId?: string // anchor: the maze's chat jump looks up the DOM row by it (data-call-id)
  toolName: string
  target?: string
  diff?: string
  // Snapshot-rebuilt block: the diff is not in the snapshot; it is recomputed from edit/write args only inside the render window
  diffArgs?: { name: string; args: unknown }
  diffSuppressed?: boolean // during approval the diff moves into the approval card
  completion?: ToolCompletion
  // fetchToolOutput: for copying the full output (the live path's preview is bounded)
  fetchToolOutput?: () => Promise<string>
}

export const ToolBlock = memo(function ToolBlock({
  callId,
  toolName,
  target,
  diff,
  diffArgs,
  diffSuppressed,
  completion,
  fetchToolOutput,
}: ToolBlockProps) {
  const [targetExpanded, setTargetExpanded] = useState(false)
  // Lazy diff: this component is only mounted inside the virtualized render window; useMemo caches by
  // diffArgs reference, so the LCS is not recomputed after scrolling away
  const diffText = useMemo(
    () =>
      diff ?? (diffArgs ? diffForToolCall(diffArgs.name, diffArgs.args) || undefined : undefined),
    [diff, diffArgs],
  )

  let statusEl
  if (!completion) {
    statusEl = <span className="tool-status running">执行中</span>
  } else {
    const st =
      completion.status === 'success' ? 'ok' : completion.status === 'error' ? 'err' : 'canceled'
    const meta = TOOL_STATUS[st]
    statusEl = (
      <span className={'tool-status ' + st}>
        {meta ? (
          <>
            <Icon name={meta[0]} /> {meta[1]}
          </>
        ) : (
          completion.status || 'done'
        )}
      </span>
    )
  }

  const getFullText = async (): Promise<string> => {
    if (completion?.full_text) return completion.full_text
    if (fetchToolOutput) return fetchToolOutput()
    return completion?.preview || '' // fall back to copying the excerpt when the server is unreachable
  }

  const [icon, verb] = toolMeta(toolName)
  return (
    <div className="block block-tool" data-call-id={callId || undefined}>
      <div className="tool-head">
        <span className="tool-kind" title={toolName || 'tool'}>
          <Icon name={icon} />
        </span>
        <span className="tool-name mono">{verb}</span>
        {target && (
          // After CSS ellipsis truncation, both the native tooltip and click-to-expand
          // (wrapped display) reveal the full content.
          <span
            className={'tool-target mono' + (targetExpanded ? ' expanded' : '')}
            title={target}
            onClick={(e) => {
              e.stopPropagation()
              setTargetExpanded((v) => !v)
            }}
          >
            {target}
          </span>
        )}
        {statusEl}
        <span className="tool-dur mono">
          {completion?.duration_ms != null ? fmtDuration(completion.duration_ms) : ''}
        </span>
      </div>
      {completion && (completion.error_message || completion.error) && (
        <div className="tool-error">{completion.error_message || completion.error}</div>
      )}
      {completion?.preview && <ToolOutput preview={completion.preview} getFullText={getFullText} />}
      {/* Render images from tool results: inline base64 first (synchronously
          available, no second authenticated request); the artifact path is used
          only when there are no inline images. An artifact is not necessarily an
          image (run_cmd's stdout artifact is text) — ArtifactBlock dispatches by
          media type. */}
      {completion &&
        ((completion.images || []).length > 0
          ? (completion.images || []).map((img, i) => (
              <InlineImage key={i} mediaType={img.media_type} data={img.data} />
            ))
          : (completion.artifacts || []).map((art) => (
              <ArtifactBlock key={art.id} artifact={art} />
            )))}
      {diffText && !diffSuppressed && <DiffView diffText={diffText} />}
    </div>
  )
})

// Tool output area: collapsed by default; expanding shows the bounded preview
// (truncation marked by a trailing "\n…"); the copy button always copies the
// full output. Memoized: tool blocks re-render on every streaming tick.
const ToolOutput = memo(function ToolOutput({
  preview,
  getFullText,
}: {
  preview: string
  getFullText: () => Promise<string>
}) {
  // '复制中…' disables the button while the full-output fetch is in flight — a double
  // click previously kicked off two fetches; '复制失败' is clickable again so a flaky
  // request can be retried.
  const [label, setLabel] = useState<'复制' | '复制中…' | '已复制' | '复制失败'>('复制')
  const truncated = preview.endsWith('\n…')
  return (
    <details className="tool-output disclosure">
      <summary>
        <span className="tool-output-label">
          {`输出 · ${preview.length} 字符${truncated ? ' · 已截断' : ''}`}
        </span>
        <button
          type="button"
          className="tool-copy"
          title="复制完整输出"
          onClick={async (e) => {
            e.preventDefault() // don't toggle the details disclosure
            e.stopPropagation()
            if (label === '复制中…') return
            setLabel('复制中…')
            try {
              const text = await getFullText()
              if (!(await copyText(text))) throw new Error('clipboard unavailable')
              setLabel('已复制')
            } catch {
              setLabel('复制失败')
            }
            setTimeout(() => setLabel('复制'), 1500)
          }}
        >
          {label === '已复制' ? (
            <>
              <Icon name="check" /> 已复制
            </>
          ) : (
            label
          )}
        </button>
      </summary>
      <div className="tool-preview mono">{preview}</div>
    </details>
  )
})
