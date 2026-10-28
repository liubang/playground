import type { BlockModel, ToolCompletion } from '../app/transcript'
import type { Message, TurnClosure } from '../protocol/types'

// View-only projection: never replace or mutate the controller's block model.
export type ToolModel = Extract<BlockModel, { kind: 'tool' }>
export type RenderItem =
  | { kind: 'block'; id: string; block: BlockModel }
  | { kind: 'tool-group'; id: string; firstCallId: string; tools: ToolModel[] }
  | { kind: 'process-head'; id: string; runId: string; members: BlockModel[]; hidden: boolean }
  | { kind: 'delivery-tool'; id: string; runId: string; tool: ToolModel; review: boolean }

export function shouldHoldProcessOpen(
  following: boolean,
  selected: boolean,
  interacted: boolean,
  searching: boolean,
): boolean {
  return !following || selected || interacted || searching
}

export interface ProcessOptions {
  closures: readonly TurnClosure[]
  activeRunIds: ReadonlySet<string>
  unknownActivity?: boolean
  expandedRunIds?: ReadonlySet<string>
}

export function snapshotBodyComplete(message: Message): boolean {
  return message.role === 'assistant' && message.status === 'final'
}

export function isFinalContent(block: BlockModel): boolean {
  if (!block.sourceBodyComplete || (block.sourceRole && block.sourceRole !== 'assistant'))
    return false
  switch (block.kind) {
    case 'assistant':
    case 'stream':
      return !!block.text.trim()
    case 'image':
      return !!block.data.trim()
    case 'artifact':
      return !!block.artifact.id
    default:
      return false
  }
}

export function hasCompleteFinalReferences(
  blocks: readonly BlockModel[],
  closure: TurnClosure,
): boolean {
  const ids = closure.final_answer_message_ids
  return (
    !!ids?.length &&
    new Set(ids).size === ids.length &&
    ids.every(
      (id) =>
        typeof id === 'string' &&
        !!id &&
        blocks.some(
          (b) => b.runId === closure.run_id && b.sourceMessageId === id && isFinalContent(b),
        ),
    )
  )
}

export function snapshotReadingHolds(
  closures: readonly TurnClosure[],
  previousClosures: readonly TurnClosure[],
  previousHolds: readonly string[],
  deferred: readonly string[],
  preserveScroll: boolean,
  following: boolean,
  selected: boolean,
): string[] {
  if (!preserveScroll) return []
  const alreadyResolved = new Set(
    previousClosures.map((c) => c.run_id).filter((id) => !deferred.includes(id)),
  )
  return [
    ...new Set([
      ...previousHolds,
      ...(!following || selected
        ? closures.filter((c) => !alreadyResolved.has(c.run_id)).map((c) => c.run_id)
        : []),
    ]),
  ]
}

export function continuesSnapshotRun(
  messages: readonly Message[],
  index: number,
  runId: string,
): boolean {
  if (!runId) return false
  const message = messages[index]
  if (message.metadata?.run_id) return message.metadata.run_id === runId
  return messages.slice(index + 1).find((m) => m.role === 'assistant')?.metadata?.run_id === runId
}

// No positional inference: only canonical message/run attribution can hide rows.
export function processMembers(
  blocks: readonly BlockModel[],
  options: ProcessOptions,
): Map<string, BlockModel[]> {
  const result = new Map<string, BlockModel[]>()
  if (options.unknownActivity) return result
  const byRun = new Map<string, BlockModel[]>()
  for (const b of blocks) {
    if (!b.runId) continue
    const rows = byRun.get(b.runId) || []
    rows.push(b)
    byRun.set(b.runId, rows)
  }
  const closureCounts = new Map<string, number>()
  for (const closure of options.closures)
    closureCounts.set(closure.run_id, (closureCounts.get(closure.run_id) || 0) + 1)
  for (const closure of options.closures) {
    const runId = closure.run_id
    const ids = closure.final_answer_message_ids
    if (
      !runId ||
      closureCounts.get(runId) !== 1 ||
      closure.outcome !== 'succeeded' ||
      !ids?.length ||
      options.activeRunIds.has(runId)
    )
      continue
    const rows = byRun.get(runId) || []
    if (!hasCompleteFinalReferences(rows, closure)) continue
    if (
      rows.some(
        (b) =>
          ((!b.sourceRole || b.sourceRole === 'assistant') &&
            (b.kind === 'assistant' ||
              b.kind === 'stream' ||
              b.kind === 'image' ||
              b.kind === 'artifact') &&
            !b.sourceBodyComplete) ||
          b.kind === 'thinking' ||
          (b.kind === 'reasoning' && b.live) ||
          b.kind === 'approval' ||
          b.kind === 'question' ||
          b.kind === 'fatal' ||
          b.kind === 'interrupted' ||
          (b.kind === 'turn_summary' && (b.failed || b.cancelled)) ||
          (b.kind === 'tool' &&
            (b.completion?.status !== 'success' ||
              b.completion.error ||
              b.completion.error_message)),
      )
    )
      continue
    const finals = new Set(ids)
    const members = rows.filter((b) => {
      if (b.sourceRole && b.sourceRole !== 'assistant') {
        // Synthetic user-role nudges are process, but genuine steers and all
        // system/developer context must remain visible.
        return b.sourceRole === 'user' && b.kind === 'notice' && !b.warn &&
          ['goal_continue', 'plan_reconcile'].includes(b.sourcePromptKind || '')
      }
      // Explicit run ownership is sufficient for successful live tools and
      // same-run lifecycle rows, but never infer an assistant message's identity.
      if (b.kind === 'tool' || b.kind === 'reasoning') return true
      if (b.kind === 'resolved') return b.ok
      if (b.kind === 'compact') return true
      if (b.kind === 'notice') return !b.warn
      if (!b.sourceMessageId) return false
      if (
        b.kind === 'assistant' || b.kind === 'stream' ||
        b.kind === 'image' || b.kind === 'artifact'
      ) return !finals.has(b.sourceMessageId)
      return false
    })
    if (members.length) result.set(runId, members)
  }
  return result
}

export function revealedProcessRuns(
  blocks: readonly BlockModel[],
  ids: ReadonlySet<string>,
): Set<string> {
  return new Set(blocks.filter((b) => ids.has(b.id) && b.runId).map((b) => b.runId!))
}

const PROCESS_PREF_KEY = 'loom_process_disclosures_v1'

// Only opaque session/run ids and booleans; one bounded record, no transcript data.
export function readProcessPreferences(sessionId: string): Map<string, boolean> {
  try {
    const rows: unknown = JSON.parse(localStorage.getItem(PROCESS_PREF_KEY) || '[]')
    if (!Array.isArray(rows)) return new Map()
    return new Map(
      rows
        .slice(-200)
        .filter(
          (r) =>
            Array.isArray(r) &&
            r.length === 3 &&
            r[0] === sessionId &&
            typeof r[1] === 'string' &&
            typeof r[2] === 'boolean',
        )
        .map((r) => [r[1], r[2]]),
    )
  } catch {
    return new Map()
  }
}

export function writeProcessPreference(sessionId: string, runId: string, expanded: boolean): void {
  if (!sessionId || !runId) return
  try {
    const raw: unknown = JSON.parse(localStorage.getItem(PROCESS_PREF_KEY) || '[]')
    const rows = (Array.isArray(raw) ? raw : []).filter(
      (r) =>
        Array.isArray(r) &&
        r.length === 3 &&
        typeof r[0] === 'string' &&
        typeof r[1] === 'string' &&
        typeof r[2] === 'boolean' &&
        !(r[0] === sessionId && r[1] === runId),
    )
    localStorage.setItem(
      PROCESS_PREF_KEY,
      JSON.stringify([...rows, [sessionId, runId, expanded]].slice(-200)),
    )
  } catch {
    /* Storage can be unavailable in private/read-only contexts. */
  }
}

const READ_ONLY_TOOLS = new Set([
  'read_file',
  'list_dir',
  'glob',
  'grep',
  'search',
  'search_text',
  'web_fetch',
  'web_search',
  'kb_search',
  'kb_read',
  'read_skill',
])

const MUTATION_TOOLS = new Set([
  'write', 'edit', 'apply_patch', 'multi_edit', 'MultiEdit', 'write_file',
])

export function isReviewTool(tool: ToolModel): boolean {
  return MUTATION_TOOLS.has(tool.toolName) ||
    tool.diff != null || !!tool.diffSuppressed ||
    MUTATION_TOOLS.has(tool.diffArgs?.name || '')
}

// Share the original completion references with delivery rendering. Command
// attachments must pass the same complete-JSON and text-ref checks as process.
export function toolDelivery(tool: Pick<ToolModel, 'toolName' | 'completion'>) {
  const completion = tool.completion
  const artifacts = completion?.artifacts || []
  const command = tool.toolName === 'run_cmd'
    ? runCmdResult(completion?.full_text || completion?.preview || '')
    : null
  const attachments = command?.attachments.flatMap(({ name, id }) => {
    const artifact = artifacts.find(
      (a) => a.id === id && (a.media_type == null || a.media_type === 'text/plain'),
    )
    return artifact ? [{ name, artifact }] : []
  }) || []
  const mergedIds = new Set(attachments.map(({ artifact }) => artifact.id))
  const images = completion?.images || []
  const media = images.length ? [] : artifacts.filter((a) => !mergedIds.has(a.id))
  // stdout/stderr are process evidence, not user deliverables. If live command
  // JSON is clipped, keep ambiguous text refs in process until the snapshot can
  // identify them; never hide those refs from the original tool renderer.
  const deliveryMedia = tool.toolName === 'run_cmd' && !command
    ? media.filter((a) => a.media_type != null && a.media_type !== 'text/plain')
    : media
  return { command, attachments, images, media, deliveryMedia }
}

// Immutable completion objects survive streaming updates to other blocks.
// Cache only classification, not output strings; WeakMap cannot retain old sessions.
const commandDeliveryCache = new WeakMap<ToolCompletion, boolean>()

export function hasToolDelivery(tool: ToolModel): boolean {
  const completion = tool.completion
  if (completion?.images?.length) return true
  if (!completion?.artifacts?.length) return false
  if (tool.toolName !== 'run_cmd') return true
  const cached = commandDeliveryCache.get(completion)
  if (cached !== undefined) return cached
  const delivers = toolDelivery(tool).deliveryMedia.length > 0
  commandDeliveryCache.set(completion, delivers)
  return delivers
}

// Only complete command JSON can identify stdout/stderr attachments.
export function runCmdResult(text: string): {
  text: string
  truncated: boolean
  attachments: { name: 'stdout' | 'stderr'; id: string }[]
} | null {
  try {
    const value: unknown = JSON.parse(text)
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null
    const r = value as Record<string, unknown>
    if (typeof r.stdout !== 'string' || typeof r.stderr !== 'string') return null
    const lines: string[] = []
    for (const name of ['stdout', 'stderr'] as const) {
      const body = r[name]
      if (body) {
        const clipped = r[`${name}_preview_truncated`] === true
        lines.push(`${name}${clipped ? ' (preview truncated)' : ''}:\n${body}`)
      }
    }
    const status: string[] = []
    if (typeof r.exit_code === 'number') status.push(`exit code: ${r.exit_code}`)
    if (typeof r.signal === 'string' && r.signal) status.push(`signal: ${r.signal}`)
    if (r.timed_out === true) status.push('timed out')
    if (r.cancelled === true) status.push('cancelled')
    if (status.length) lines.push(status.join(' · '))
    if (typeof r.note === 'string' && r.note) lines.push(`note: ${r.note}`)
    const attachments: { name: 'stdout' | 'stderr'; id: string }[] = []
    for (const name of ['stdout', 'stderr'] as const) {
      const ref = r[`${name}_artifact`]
      if (ref && typeof ref === 'object' && !Array.isArray(ref)) {
        const a = ref as Record<string, unknown>
        if (typeof a.id === 'string' && a.id &&
          (a.media_type == null || a.media_type === 'text/plain')) {
          attachments.push({ name, id: a.id })
        }
      }
    }
    return {
      text: lines.join('\n\n') || '(no stdout/stderr)',
      truncated: r.truncated === true || r.stdout_preview_truncated === true ||
        r.stderr_preview_truncated === true,
      attachments,
    }
  } catch {
    return null
  }
}

export function projectRenderItems(
  blocks: readonly BlockModel[],
  active: boolean,
  revealedIds: ReadonlySet<string> = new Set(),
  process?: ProcessOptions,
): RenderItem[] {
  if (process) {
    const members = processMembers(blocks, process)
    const revealRuns = revealedProcessRuns(blocks, revealedIds)
    const memberIds = new Set([...members.values()].flat().map((b) => b.id))
    const closureRuns = new Set(process.closures.map((c) => c.run_id))
    const emitted = new Set<string>()
    const deliveries = new Map<string, RenderItem[]>()
    const anchors = new Map<BlockModel, string>()
    const finalIds = new Map(
      process.closures.map((c) => [c.run_id, new Set(c.final_answer_message_ids)]),
    )
    const lastFinal = new Map<string, BlockModel>()
    const reviewRuns = new Set<string>()
    for (const b of blocks) {
      if (!b.runId || !members.has(b.runId)) continue
      if (finalIds.get(b.runId)?.has(b.sourceMessageId || '') && isFinalContent(b))
        lastFinal.set(b.runId, b)
      if (b.kind === 'turn_summary' && b.changes?.length) reviewRuns.add(b.runId)
    }
    for (const [runId, runMembers] of members) {
      if (process.expandedRunIds?.has(runId) || revealRuns.has(runId)) continue
      // Last final body part, not last message or last process member. Preserve
      // final media order and any trailing review summary in the original stream.
      anchors.set(lastFinal.get(runId)!, runId)
      const hasReview = reviewRuns.has(runId)
      const delivery: RenderItem[] = []
      for (const b of runMembers) {
        if (b.kind === 'image' || b.kind === 'artifact') {
          delivery.push({ kind: 'block', id: b.id, block: b })
        } else if (b.kind === 'tool') {
          const review = isReviewTool(b) && !hasReview
          if (review || hasToolDelivery(b))
            delivery.push({ kind: 'delivery-tool', id: `delivery:${b.id}`, runId, tool: b, review })
        }
      }
      deliveries.set(runId, delivery)
    }
    const result: RenderItem[] = []
    // Legacy unowned history keeps its existing grouping; owned runs stay flat,
    // including failed, active and expanded runs (no nested read-only groups).
    let legacy: BlockModel[] = []
    const flushLegacy = () => {
      result.push(...projectRenderItems(legacy, active, revealedIds))
      legacy = []
    }
    for (const b of blocks) {
      if (!b.runId || (!closureRuns.has(b.runId) && !process.activeRunIds.has(b.runId))) {
        if (legacy.length && legacy[legacy.length - 1].runId !== b.runId) flushLegacy()
        legacy.push(b)
        continue
      }
      flushLegacy()
      const runMembers = members.get(b.runId)
      if (runMembers && memberIds.has(b.id)) {
        const hidden = !process.expandedRunIds?.has(b.runId) && !revealRuns.has(b.runId)
        if (!emitted.has(b.runId)) {
          emitted.add(b.runId)
          result.push({
            kind: 'process-head',
            id: `process:${b.runId}`,
            runId: b.runId,
            members: runMembers,
            hidden,
          })
        }
        if (hidden) continue
      }
      result.push({ kind: 'block', id: b.id, block: b })
      const deliveryRun = anchors.get(b)
      if (deliveryRun) result.push(...deliveries.get(deliveryRun)!)
    }
    flushLegacy()
    return result
  }
  // The app supplies busy; conservatively leave its entire last user turn
  // ungrouped so completed tools never collapse between streaming deltas.
  let liveStart = blocks.length
  if (active) {
    liveStart = 0
    for (let i = blocks.length - 1; i >= 0; i--) {
      if (blocks[i].kind === 'user') {
        liveStart = i
        break
      }
    }
  }
  const eligible = (b: BlockModel, index: number): b is ToolModel =>
    index < liveStart &&
    b.kind === 'tool' &&
    !!b.callId &&
    READ_ONLY_TOOLS.has(b.toolName) &&
    b.completion?.status === 'success' &&
    !b.completion.error &&
    !b.completion.error_message &&
    !b.completion.images?.length &&
    !b.completion.artifacts?.length &&
    b.diff == null &&
    // Snapshots attach args to every tool. diffForToolCall only handles edit/
    // write; same-name whitelisted read-only args cannot produce a diff.
    // Mismatched names remain excluded, even when both names are read-only.
    (b.diffArgs == null || b.diffArgs.name === b.toolName) &&
    !b.diffSuppressed &&
    !revealedIds.has(b.id)
  const result: RenderItem[] = []
  for (let i = 0; i < blocks.length;) {
    const first = blocks[i]
    if (!eligible(first, i)) {
      result.push({ kind: 'block', id: first.id, block: first })
      i++
      continue
    }
    const tools: ToolModel[] = [first]
    while (tools.length < 8 && i + tools.length < blocks.length) {
      const next = blocks[i + tools.length]
      if (!eligible(next, i + tools.length) || next.runId !== first.runId) break
      tools.push(next)
    }
    if (tools.length === 1) {
      result.push({ kind: 'block', id: first.id, block: first })
    } else {
      result.push({
        kind: 'tool-group',
        id: `tool-group:${first.id}`,
        firstCallId: first.callId!,
        tools,
      })
    }
    i += tools.length
  }
  return result
}

export interface RenderGeometry {
  items: readonly RenderItem[]
  offsets: readonly number[]
  heights: readonly number[]
  total: number
}

export function lowerBound(offsets: readonly number[], value: number): number {
  let lo = 0
  let hi = offsets.length
  while (lo < hi) {
    const mid = (lo + hi) >>> 1
    if (offsets[mid] < value) lo = mid + 1
    else hi = mid
  }
  return lo
}

// Preserve the first intersecting row across projection AND measurement changes.
// Geometry snapshots retain old heights even after the measurement cache changes.
export function readingAnchorTop(
  previous: RenderGeometry,
  next: RenderGeometry,
  top: number,
  previousInset: number,
  inset: number,
  bottomInset: number,
  viewportHeight: number,
): number | undefined {
  if (previous === next) return undefined
  let index = Math.max(0, lowerBound(previous.offsets, top - previousInset) - 1)
  // A gutter between rows selects the next row.
  if (
    index < previous.items.length - 1 &&
    previous.offsets[index] + previous.heights[index] < top - previousInset
  )
    index++
  const item = previous.items[index]
  if (!item) return undefined
  // A group's first child identifies its header; singleton ids survive grouping.
  const blockId =
    item.kind === 'block'
      ? item.block.id
      : item.kind === 'delivery-tool'
        ? item.tool.id
        : item.kind === 'tool-group'
          ? item.tools[0].id
          : item.members[0].id
  const nextIndex = next.items.findIndex(
    (row) =>
      row.id === item.id ||
      (row.kind === 'block'
        ? row.block.id === blockId
        : row.kind === 'delivery-tool'
          ? row.tool.id === blockId
          : row.kind === 'tool-group'
            ? row.tools.some((tool) => tool.id === blockId)
            : row.hidden && row.members.some((b) => b.id === blockId)),
  )
  if (nextIndex < 0) return undefined
  const relative = top - (previous.offsets[index] + previousInset)
  // Never keep an intra-row offset beyond a newly collapsed summary.
  const safeRelative = Math.min(relative, Math.max(0, next.heights[nextIndex] - 1))
  const desired = Math.max(0, next.offsets[nextIndex] + inset + safeRelative)
  const maxTop = Math.max(0, next.total + inset + bottomInset - viewportHeight)
  return Math.min(desired, maxTop)
}

// Combined tool execution time, not wall time. Missing or invalid durations
// invalidate the whole aggregate; finite inputs can still overflow the sum.
export function combinedToolTime(tools: readonly ToolModel[]): number | undefined {
  if (tools.length === 0) return undefined
  let sum = 0
  for (const tool of tools) {
    const ms = tool.completion?.duration_ms
    if (typeof ms !== 'number' || !Number.isFinite(ms) || ms < 0) return undefined
    sum += ms
    if (!Number.isFinite(sum)) return undefined
  }
  return sum
}
