// ToolBlock.tsx — tool block renderer (one-to-one with the old blocks.js toolBlock).
// The live event's preview is a bounded excerpt; full content is fetched on demand
// via fetchToolOutput. The snapshot rebuild path has full_text and never needs it.

import { memo, useMemo, useState } from 'react'
import type { ToolCompletion } from '../../app/transcript'
import { combinedToolTime, runCmdResult, toolDelivery, type ToolModel } from '../../lib/toolGroups'
import { fmtDuration, copyText } from '../../lib/format'
import { diffForToolCall } from '../../lib/diff'
import { Icon, type IconName } from '../../lib/icons'
import { DiffView } from './DiffView'
import { ArtifactBlock, InlineImage, RunCmdAttachment } from './images'
import type { ArtifactRef } from '../../protocol/events'
import { useBlocksIO } from './context'

// st → [icon, label]; className uses English short codes (err/canceled).
// The ok label is only used as the icon's tooltip; err/canceled render it inline.
const TOOL_STATUS: Record<string, ['check' | 'xmark' | 'ban', string]> = {
  ok: ['check', 'Succeeded'],
  err: ['xmark', 'Failed'],
  canceled: ['ban', 'Cancelled'],
}

// Tool kind → [icon, plain-language verb]: the header row shows verb + target
// (what the agent is doing at a glance); the raw toolName goes into the tooltip
// for debugging. MCP tools take the mcp__ prefix branch.
const TOOL_META: Record<string, [IconName, string]> = {
  read_file: ['eye', 'read'],
  list_dir: ['folder-open', 'list'],
  glob: ['magnifying-glass', 'glob'],
  grep: ['magnifying-glass', 'grep'],
  search: ['magnifying-glass', 'search'],
  search_text: ['magnifying-glass', 'search'],
  edit: ['pen-to-square', 'edit'],
  write: ['file', 'write'],
  run_cmd: ['terminal', 'run'],
  exec_session: ['terminal', 'exec'],
  // Retired tool names stay mapped so historical transcripts (merged into
  // exec_session/update_task/memory) still render their icon and label.
  write_stdin: ['terminal', 'stdin'],
  delegate_task: ['robot', 'delegate'],
  cancel_subagent: ['ban', 'cancel subagent'],
  update_task: ['square-check', 'task'],
  update_plan: ['square-check', 'plan'],
  update_goal: ['star', 'goal'],
  memory: ['database', 'memory'],
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

export function toolGroupSummary(tools: readonly ToolModel[]) {
  const verbs = [...new Set(tools.map((tool) => toolMeta(tool.toolName)[1]))].join(' · ')
  const duration = combinedToolTime(tools)
  return { verbs, duration }
}

// Tools whose display target is a workspace file path (and thus revealable in
// the right panel).
const FILE_PATH_TOOLS = new Set(['read_file', 'edit', 'write'])

// pathFromArgs reads the canonical file path out of edit/write args (snapshot
// rebuilds carry args; the live path carries target).
function pathFromArgs(args: unknown): string {
  if (args && typeof args === 'object') {
    const a = args as Record<string, unknown>
    const v = a.path ?? a.file_path
    if (typeof v === 'string' && v) return v
  }
  return ''
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
  presentation?: 'process' | 'delivery'
  review?: boolean
  onShowProcess?: () => void
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
  presentation = 'process',
  review = false,
  onShowProcess,
}: ToolBlockProps) {
  const [targetExpanded, setTargetExpanded] = useState(false)
  const { revealFile } = useBlocksIO()
  // A file path we can hand to the right panel: prefer structured args, fall
  // back to the display target for the known file tools.
  const revealPath =
    pathFromArgs(diffArgs?.args) || (FILE_PATH_TOOLS.has(toolName) ? target || '' : '')
  const canReveal = !!revealFile && !!revealPath
  // Lazy diff: this component is only mounted inside the virtualized render window; useMemo caches by
  // diffArgs reference, so the LCS is not recomputed after scrolling away
  const diffText = useMemo(
    () =>
      presentation === 'delivery' ? undefined :
        diff ?? (diffArgs ? diffForToolCall(diffArgs.name, diffArgs.args) || undefined : undefined),
    [diff, diffArgs, presentation],
  )

  let statusEl
  if (!completion) {
    statusEl = <span className="tool-status running">Running</span>
  } else {
    const st =
      completion.status === 'success' ? 'ok' : completion.status === 'error' ? 'err' : 'canceled'
    const meta = TOOL_STATUS[st]
    // Success is the common case: icon only, with the label kept as tooltip for
    // a11y — the repeated "Succeeded" text is noise in a long transcript. Failure
    // and cancellation keep icon + label so anomalies stand out while scanning.
    statusEl = meta ? (
      st === 'ok' ? (
        <span className={'tool-status ' + st} title={meta[1]}>
          <Icon name={meta[0]} />
        </span>
      ) : (
        <span className={'tool-status ' + st}>
          <Icon name={meta[0]} /> {meta[1]}
        </span>
      )
    ) : (
      <span className={'tool-status ' + st}>{completion.status || 'done'}</span>
    )
  }

  const getFullText = async (): Promise<string> => {
    if (completion?.full_text) return completion.full_text
    if (fetchToolOutput) return fetchToolOutput()
    return completion?.preview || '' // fall back to copying the excerpt when the server is unreachable
  }

  const { command, attachments, images, media, deliveryMedia } = useMemo(
    () => toolDelivery({ toolName, completion }), [toolName, completion],
  )
  const commandPreview =
    command?.text && command.text.length > 600 ? command.text.slice(0, 600) + '\n…' : command?.text
  const [icon, verb] = toolMeta(toolName)
  const mediaEl = images.length > 0
    ? images.map((img, i) => <InlineImage key={i} mediaType={img.media_type} data={img.data} />)
    : (presentation === 'delivery' ? deliveryMedia : media).map((art) => (
        <ArtifactBlock key={art.id} artifact={art} />
      ))
  if (presentation === 'delivery') {
    return (
      <div className="block tool-delivery">
        <div className="delivery-caption">
          <span title={target}>{`From ${verb}${target ? ` · ${target}` : ''}`}</span>
          {review && onShowProcess && (
            <button type="button" className="process-toggle" onClick={onShowProcess}>
              Review in processing details
            </button>
          )}
        </div>
        {mediaEl}
      </div>
    )
  }
  return (
    <div className="block block-tool" data-call-id={callId || undefined}>
      <div className="tool-head">
        <span className="tool-kind" title={toolName || 'tool'}>
          <Icon name={icon} />
        </span>
        <span className="tool-name mono">{verb}</span>
        {target &&
          (canReveal ? (
            <button
              type="button"
              className={'tool-target mono is-path' + (targetExpanded ? ' expanded' : '')}
              title={`${target} — open in the file panel`}
              onClick={(e) => {
                e.stopPropagation()
                revealFile!(revealPath)
              }}
            >
              {target}
            </button>
          ) : (
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
          ))}
        {statusEl}
        <span className="tool-dur mono">
          {completion?.duration_ms != null ? fmtDuration(completion.duration_ms) : ''}
        </span>
      </div>
      {completion && (completion.error_message || completion.error) && (
        <div className="tool-error">{completion.error_message || completion.error}</div>
      )}
      {(completion?.preview || command) && (
        <ToolOutput
          preview={commandPreview || completion?.preview || ''}
          getFullText={
            toolName === 'run_cmd'
              ? async () => {
                  const full = await getFullText()
                  return runCmdResult(full)?.text || full
                }
              : getFullText
          }
          commandOutput={command ? { text: command.text, truncated: command.truncated } : undefined}
          isRunCmd={toolName === 'run_cmd'}
          attachments={attachments}
        />
      )}
      {/* Render images from tool results: inline base64 first (synchronously
          available, no second authenticated request); the artifact path is used
          only when there are no inline images. An artifact is not necessarily an
          image (run_cmd's stdout artifact is text) — ArtifactBlock dispatches by
          media type. */}
      {mediaEl}
      {diffText && !diffSuppressed && <DiffView diffText={diffText} />}
    </div>
  )
})

// Tool output area: collapsed by default; expanding shows the bounded preview.
// For parsed run_cmd JSON, stdout/stderr are themselves bounded: copy the formatted
// preview, not a purportedly complete process output. Other tools retain full copy.
const ToolOutput = memo(function ToolOutput({
  preview,
  getFullText,
  commandOutput,
  isRunCmd = false,
  attachments = [],
}: {
  preview: string
  getFullText: () => Promise<string>
  commandOutput?: { text: string; truncated: boolean }
  isRunCmd?: boolean
  attachments?: { name: 'stdout' | 'stderr'; artifact: ArtifactRef }[]
}) {
  // 'Copying…' disables the button while the full-output fetch is in flight — a double
  // click previously kicked off two fetches; 'Copy failed' is clickable again so a flaky
  // request can be retried.
  const [label, setLabel] = useState<'Copy' | 'Copying…' | 'Copied' | 'Copy failed'>('Copy')
  const truncated = preview.endsWith('\n…')
  return (
    <details className="tool-output disclosure">
      <summary>
        <span className="tool-output-label">
          {`Output · ${commandOutput ? 'preview · ' : ''}${preview.length} chars${truncated || commandOutput?.truncated ? ' · truncated' : ''}`}
        </span>
        <button
          type="button"
          className="tool-copy"
          title={
            commandOutput
              ? 'Copy formatted stdout/stderr preview'
              : isRunCmd
                ? 'Copy tool output (stdout/stderr may be bounded)'
                : 'Copy full output'
          }
          onClick={async (e) => {
            e.preventDefault() // don't toggle the details disclosure
            e.stopPropagation()
            if (label === 'Copying…') return
            setLabel('Copying…')
            try {
              const text = commandOutput ? commandOutput.text : await getFullText()
              if (!(await copyText(text))) throw new Error('clipboard unavailable')
              setLabel('Copied')
            } catch {
              setLabel('Copy failed')
            }
            setTimeout(() => setLabel('Copy'), 1500)
          }}
        >
          {label === 'Copied' ? (
            <>
              <Icon name="check" /> Copied
            </>
          ) : label === 'Copy' && commandOutput ? (
            'Copy preview'
          ) : (
            label
          )}
        </button>
      </summary>
      <div className="tool-preview mono">{preview}</div>
      {attachments.map(({ name, artifact }) => (
        <RunCmdAttachment key={name} name={name} artifact={artifact} />
      ))}
    </details>
  )
})
