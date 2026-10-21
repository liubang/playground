import test from 'node:test'
import assert from 'node:assert/strict'
import { projectRenderItems, combinedToolTime, readingAnchorTop } from './toolGroups.ts'

const readonlyNames = [
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
]
const tool = (id, patch = {}) => ({
  kind: 'tool',
  id,
  v: 0,
  callId: `call-${id}`,
  toolName: 'read_file',
  completion: { status: 'success', duration_ms: 10 },
  ...patch,
})
const block = (kind, id = kind) => ({ kind, id, v: 0 })
const flatten = (items) =>
  items.flatMap((item) => (item.kind === 'block' ? [item.block] : item.tools))
const assertRefs = (items, input) => {
  const flat = flatten(items)
  assert.equal(flat.length, input.length)
  flat.forEach((value, i) => assert.equal(value, input[i]))
}
const freezeDeep = (value) => {
  if (value && typeof value === 'object') {
    Object.values(value).forEach(freezeDeep)
    Object.freeze(value)
  }
  return value
}

for (const name of readonlyNames) {
  test(`snapshot and live parity: ${name}`, () => {
    const live = [tool('a', { toolName: name }), tool('b', { toolName: name })]
    const snapshot = live.map((b) => ({
      ...b,
      diffArgs: { name, args: { path: '/file', content: 'text', new_string: 'text' } },
    }))
    for (const input of [freezeDeep(live), freezeDeep(snapshot)]) {
      const before = JSON.stringify(input)
      const items = projectRenderItems(input, false)
      assert.equal(JSON.stringify(input), before)
      assert.equal(items.length, 1)
      assert.equal(items[0].kind, 'tool-group')
      assert.equal(items[0].firstCallId, 'call-a')
      assertRefs(items, input)
    }
  })
}

for (const name of [
  'edit',
  'write',
  'run_cmd',
  'exec_session',
  'update_task',
  'view_image',
  'mcp__read_file',
  'READ_FILE',
  'read_file_extra',
  'unknown',
  '',
]) {
  test(`strict whitelist excludes ${JSON.stringify(name)}`, () => {
    const input = [tool('a'), tool('x', { toolName: name }), tool('b')]
    const items = projectRenderItems(input, false)
    assert.deepEqual(
      items.map((item) => item.kind),
      ['block', 'block', 'block'],
    )
    assertRefs(items, input)
  })
}

test('mixed whitelisted snapshot calls group in original order', () => {
  const input = freezeDeep(
    readonlyNames.map((name, i) =>
      tool(`mixed-${i}`, {
        toolName: name,
        diffArgs: { name, args: { path: '/file' } },
      }),
    ),
  )
  const items = projectRenderItems(input, false)
  assert.deepEqual(
    items.map((item) => item.tools.length),
    [8, 3],
  )
  assertRefs(items, input)
})

for (const name of ['edit', 'write']) {
  test(`same-name ${name} diffArgs never permits grouping`, () => {
    const input = [
      tool('a'),
      tool('x', {
        toolName: name,
        diffArgs: { name, args: { path: '/file', content: 'new', new_string: 'new' } },
      }),
      tool('b'),
    ]
    assert.ok(projectRenderItems(input, false).every((item) => item.kind === 'block'))
  })
}

for (const name of ['edit', 'write', 'grep', '', 'unknown']) {
  test(`mismatched diffArgs ${JSON.stringify(name)} stays visible`, () => {
    const input = [
      tool('a'),
      tool('x', { diffArgs: { name, args: { content: 'new' } } }),
      tool('b'),
    ]
    assert.deepEqual(
      projectRenderItems(input, false).map((item) => item.kind),
      ['block', 'block', 'block'],
    )
  })
}

for (const kind of [
  'user',
  'assistant',
  'stream',
  'reasoning',
  'thinking',
  'approval',
  'question',
  'notice',
  'resolved',
  'fatal',
  'interrupted',
  'compact',
  'image',
  'artifact',
  'turn_summary',
]) {
  test(`never crosses ${kind} boundary`, () => {
    const input = [tool('a'), tool('b'), block(kind), tool('c'), tool('d')]
    const items = projectRenderItems(input, false)
    assert.deepEqual(
      items.map((item) => item.kind),
      ['tool-group', 'block', 'tool-group'],
    )
    assert.equal(items[1].block, input[2])
    assertRefs(items, input)
  })
}

test('active last turn stays ungrouped, including earlier successful tools before streaming', () => {
  const input = [
    block('user', 'old'),
    tool('a'),
    tool('b'),
    block('assistant', 'answer'),
    block('user', 'live'),
    tool('c'),
    tool('d'),
    block('stream'),
    tool('e'),
    tool('f'),
  ]
  const items = projectRenderItems(input, true)
  assert.equal(items[1].kind, 'tool-group')
  assert.ok(items.slice(2).every((item) => item.kind === 'block'))
  assertRefs(items, input)
  assert.equal(
    projectRenderItems(input, false).filter((item) => item.kind === 'tool-group').length,
    3,
  )
})

test('unknown active turn without user boundary conservatively excludes all tools', () => {
  assert.ok(projectRenderItems([tool('a'), tool('b')], true).every((item) => item.kind === 'block'))
})

for (const count of [0, 1, 2, 8, 9, 10, 16, 17, 25]) {
  test(`cap eight, minimum two, order and identity for ${count} tools`, () => {
    const input = freezeDeep(Array.from({ length: count }, (_, i) => tool(`b${i}`)))
    const before = JSON.stringify(input)
    const items = projectRenderItems(input, false)
    assertRefs(items, input)
    assert.equal(JSON.stringify(input), before)
    for (const item of items) {
      if (item.kind === 'tool-group') {
        assert.ok(item.tools.length >= 2 && item.tools.length <= 8)
        assert.equal(item.firstCallId, item.tools[0].callId)
        assert.equal(item.id, `tool-group:${item.tools[0].id}`)
      }
    }
    assert.deepEqual(
      items.map((item) => (item.kind === 'block' ? 1 : item.tools.length)),
      Array.from({ length: Math.ceil(count / 8) }, (_, i) => Math.min(8, count - i * 8)),
    )
  })
}

const exclusions = [
  ['missing callId', { callId: undefined }],
  ['empty callId', { callId: '' }],
  ['pending', { completion: undefined }],
  ...[
    undefined,
    '',
    'pending',
    'running',
    'error',
    'failed',
    'cancelled',
    'canceled',
    'unknown',
  ].map((status) => [`status ${String(status)}`, { completion: { status } }]),
  ['error message', { completion: { status: 'success', error_message: 'failed' } }],
  ['error', { completion: { status: 'success', error: 'failed' } }],
  [
    'image',
    { completion: { status: 'success', images: [{ media_type: 'image/png', data: 'x' }] } },
  ],
  ['artifact', { completion: { status: 'success', artifacts: [{ id: 'artifact' }] } }],
  ['diff', { diff: '+new' }],
  ['empty actual diff', { diff: '' }],
  ['approval diff suppression', { diffSuppressed: true }],
]
for (const [label, patch] of exclusions) {
  test(`exclusion is a visible boundary: ${label}`, () => {
    const input = freezeDeep([tool('a'), tool('b'), tool('x', patch), tool('c'), tool('d')])
    const items = projectRenderItems(input, false)
    assert.deepEqual(
      items.map((item) => item.kind),
      ['tool-group', 'block', 'tool-group'],
    )
    assert.equal(items[1].block, input[2])
    assertRefs(items, input)
  })
}

test('empty rich arrays and empty error strings do not exclude successful text output', () => {
  const input = [
    tool('a', {
      completion: {
        status: 'success',
        images: [],
        artifacts: [],
        error: '',
        error_message: '',
        preview: 'preview',
        full_text: 'full output',
      },
    }),
    tool('b'),
  ]
  assert.equal(projectRenderItems(input, false)[0].kind, 'tool-group')
})

test('revealed ids split groups without mutating input or caller state', () => {
  const input = freezeDeep([tool('a'), tool('b'), tool('c'), tool('d'), tool('e')])
  const revealed = new Set(['c', 'not-present'])
  const items = projectRenderItems(input, false, revealed)
  assert.deepEqual(
    items.map((item) => item.kind),
    ['tool-group', 'block', 'tool-group'],
  )
  assert.equal(items[1].block, input[2])
  assertRefs(items, input)
  assert.deepEqual([...revealed], ['c', 'not-present'])
})

test('duration is combined tool time, including zero and fractional values', () => {
  const tools = freezeDeep([
    tool('a', { completion: { duration_ms: 0 } }),
    tool('b', { completion: { duration_ms: 1.5 } }),
    tool('c', { completion: { duration_ms: 20 } }),
  ])
  assert.equal(combinedToolTime(tools), 21.5)
})

for (const value of [undefined, null, -1, NaN, Infinity, -Infinity, '10']) {
  test(`invalid or incomplete duration ${String(value)} hides aggregate`, () => {
    assert.equal(
      combinedToolTime([tool('a'), tool('b', { completion: { duration_ms: value } })]),
      undefined,
    )
  })
}

test('empty tools or missing completion has no combined time', () => {
  assert.equal(combinedToolTime([]), undefined)
  assert.equal(combinedToolTime([tool('a', { completion: undefined })]), undefined)
})

test('finite individual durations cannot overflow combined time', () => {
  const large = (id) => tool(id, { completion: { duration_ms: Number.MAX_VALUE } })
  assert.equal(combinedToolTime([large('a')]), Number.MAX_VALUE)
  assert.equal(combinedToolTime([large('a'), large('b')]), undefined)
})

// Heights are supplied independently of projection, just as ResizeObserver can
// replace estimates without changing membership, versions or disclosure state.
const geometry = (items, heights) => {
  const offsets = []
  let total = 0
  for (const height of heights) {
    offsets.push(total)
    total += height + 20
  }
  return freezeDeep({ items, offsets, heights, total })
}

test('reading anchor survives regrouping and subsequent measurements of the same rows', () => {
  const input = [tool('a'), tool('b'), block('assistant', 'answer'), block('user', 'tail')]
  const singletons = geometry(
    projectRenderItems(input, false, new Set(['a', 'b'])),
    [100, 100, 300, 1000],
  )
  const groupedItems = projectRenderItems(input, false)
  const estimated = geometry(groupedItems, [44, 300, 1000])
  // Read 30px into the answer, then close search so the earlier tools regroup.
  const top = readingAnchorTop(singletons, estimated, 294, 24, 24, 12, 200)
  assert.equal(top, 118)
  // The summary wraps on a narrow viewport; projection and row ids are unchanged.
  const measured = geometry(groupedItems, [88, 300, 1000])
  const measuredTop = readingAnchorTop(estimated, measured, top, 24, 24, 12, 200)
  assert.equal(measuredTop, 162)
  const settled = geometry(groupedItems, [72, 300, 1000])
  assert.equal(readingAnchorTop(measured, settled, measuredTop, 24, 24, 12, 200), 146)
  assert.equal(estimated.heights[0], 44)
})

test('measurement changes after the reading row do not move the anchor', () => {
  const items = projectRenderItems([tool('a'), tool('b'), block('assistant')], false)
  const before = geometry(items, [44, 1000])
  const after = geometry(items, [44, 1500])
  assert.equal(readingAnchorTop(before, after, 34, 24, 24, 12, 200), 34)
  assert.equal(readingAnchorTop(before, before, 34, 24, 24, 12, 200), undefined)
})

test('collapsing the reading group clamps its intra-row position to the summary', () => {
  const items = projectRenderItems([tool('a'), tool('b'), block('assistant')], false)
  const expanded = geometry(items, [500, 1000])
  const collapsed = geometry(items, [44, 1000])
  assert.equal(readingAnchorTop(expanded, collapsed, 224, 24, 24, 12, 200), 67)
})

test('a reading singleton remains discoverable when folded into a group', () => {
  const input = [tool('a'), tool('b'), block('assistant')]
  const before = geometry(projectRenderItems(input, true), [100, 100, 1000])
  const after = geometry(projectRenderItems(input, false), [44, 1000])
  assert.equal(readingAnchorTop(before, after, 174, 24, 24, 12, 200), 54)
})

test('a gutter selects the next row and retains its screen position', () => {
  const items = projectRenderItems([tool('a'), tool('b'), block('assistant')], false)
  const before = geometry(items, [44, 1000])
  const after = geometry(items, [88, 1000])
  assert.equal(readingAnchorTop(before, after, 78, 24, 24, 12, 200), 122)
})

test('reading anchor respects changed padding and the new scroll limit', () => {
  const items = projectRenderItems([block('assistant', 'a'), block('assistant', 'b')], false)
  const before = geometry(items, [100, 1000])
  const after = geometry(items, [200, 1000])
  assert.equal(readingAnchorTop(before, after, 154, 24, 40, 12, 200), 270)
  const shortened = geometry(items, [44, 100])
  assert.equal(readingAnchorTop(before, shortened, 154, 24, 24, 12, 150), 70)
})

test('empty layouts and removed reading rows have no restorable anchor', () => {
  const before = geometry(projectRenderItems([block('assistant', 'old')], false), [1000])
  const after = geometry(projectRenderItems([block('assistant', 'new')], false), [1000])
  const empty = geometry([], [])
  assert.equal(readingAnchorTop(before, after, 34, 24, 24, 12, 200), undefined)
  assert.equal(readingAnchorTop(before, empty, 34, 24, 24, 12, 200), undefined)
  assert.equal(readingAnchorTop(empty, after, 0, 24, 24, 12, 200), undefined)
})
