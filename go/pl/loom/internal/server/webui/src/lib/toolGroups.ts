import type { BlockModel } from '../app/transcript'

// View-only projection: never replace or mutate the controller's block model.
export type ToolModel = Extract<BlockModel, { kind: 'tool' }>
export type RenderItem =
  | { kind: 'block'; id: string; block: BlockModel }
  | { kind: 'tool-group'; id: string; firstCallId: string; tools: ToolModel[] }

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

export function projectRenderItems(
  blocks: readonly BlockModel[],
  active: boolean,
  revealedIds: ReadonlySet<string> = new Set(),
): RenderItem[] {
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
      if (!eligible(next, i + tools.length)) break
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
  const blockId = item.kind === 'block' ? item.block.id : item.tools[0].id
  const nextIndex = next.items.findIndex((row) =>
    row.kind === 'block' ? row.block.id === blockId : row.tools.some((tool) => tool.id === blockId),
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
