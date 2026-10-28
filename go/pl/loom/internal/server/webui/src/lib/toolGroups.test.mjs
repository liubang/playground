import test from 'node:test'
import assert from 'node:assert/strict'
import { registerHooks } from 'node:module'
import {
  projectRenderItems,
  combinedToolTime,
  readingAnchorTop,
  readProcessPreferences,
  writeProcessPreference,
  shouldHoldProcessOpen,
  snapshotBodyComplete,
  snapshotReadingHolds,
  continuesSnapshotRun,
  toolDelivery,
  runCmdResult,
  isReviewTool,
  revealedProcessRuns,
  hasToolDelivery,
} from './toolGroups.ts'

// Production TS uses bundler-style extensionless imports. Resolve those to
// their source files for real controller regressions under Node's TS runner.
const sourceImports = registerHooks({
  resolve(specifier, context, nextResolve) {
    try {
      return nextResolve(specifier, context)
    } catch (error) {
      if (
        error.code !== 'ERR_MODULE_NOT_FOUND' ||
        !specifier.startsWith('.') ||
        /\.[a-z]+$/i.test(specifier)
      )
        throw error
      return nextResolve(`${specifier}.ts`, context)
    }
  },
})
const { TranscriptController } = await import('../app/transcript.ts')
sourceImports.deregister()

const owned = (b, runId = 'r', sourceMessageId = 'process') => ({ ...b, runId, sourceMessageId })
const finalBody = (id = 'final', runId = 'r', messageId = 'f') =>
  owned(
    { kind: 'assistant', id, v: 0, text: 'Complete final answer', sourceBodyComplete: true },
    runId,
    messageId,
  )
const closure = (run_id = 'r', outcome = 'succeeded', final_answer_message_ids = ['f']) => ({
  run_id,
  outcome,
  final_answer_message_ids,
})
const options = (patch = {}) => ({ closures: [closure()], activeRunIds: new Set(), ...patch })
const processProjection = (blocks, patch = {}, revealed = new Set()) =>
  projectRenderItems(freezeDeep(blocks), false, revealed, options(patch))

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

test('success hides process only, retaining all final body parts and rich deliveries', () => {
  const input = [
    owned(block('user')),
    owned(tool('t')),
    owned({ ...block('reasoning'), text: 'reason' }, 'r', 'f'),
    finalBody('part-one'),
    owned({ ...block('image'), data: 'image', sourceBodyComplete: true }, 'r', 'f'),
    finalBody('part-two'),
    owned(
      { ...block('artifact'), artifact: { id: 'image', size: 1 }, sourceBodyComplete: true },
      'r',
      'f',
    ),
  ]
  const before = JSON.stringify(input)
  const items = processProjection(input)
  assert.deepEqual(
    items.map((i) => i.id),
    ['user', 'process:r', 'part-one', 'image', 'part-two', 'artifact'],
  )
  assert.deepEqual(
    items[1].members.map((b) => b.id),
    ['t', 'reasoning'],
  )
  assert.equal(items[1].hidden, true)
  assert.equal(JSON.stringify(input), before)
  for (const item of items.filter((i) => i.kind === 'block'))
    assert.equal(
      item.block,
      input.find((b) => b.id === item.id),
    )
})

for (const outcome of [
  'failed',
  'cancelled',
  'canceled',
  'budget',
  'budget_exhausted',
  'unknown',
  '',
]) {
  test(`non-success ${outcome} keeps all original rows flat`, () => {
    const input = [owned(tool('a')), owned(tool('b')), finalBody()]
    const items = processProjection(input, { closures: [closure('r', outcome)] })
    assertRefs(items, input)
    assert.ok(items.every((i) => i.kind === 'block'))
  })
}

for (const patch of [
  { activeRunIds: new Set(['r']) },
  { unknownActivity: true },
  { closures: [closure('r', 'succeeded', [])] },
  { closures: [closure('r', 'succeeded', ['f', 'missing'])] },
  { closures: [closure('r', 'succeeded', [''])] },
]) {
  test(`incomplete contract or active request remains open: ${JSON.stringify(patch)}`, () => {
    const input = [owned(tool('a')), finalBody()]
    assertRefs(processProjection(input, patch), input)
  })
}

for (const badFinal of [
  finalBody('wrong-run', 'other'),
  { ...finalBody(), sourceBodyComplete: false },
  { ...finalBody(), text: '   ' },
  { ...finalBody(), sourceMessageId: undefined },
  owned({ ...block('reasoning'), text: 'not body', sourceBodyComplete: true }, 'r', 'f'),
]) {
  test(`missing/incomplete final never inferred from nearby answer: ${badFinal.id}`, () => {
    const input = [owned(tool('a')), badFinal]
    assertRefs(processProjection(input), input)
  })
}

for (const unsafe of [
  tool('pending', { completion: undefined }),
  tool('failed', { completion: { status: 'error' } }),
  tool('error', { completion: { status: 'success', error: 'bad' } }),
  block('thinking'),
  { ...block('reasoning'), live: true },
  block('approval'),
  block('question'),
  block('fatal'),
  block('interrupted'),
  { ...block('turn_summary'), cancelled: true },
]) {
  test(`unsafe same-run ${unsafe.id} prevents folding`, () => {
    const input = [owned(tool('a')), owned(unsafe), finalBody()]
    assertRefs(processProjection(input), input)
  })
}

test('legacy history with or without attribution retains previous projection', () => {
  for (const input of [
    [tool('a'), tool('b'), finalBody()],
    [owned(tool('a')), owned(tool('b')), finalBody()],
  ]) {
    assert.deepEqual(processProjection(input, { closures: [] }), projectRenderItems(input, false))
  }
})

test('same-run steer stays visible and expanded rows keep original order, without nested tool groups', () => {
  const input = [owned(tool('a')), owned(block('user', 'steer')), owned(tool('b')), finalBody()]
  const collapsed = processProjection(input)
  assert.deepEqual(
    collapsed.map((i) => i.id),
    ['process:r', 'steer', 'final'],
  )
  const expanded = processProjection(input, { expandedRunIds: new Set(['r']) })
  assert.deepEqual(
    expanded.map((i) => i.id),
    ['process:r', 'a', 'steer', 'b', 'final'],
  )
  assert.deepEqual(
    expanded.filter((i) => i.kind === 'block').map((i) => i.block),
    input,
  )
  assert.ok(expanded.every((i) => i.kind !== 'tool-group'))
})

test('single process member gets disclosure; pure QA does not', () => {
  assert.equal(processProjection([owned(tool('a')), finalBody()])[0].kind, 'process-head')
  assert.equal(processProjection([owned(block('user')), finalBody()]).length, 2)
})

test('complete process folds lifecycle, rich tools and nonfinal media; warnings and review stay visible', () => {
  const process = [
    owned(tool('a')),
    owned({ ...block('notice'), text: 'info' }),
    owned({ ...block('resolved'), ok: true }),
    owned(block('compact')),
    owned({ ...block('image'), data: 'image', sourceBodyComplete: true }),
    owned({ ...block('artifact'), artifact: { id: 'art', size: 1 }, sourceBodyComplete: true }),
    owned(tool('diff', { diff: '+new' }), 'r', 'f'),
    owned(tool('edit', { toolName: 'edit', diffArgs: { name: 'edit', args: {} } })),
    owned(tool('rich', { completion: { status: 'success', artifacts: [{ id: 'artifact', size: 1 }] } })),
  ]
  const input = [
    ...process,
    owned({ ...block('notice', 'warning'), text: 'warning', warn: true }),
    owned({ ...block('resolved', 'denied'), ok: false }),
    finalBody(),
    owned({ ...block('turn_summary'), changes: [] }),
  ]
  const before = JSON.stringify(input)
  const items = processProjection(input)
  assert.deepEqual(items.map((i) => i.id), [
    'process:r', 'warning', 'denied', 'final', 'image', 'artifact',
    'delivery:diff', 'delivery:edit', 'delivery:rich', 'turn_summary',
  ])
  assert.deepEqual(items[0].members, process)
  assert.ok(!items.some((i) => i.kind === 'block' && i.block.kind === 'tool'))
  for (const item of items.filter((i) => i.kind === 'delivery-tool'))
    assert.equal(item.tool, input.find((b) => `delivery:${b.id}` === item.id))
  assert.equal(JSON.stringify(input), before)
  const expanded = processProjection(input, { expandedRunIds: new Set(['r']) })
  assert.deepEqual(expanded.filter((i) => i.kind !== 'process-head').map((i) => i.block), input)
  expanded.filter((i) => i.kind === 'block').forEach((item, i) => assert.equal(item.block, input[i]))
})

test('unowned process is never guessed; final refs and failures never cross runs', () => {
  const input = [
    tool('unknown'),
    owned(tool('a')),
    owned(tool('b'), 'other'),
    owned(block('fatal'), 'other'),
    finalBody(),
  ]
  const items = processProjection(input)
  assert.deepEqual(
    items.find((i) => i.kind === 'process-head').members.map((b) => b.id),
    ['a'],
  )
  assert.ok(items.some((i) => i.id === 'unknown'))
  assert.ok(items.some((i) => i.id === 'b'))
  const cross = processProjection([owned(tool('a')), owned(tool('b'), 'other')], { closures: [] })
  assert.ok(cross.every((i) => i.kind === 'block'))
})

test('search/repeated reveal expands the entire run each time without extracting a hit', () => {
  const input = [
    owned(tool('a')),
    owned(tool('b')),
    finalBody(),
    owned(tool('c'), 'other'),
    finalBody('f2', 'other', 'f2'),
  ]
  const opts = { closures: [closure(), closure('other', 'succeeded', ['f2'])] }
  for (let i = 0; i < 3; i++) {
    const items = processProjection(input, opts, new Set(['b']))
    assert.deepEqual(
      items.map((i) => i.id),
      ['process:r', 'a', 'b', 'final', 'process:other', 'f2'],
    )
    assert.equal(items[0].hidden, false)
    assert.equal(items[4].hidden, true)
  }
})

test('process reading anchor maps hidden member to header and header survives expansion', () => {
  const input = [owned(tool('a')), owned(tool('b')), finalBody()]
  const before = geometry(projectRenderItems(input, true), [100, 100, 1000])
  const collapsedItems = processProjection(input)
  const collapsed = geometry(collapsedItems, [28, 1000])
  assert.equal(readingAnchorTop(before, collapsed, 174, 24, 24, 12, 200), 51)
  const expanded = geometry(
    processProjection(input, { expandedRunIds: new Set(['r']) }),
    [28, 100, 100, 1000],
  )
  assert.equal(readingAnchorTop(collapsed, expanded, 34, 24, 24, 12, 200), 34)
  assert.equal(readingAnchorTop(expanded, collapsed, 224, 24, 24, 12, 200), 51)
})

test('closure arrival auto-collapse requires following, no selection, interaction or search', () => {
  for (const following of [false, true])
    for (const selected of [false, true])
      for (const interacted of [false, true])
        for (const searching of [false, true])
          assert.equal(
            shouldHoldProcessOpen(following, selected, interacted, searching),
            !following || selected || interacted || searching,
          )
})

test('all final IDs must resolve complete bodies; one incomplete body part blocks folding', () => {
  const input = [owned(tool('a')), finalBody(), finalBody('f2', 'r', 'f2')]
  assert.equal(
    processProjection(input, { closures: [closure('r', 'succeeded', ['f', 'f2'])] })[0].kind,
    'process-head',
  )
  const partial = [...input, { ...finalBody('partial'), sourceBodyComplete: false }]
  assertRefs(processProjection(partial), partial)
})

for (const rich of [
  { ...block('image'), data: 'image', mediaType: 'image/png', sourceBodyComplete: true },
  { ...block('artifact'), artifact: { id: 'delivery', size: 1 }, sourceBodyComplete: true },
]) {
  test(`final ${rich.kind}-only content folds process but retains delivery`, () => {
    const input = [owned(tool('a')), owned(rich, 'r', 'f')]
    assert.deepEqual(
      processProjection(input).map((i) => i.id),
      ['process:r', rich.id],
    )
    const incomplete = [input[0], { ...input[1], sourceBodyComplete: false }]
    assertRefs(processProjection(incomplete), incomplete)
  })
}

test('empty image and artifact references cannot resolve a final', () => {
  for (const rich of [
    { ...block('image'), data: ' ', sourceBodyComplete: true },
    { ...block('artifact'), artifact: { id: '', size: 0 }, sourceBodyComplete: true },
  ]) {
    const input = [owned(tool('a')), owned(rich, 'r', 'f')]
    assertRefs(processProjection(input), input)
  }
})

for (const status of [undefined, '', 'unknown', 'streaming', 'interrupted', 'draft', 'final']) {
  test(`snapshot status ${String(status)} is complete only for canonical final`, () => {
    const complete = snapshotBodyComplete({ id: 'f', role: 'assistant', status })
    assert.equal(complete, status === 'final')
    const input = [owned(tool('a')), { ...finalBody(), sourceBodyComplete: complete }]
    if (status === 'final') assert.equal(processProjection(input)[0].kind, 'process-head')
    else assertRefs(processProjection(input), input)
    assert.equal(snapshotBodyComplete({ id: 'f', role: 'user', status }), false)
  })
}

test('system/developer/unknown context never hides or resolves final references', () => {
  for (const role of ['system', 'developer', 'unknown', 'user']) {
    const context = owned({
      ...block('reasoning', role),
      text: 'visible context',
      sourceRole: role,
    })
    const input = [owned(tool('a')), context, finalBody()]
    assert.ok(processProjection(input).some((i) => i.id === role))
    const wrongRole = [owned(tool('a')), { ...finalBody(), sourceRole: role }]
    assertRefs(processProjection(wrongRole), wrongRole)
  }
})

for (const closures of [
  [closure('r', 'succeeded', ['f', 'f'])],
  [closure(), closure()],
  [closure(), closure('r', 'failed')],
  [closure('r', 'failed'), closure()],
  [closure(), closure('r', 'succeeded', ['other'])],
]) {
  test(`duplicate IDs or closures reject folding: ${JSON.stringify(closures)}`, () => {
    const input = [owned(tool('a')), finalBody()]
    assertRefs(processProjection(input, { closures: freezeDeep(closures) }), input)
  })
}

test('duplicate closure in another run never disables a unique safe run', () => {
  const items = processProjection([owned(tool('a')), finalBody()], {
    closures: [closure(), closure('other'), closure('other')],
  })
  assert.equal(items[0].kind, 'process-head')
})

test('deferred closure retains reading/selection/interaction hold through same-session resync', () => {
  const closures = freezeDeep([closure()])
  for (const [following, selected, holds] of [
    [false, false, []],
    [true, true, []],
    [true, false, ['r']],
  ]) {
    assert.deepEqual(
      snapshotReadingHolds(closures, closures, holds, ['r'], true, following, selected),
      ['r'],
    )
  }
  assert.deepEqual(snapshotReadingHolds(closures, closures, ['r'], ['r'], true, true, false), ['r'])
  assert.deepEqual(snapshotReadingHolds(closures, closures, ['r'], ['r'], false, false, true), [])
  // Explicitly collapsed, already resolved groups must not be overridden merely
  // because clicking their header detached the reader from bottom following.
  assert.deepEqual(snapshotReadingHolds(closures, closures, [], [], true, false, false), [])
})

test('same-run steer does not close review early; next-run prompt does', () => {
  const messages = freezeDeep([
    { id: 'a', role: 'assistant', metadata: { run_id: 'r' } },
    { id: 's', role: 'user', metadata: { run_id: 'r', prompt_kind: 'steer' } },
    { id: 'f', role: 'assistant', metadata: { run_id: 'r' } },
    { id: 'n', role: 'user', metadata: { run_id: 'other' } },
    { id: 'o', role: 'assistant', metadata: { run_id: 'other' } },
  ])
  assert.equal(continuesSnapshotRun(messages, 1, 'r'), true)
  assert.equal(continuesSnapshotRun(messages, 3, 'r'), false)
  const legacySteer = [{ id: 's', role: 'user' }, messages[2]]
  assert.equal(continuesSnapshotRun(legacySteer, 0, 'r'), true)
  assert.equal(continuesSnapshotRun([{ id: 'new', role: 'user' }], 0, 'r'), false)
})

test('runtime closure without final body remains open until complete snapshot content', () => {
  const process = owned(tool('a'))
  assertRefs(processProjection([process]), [process])
  const snapshot = [
    process,
    owned({ ...block('image'), data: 'image', sourceBodyComplete: true }, 'r', 'f'),
  ]
  assert.equal(processProjection(snapshot)[0].kind, 'process-head')
})

test('process preferences are scoped, bounded and defensive, storing no transcript content', () => {
  const previous = globalThis.localStorage
  let data = 'bad json'
  globalThis.localStorage = {
    getItem: () => data,
    setItem: (_, v) => {
      data = v
    },
  }
  try {
    assert.equal(readProcessPreferences('s').size, 0)
    data = '[]'
    for (let i = 0; i < 205; i++) writeProcessPreference('s', `r${i}`, true)
    assert.equal(JSON.parse(data).length, 200)
    assert.equal(readProcessPreferences('other').size, 0)
    writeProcessPreference('other', 'r204', false)
    assert.equal(readProcessPreferences('s').get('r204'), true)
    assert.equal(readProcessPreferences('other').get('r204'), false)
    globalThis.localStorage = {
      getItem: () => {
        throw new Error('denied')
      },
    }
    assert.equal(readProcessPreferences('s').size, 0)
    assert.doesNotThrow(() => writeProcessPreference('s', 'r', true))
  } finally {
    globalThis.localStorage = previous
  }
})

const controllerTest = (name, fn) =>
  test(name, (t) => {
    const previousRAF = globalThis.requestAnimationFrame
    globalThis.requestAnimationFrame = () => 0
    t.after(() => {
      globalThis.requestAnimationFrame = previousRAF
    })
    const controller = new TranscriptController({
      resolveApproval: async () => {},
      answerQuestion: async () => {},
      onError: (error) => {
        throw error
      },
    })
    fn(controller)
  })
const completedEvent = (message_id = 'M', text = 'Canonical final', request_id) => ({
  kind: 'model.response_completed',
  run_id: 'r',
  request_id,
  payload: { message_id, text },
})
const canonicalSnapshot = (parts = [{ kind: 'text', text: 'Canonical final' }]) => ({
  state: 'idle',
  event_seq: 10,
  messages: [{ id: 'M', role: 'assistant', status: 'final', metadata: { run_id: 'r' }, parts }],
  turn_closures: [closure('r', 'succeeded', ['M'])],
})

controllerTest(
  'canonical snapshot followed by completed replay preserves rows, final and closures',
  (controller) => {
    controller.applySnapshot(freezeDeep(canonicalSnapshot()))
    const state = controller.store.get()
    const blocks = state.blocks
    const closures = state.closures
    const before = JSON.stringify(blocks)
    controller.handleEvent(completedEvent())
    controller.handleEvent(completedEvent('M', 'Replayed correction must not override snapshot'))
    assert.equal(controller.store.get().blocks, blocks)
    assert.equal(JSON.stringify(controller.store.get().blocks), before)
    assert.equal(controller.store.get().closures, closures)
    assert.equal(closures.length, 1)
    assert.equal(blocks[0].sourceMessageId, 'M')
    assert.equal(blocks[0].text, 'Canonical final')
  },
)

controllerTest(
  'rich canonical snapshot replay never appends text or hides final deliveries',
  (controller) => {
    controller.applySnapshot(
      canonicalSnapshot([
        { kind: 'image', image: { media_type: 'image/png', data: 'image' } },
        { kind: 'artifact_ref', artifact: { id: 'artifact', size: 1 } },
      ]),
    )
    const blocks = controller.store.get().blocks
    controller.handleEvent(completedEvent())
    assert.equal(controller.store.get().blocks, blocks)
    assert.deepEqual(
      blocks.map((b) => b.kind),
      ['image', 'artifact'],
    )
  },
)

controllerTest(
  'first live completion corrects draft canonically; identical repeats are no-ops',
  (controller) => {
    controller.handleEvent({
      kind: 'model.text_delta',
      run_id: 'r',
      payload: { delta: 'Partial draft' },
    })
    controller.handleEvent(completedEvent())
    const blocks = controller.store.get().blocks
    assert.equal(blocks.length, 1)
    assert.equal(blocks[0].text, 'Canonical final')
    assert.equal(blocks[0].sourceMessageId, 'M')
    controller.handleEvent(completedEvent())
    controller.handleEvent(completedEvent())
    assert.equal(controller.store.get().blocks, blocks)
    controller.handleEvent({
      kind: 'turn.finished',
      run_id: 'r',
      payload: { closure: closure('r', 'succeeded', ['M']) },
    })
    assert.equal(controller.store.get().closures.length, 1)
    assert.equal(controller.store.get().blocks.filter((b) => b.sourceMessageId === 'M').length, 1)
  },
)

controllerTest(
  'old-server completions without message_id retain existing append semantics',
  (controller) => {
    // Explicitly omit ID (the helper default otherwise supplies it).
    const event = { kind: 'model.response_completed', run_id: 'r', payload: { text: 'Legacy' } }
    controller.handleEvent(event)
    controller.handleEvent(event)
    assert.equal(controller.store.get().blocks.filter((b) => b.text === 'Legacy').length, 2)
  },
)

for (const requestId of [undefined, 'newer-request']) {
  controllerTest(
    `canonical replay never consumes newer/unowned draft (${String(requestId)})`,
    (controller) => {
      controller.applySnapshot(canonicalSnapshot())
      controller.handleEvent({
        kind: 'model.reasoning_delta',
        run_id: 'r',
        request_id: requestId,
        payload: { delta: 'new reasoning' },
      })
      controller.handleEvent({
        kind: 'model.text_delta',
        run_id: 'r',
        request_id: requestId,
        payload: { delta: 'new draft' },
      })
      const blocks = controller.store.get().blocks
      const before = JSON.stringify(blocks)
      controller.handleEvent(completedEvent('M', 'Canonical final', 'older-request'))
      assert.equal(controller.store.get().blocks, blocks)
      assert.equal(JSON.stringify(blocks), before)
      controller.handleEvent(completedEvent('N', 'New final', requestId))
      assert.ok(controller.store.get().blocks.some((b) => b.sourceMessageId === 'M'))
      assert.ok(
        controller.store
          .get()
          .blocks.some((b) => b.sourceMessageId === 'N' && b.text === 'New final'),
      )
    },
  )
}

controllerTest(
  'replayed response clears only request-owned redundant drafts, retaining canonical rows',
  (controller) => {
    controller.applySnapshot(canonicalSnapshot())
    controller.handleEvent({
      kind: 'model.reasoning_delta',
      run_id: 'r',
      request_id: 'old',
      payload: { delta: 'replay reasoning' },
    })
    controller.handleEvent({
      kind: 'model.text_delta',
      run_id: 'r',
      request_id: 'old',
      payload: { delta: 'replay draft' },
    })
    controller.handleEvent(completedEvent('M', 'Canonical final', 'old'))
    // Sealed reasoning has no live pointer; it is intentionally not removed by
    // an ownership guess. The request-owned live stream is safely discarded.
    assert.equal(controller.store.get().blocks.filter((b) => b.kind === 'stream').length, 0)
    assert.equal(controller.store.get().blocks.filter((b) => b.sourceMessageId === 'M').length, 1)
  },
)

controllerTest(
  'duplicate completion clears matching live reasoning and thinking only',
  (controller) => {
    controller.applySnapshot(canonicalSnapshot())
    controller.handleEvent({
      kind: 'model.reasoning_delta',
      run_id: 'r',
      request_id: 'old',
      payload: { delta: 'replayed' },
    })
    controller.handleEvent(completedEvent('M', 'Canonical final', 'old'))
    assert.deepEqual(
      controller.store.get().blocks.map((b) => b.kind),
      ['assistant'],
    )
    controller.handleEvent({
      kind: 'tool.completed',
      run_id: 'r',
      request_id: 'new',
      payload: { call_id: 'missing' },
    })
    const blocks = controller.store.get().blocks
    controller.handleEvent(completedEvent('M', 'Canonical final', 'old'))
    assert.equal(controller.store.get().blocks, blocks)
    assert.ok(blocks.some((b) => b.kind === 'thinking'))
    controller.handleEvent(completedEvent('M', 'Canonical final', 'new'))
    assert.deepEqual(
      controller.store.get().blocks.map((b) => b.kind),
      ['assistant'],
    )
  },
)

controllerTest(
  'snapshot tool prepared/completed replay remains idempotent by call id',
  (controller) => {
    const snapshot = canonicalSnapshot([
      {
        kind: 'tool_call',
        tool_call: { id: 'call', name: 'read_file', arguments: { path: 'file' } },
      },
      { kind: 'tool_result', tool_result: { call_id: 'call', status: 'success', content: [] } },
      { kind: 'text', text: 'Canonical final' },
    ])
    controller.applySnapshot(snapshot)
    const count = controller.store.get().blocks.length
    controller.handleEvent({
      kind: 'tool.prepared',
      run_id: 'r',
      payload: { call_id: 'call', tool_name: 'read_file' },
    })
    controller.handleEvent({
      kind: 'tool.completed',
      run_id: 'r',
      payload: { call_id: 'call', status: 'success' },
    })
    assert.equal(controller.store.get().blocks.filter((b) => b.kind === 'tool').length, 1)
    assert.ok(controller.store.get().blocks.length <= count + 1) // existing next-step thinking semantics
  },
)

controllerTest(
  'canonical response registry resets with the session, not across same-session snapshot replay',
  (controller) => {
    controller.applySnapshot(canonicalSnapshot())
    controller.clear()
    controller.handleEvent(completedEvent())
    assert.equal(controller.store.get().blocks.length, 1)
  },
)

test('explicit run-owned successful live tool without source message folds only after verified final', () => {
  const liveTool = owned(tool('live'), 'r', undefined)
  liveTool.sourceMessageId = undefined
  const input = [liveTool, finalBody()]
  assert.equal(processProjection(input)[0].members[0], liveTool)
  for (const patch of [{ activeRunIds: new Set(['r']) }, { unknownActivity: true },
    { closures: [closure('r', 'succeeded', ['missing'])] }]) assertRefs(processProjection(input, patch), input)
})

test('nonfinal assistant and stream text join process while rich active runs stay reference-identical', () => {
  const input = [
    owned({ ...block('assistant', 'draft'), text: 'draft', sourceBodyComplete: true }),
    owned({ ...block('stream', 'stream-draft'), text: 'sealed stream', sourceBodyComplete: true }),
    owned(tool('rich-write', { toolName: 'write', diff: '+new', completion: {
      status: 'success', images: [{ media_type: 'image/png', data: 'i' }],
      artifacts: [{ id: 'artifact', size: 1 }],
    } })),
    finalBody(),
  ]
  const items = processProjection(input)
  assert.deepEqual(items[0].members, input.slice(0, 3))
  assert.deepEqual(items.map((i) => i.id), ['process:r', 'final', 'delivery:rich-write'])
  assert.equal(items.at(-1).tool, input[2])
  assertRefs(processProjection(input, { activeRunIds: new Set(['r']) }), input)
  assertRefs(processProjection(input, { expandedRunIds: new Set(['r']) }).filter((i) => i.kind !== 'process-head'), input)
})

controllerTest('real live tool starts run-owned then folds after canonical final attribution and closure', (controller) => {
  controller.handleEvent({ kind: 'tool.prepared', run_id: 'r', payload: {
    call_id: 'live-call', tool_name: 'write', target: 'file',
  } })
  controller.handleEvent({ kind: 'tool.completed', run_id: 'r', payload: {
    call_id: 'live-call', status: 'success',
  } })
  assert.equal(controller.store.get().blocks.find((b) => b.kind === 'tool').sourceMessageId, undefined)
  controller.handleEvent(completedEvent())
  controller.handleEvent({ kind: 'turn.finished', run_id: 'r', payload: {
    closure: closure('r', 'succeeded', ['M']),
  } })
  const state = controller.store.get()
  const b = state.blocks.find((b) => b.kind === 'tool')
  assert.equal(b.runId, 'r')
  assert.equal(b.sourceMessageId, 'M')
  const items = projectRenderItems(state.blocks, false, new Set(), {
    closures: state.closures, activeRunIds: new Set(),
  })
  assert.ok(items.find((i) => i.kind === 'process-head').members.includes(b))
  assert.equal(items.find((i) => i.kind === 'delivery-tool').tool, b)
})

for (const name of ['write', 'edit', 'apply_patch', 'multi_edit', 'MultiEdit', 'write_file']) {
  test(`${name} without diff keeps a delivery-only review unless summary has real changes`, () => {
    const write = owned(tool('write', { toolName: name }))
    for (const changes of [undefined, [], [{ path: 'file', added: 1, removed: 0 }]]) {
      const summary = owned({ ...block('turn_summary'), changes })
      const items = processProjection([write, finalBody(), summary])
      const delivery = items.find((i) => i.kind === 'delivery-tool')
      assert.equal(!!delivery, !changes?.length)
      if (delivery) {
        assert.equal(delivery.tool, write)
        assert.equal(delivery.review, true)
      }
      assert.equal(items.at(-1).block, summary)
    }
    const rich = { ...write, completion: { status: 'success', images: [{ media_type: 'image/png', data: 'x' }] } }
    const summary = owned({ ...block('turn_summary'), changes: [{ path: 'file' }] })
    const items = processProjection([rich, finalBody(), summary])
    assert.equal(items.find((i) => i.kind === 'delivery-tool').review, false)
    assert.equal(items.find((i) => i.kind === 'delivery-tool').tool, rich)
  })
}

for (const inline of [true, false]) {
  test(`tool delivery retains original ${inline ? 'inline' : 'artifact'} refs and inline priority`, () => {
    const images = [{ media_type: 'image/png', data: 'image' }]
    const artifacts = [{ id: 'img', size: 1, media_type: 'image/png' }, { id: 'file', size: 2 }]
    const b = owned(tool('rich', { completion: { status: 'success', images: inline ? images : [], artifacts } }))
    const result = toolDelivery(freezeDeep(b))
    if (inline) {
      assert.equal(result.images, images)
      assert.deepEqual(result.media, [])
    } else result.media.forEach((a, i) => assert.equal(a, artifacts[i]))
    const items = processProjection([b, finalBody()])
    assert.equal(items.at(-1).tool, b)
    assert.ok(!items.some((i) => i.kind === 'block' && i.block === b))
  })
}

test('stdout/stderr attachments retain exact text refs, complete JSON checks and inline priority', () => {
  const out = { id: 'out', size: 99, media_type: 'text/plain' }
  const err = { id: 'err', size: 30 }
  const file = { id: 'file', size: 1, media_type: 'application/pdf' }
  const command = {
    stdout: 'stdout preview', stderr: 'stderr preview', stdout_preview_truncated: true,
    stdout_artifact: { id: 'out', media_type: 'text/plain' }, stderr_artifact: { id: 'err' },
  }
  const b = owned(tool('cmd', { toolName: 'run_cmd', completion: {
    status: 'success', full_text: JSON.stringify(command), artifacts: [out, err, file],
  } }))
  const before = JSON.stringify(b)
  const delivery = toolDelivery(freezeDeep(b))
  assert.equal(delivery.attachments[0].artifact, out)
  assert.equal(delivery.attachments[1].artifact, err)
  assert.equal(delivery.media[0], file)
  assert.deepEqual(delivery.deliveryMedia, [file])
  assert.equal(delivery.command.truncated, true)
  assert.equal(JSON.stringify(b), before)
  assert.equal(processProjection([b, finalBody()]).at(-1).tool, b)
  assert.equal(runCmdResult('{"stdout":"partial'), null)
  assert.equal(runCmdResult('{"stdout":"missing stderr"}'), null)
  const mismatched = toolDelivery({ ...b, completion: { ...b.completion, artifacts: [file] } })
  assert.deepEqual(mismatched.attachments, [])
  const nonText = toolDelivery({ ...b, completion: { ...b.completion, artifacts: [{ ...out, media_type: 'image/png' }] } })
  assert.deepEqual(nonText.attachments, [])
  const inline = toolDelivery({ ...b, completion: { ...b.completion, images: [{ media_type: 'image/png', data: 'i' }] } })
  assert.equal(inline.attachments[0].artifact, out)
  assert.deepEqual(inline.media, [])
})

test('weather workflow folds automatic command logs and delivers only the chart', () => {
  const command = (id, stdout) => {
    const artifact = { id: `stdout-${id}`, size: stdout.length, media_type: 'text/plain' }
    return owned(tool(id, { toolName: 'run_cmd', completion: {
      status: 'success', artifacts: [artifact], full_text: JSON.stringify({
        stdout, stderr: '', stdout_artifact: { id: artifact.id }, exit_code: 0,
      }),
    } }))
  }
  const chart = { id: 'chart', size: 193332, media_type: 'image/png' }
  const steps = [
    owned(tool('skill', { toolName: 'read_skill' })),
    owned(tool('glob', { toolName: 'glob' })),
    command('inspect', '7 2026-10-22 2026-10-28'),
    command('plot', 'Font check passed; chart saved'),
    owned(tool('present', { toolName: 'present_image', completion: { status: 'success', artifacts: [chart] } })),
    command('cleanup', 'cleaned'),
  ]
  const input = [block('user'), ...steps, finalBody()]
  const items = processProjection(input)
  assert.deepEqual(items.map((i) => i.id), ['user', 'process:r', 'final', 'delivery:present'])
  assert.deepEqual(items[1].members, steps)
  assert.equal(items.at(-1).tool, steps[4])
  for (const index of [2, 3, 5]) {
    assert.equal(hasToolDelivery(steps[index]), false)
    assert.equal(toolDelivery(steps[index]).attachments.length, 1)
    assert.deepEqual(toolDelivery(steps[index]).deliveryMedia, [])
  }
  assertRefs(processProjection(input, { expandedRunIds: new Set(['r']) }).filter((i) => i.kind !== 'process-head'), input)
  assertRefs(processProjection(input, { activeRunIds: new Set(['r']) }), input)
})

test('repeated projection classifies immutable command completion only once', () => {
  let reads = 0
  const completion = {
    status: 'success', artifacts: [{ id: 'out', size: 10, media_type: 'text/plain' }],
    get full_text() {
      reads++
      return JSON.stringify({ stdout: 'cleaned', stderr: '', stdout_artifact: { id: 'out' } })
    },
  }
  const b = owned(tool('cmd', { toolName: 'run_cmd', completion }))
  for (let i = 0; i < 100; i++) {
    const items = projectRenderItems([b, finalBody()], false, new Set(), options())
    assert.deepEqual(items.map((item) => item.id), ['process:r', 'final'])
  }
  assert.equal(reads, 1)
  // A new authoritative completion must be classified independently.
  const updated = { ...b, completion: { status: 'success', artifacts: [
    { id: 'report', size: 20, media_type: 'application/pdf' },
  ] } }
  assert.equal(hasToolDelivery(updated), true)
  assert.equal(hasToolDelivery({ ...b, toolName: 'resource' }), true)
})

test('clipped command JSON defers ambiguous logs without removing process refs or real media', () => {
  const out = { id: 'out', size: 10, media_type: 'text/plain' }
  const unknown = { id: 'unknown', size: 12 }
  const chart = { id: 'chart', size: 20, media_type: 'image/png' }
  for (const preview of ['{"stdout":"clipped', '{"stdout":"missing stderr"}', '']) {
    const b = owned(tool('cmd', { toolName: 'run_cmd', completion: {
      status: 'success', preview, artifacts: [out, unknown, chart],
    } }))
    const delivery = toolDelivery(freezeDeep(b))
    assert.deepEqual(delivery.media, [out, unknown, chart])
    assert.deepEqual(delivery.deliveryMedia, [chart])
    assert.equal(hasToolDelivery(b), true)
    const logs = { ...b, completion: { ...b.completion, artifacts: [out, unknown] } }
    assert.equal(hasToolDelivery(logs), false)
    assert.deepEqual(processProjection([logs, finalBody()]).map((i) => i.id), ['process:r', 'final'])
  }
})

test('explicit command file deliverables and final log references remain visible', () => {
  const log = { id: 'out', size: 10, media_type: 'text/plain' }
  const report = { id: 'report', size: 20, media_type: 'text/plain' }
  const b = owned(tool('cmd', { toolName: 'run_cmd', completion: {
    status: 'success', artifacts: [log, report], full_text: JSON.stringify({
      stdout: 'done', stderr: '', stdout_artifact: { id: 'out' },
    }),
  } }))
  const finalLog = owned({ ...block('artifact', 'final-log'), artifact: log, sourceBodyComplete: true }, 'r', 'f')
  assert.deepEqual(toolDelivery(b).deliveryMedia, [report])
  assert.deepEqual(processProjection([b, finalBody(), finalLog]).map((i) => i.id), [
    'process:r', 'final', 'final-log', 'delivery:cmd',
  ])
})

test('multiple final messages/body parts across interleaved runs and steer anchor delivery once at last final', () => {
  const image = owned({ ...block('image', 'early-image'), data: 'i', sourceBodyComplete: true })
  const write = owned(tool('write', { toolName: 'write' }), 'r', 'f')
  const input = [
    image, finalBody('first'), owned(block('user', 'steer')), write,
    owned(tool('other-tool'), 'other'), finalBody('other-final', 'other', 'of'),
    finalBody('second', 'r', 'f2'),
    owned({ ...block('image', 'final-image'), data: 'f', sourceBodyComplete: true }, 'r', 'f2'),
    owned({ ...block('reasoning'), text: 'final reasoning' }, 'r', 'f2'),
    owned({ ...block('turn_summary'), changes: [] }),
  ]
  const opts = { closures: [closure('r', 'succeeded', ['f', 'f2']), closure('other', 'succeeded', ['of'])] }
  const items = processProjection(input, opts)
  assert.deepEqual(items.map((i) => i.id), [
    'process:r', 'first', 'steer', 'process:other', 'other-final', 'second',
    'final-image', 'early-image', 'delivery:write', 'turn_summary',
  ])
  assert.equal(items.find((i) => i.id === 'early-image').block, image)
  assert.equal(new Set(items.map((i) => i.id)).size, items.length)
  for (const patch of [{ expandedRunIds: new Set(['r', 'other']) }]) {
    const expanded = processProjection(input, { ...opts, ...patch })
    assertRefs(expanded.filter((i) => i.kind !== 'process-head'), input)
    assert.ok(!expanded.some((i) => i.kind === 'delivery-tool'))
  }
  const revealed = processProjection(input, opts, new Set(['early-image']))
  assert.equal(revealed.find((i) => i.id === 'process:r').hidden, false)
  assert.ok(!revealed.some((i) => i.kind === 'delivery-tool'))
})

controllerTest('model_only resources stay private in process and delivery projection', (controller) => {
  controller.applySnapshot(canonicalSnapshot([
    { kind: 'tool_call', tool_call: { id: 'private', name: 'view_image', arguments: {} } },
    { kind: 'tool_result', tool_result: { call_id: 'private', status: 'success', content: [
      { kind: 'artifact_ref', model_only: true, artifact: { id: 'private-image', size: 10 } },
      { kind: 'text', text: 'audit' },
    ] } },
    { kind: 'artifact_ref', model_only: true, artifact: { id: 'private-body', size: 10 } },
    { kind: 'text', text: 'Canonical final' },
  ]))
  const state = controller.store.get()
  const b = state.blocks.find((b) => b.kind === 'tool')
  assert.deepEqual(toolDelivery(b).media, [])
  const items = projectRenderItems(state.blocks, false, new Set(), {
    closures: state.closures, activeRunIds: new Set(),
  })
  assert.ok(!items.some((i) => i.kind === 'delivery-tool' || (i.kind === 'block' && i.block.kind === 'artifact')))
  assert.ok(items.find((i) => i.kind === 'process-head').members.includes(b))
})

test('moved media and delivery tool anchors restore to original references when expanded', () => {
  const input = [
    owned(tool('write', { toolName: 'write' })),
    owned({ ...block('image'), data: 'i', sourceBodyComplete: true }),
    finalBody(),
    block('user', 'tail'),
  ]
  const collapsed = geometry(processProjection(input), [28, 100, 64, 340, 1000])
  const expanded = geometry(processProjection(input, { expandedRunIds: new Set(['r']) }), [28, 100, 340, 100, 1000])
  // 10px inside the tool delivery and image rows maps to the actual original rows.
  assert.equal(readingAnchorTop(collapsed, expanded, collapsed.offsets[2] + 34, 24, 24, 12, 200), expanded.offsets[1] + 34)
  assert.equal(readingAnchorTop(collapsed, expanded, collapsed.offsets[3] + 34, 24, 24, 12, 200), expanded.offsets[2] + 34)
})

for (const name of ['apply_patch', 'multi_edit', 'MultiEdit', 'write_file']) {
  test(`historical mutation diffArgs ${name} provides review even without a known tool name`, () => {
    const b = owned(tool('history', { toolName: 'historical', diffArgs: { name, args: {} } }))
    assert.equal(isReviewTool(b), true)
    const items = processProjection([b, finalBody()])
    assert.equal(items.at(-1).tool, b)
    assert.equal(items.at(-1).review, true)
    const summary = owned({ ...block('turn_summary'), changes: [{ path: 'file' }] })
    assert.ok(!processProjection([b, finalBody(), summary]).some((i) => i.kind === 'delivery-tool'))
  })
}

test('temporary search/reveal projection never mutates inputs or persists run expansion', () => {
  const input = freezeDeep([owned(tool('review', { toolName: 'apply_patch' })), finalBody(),
    owned(tool('held'), 'reading'), finalBody('reading-final', 'reading', 'rf')])
  const closures = freezeDeep([closure(), closure('reading', 'succeeded', ['rf'])])
  const expandedRunIds = new Set(['reading'])
  const opts = { closures, activeRunIds: new Set(), expandedRunIds }
  const matches = new Set(['review'])
  const before = JSON.stringify(input)
  const searchRuns = revealedProcessRuns(input, matches)
  const temporary = new Set([...expandedRunIds, ...searchRuns])
  const searching = projectRenderItems(input, false, matches, { ...opts, expandedRunIds: temporary })
  assert.equal(searching.find((i) => i.id === 'process:r').hidden, false)
  assertRefs(searching.filter((i) => i.kind !== 'process-head'), input)
  assert.deepEqual([...expandedRunIds], ['reading'])
  assert.deepEqual([...matches], ['review'])
  assert.deepEqual([...searchRuns], ['r'])
  assert.equal(JSON.stringify(input), before)
  const closed = projectRenderItems(input, false, new Set(), opts)
  assert.equal(closed.find((i) => i.id === 'process:r').hidden, true)
  assert.equal(closed.find((i) => i.id === 'process:reading').hidden, false)
  assert.equal(closed.find((i) => i.kind === 'delivery-tool').tool, input[0])
  // Explicit reveal and reading holds still expand independently of search.
  const explicitlyRevealed = projectRenderItems(input, false, matches, opts)
  assert.equal(explicitlyRevealed.find((i) => i.id === 'process:r').hidden, false)
  assert.deepEqual([...expandedRunIds], ['reading'])
})

const controllerProjection = (controller, expandedRunIds = new Set()) => {
  const s = controller.store.get()
  return projectRenderItems(s.blocks, false, new Set(), {
    closures: s.closures,
    activeRunIds: new Set([...s.activeRunIds, ...s.deferredClosureRunIds]),
    unknownActivity: s.unknownActivity,
    expandedRunIds,
  })
}
const processSnapshot = () => canonicalSnapshot([
  { kind: 'tool_call', tool_call: { id: 'call', name: 'read_file', arguments: {} } },
  { kind: 'tool_result', tool_result: { call_id: 'call', status: 'success', content: [] } },
  { kind: 'text', text: 'Canonical final' },
])
const finishEvent = (terminal = closure('r', 'succeeded', ['M']), sequence = 11) => ({
  kind: 'turn.finished', run_id: terminal.run_id, sequence,
  payload: { closure: terminal, changes: [{ path: 'file', created: true }] },
})

controllerTest('legal snapshot watermark handoff replays identical terminal without lifecycle effects', (controller) => {
  const snapshot = processSnapshot()
  snapshot.turn_summaries = [{ run_id: 'r', changes: [{ path: 'file', created: true }] }]
  controller.applySnapshot(freezeDeep(snapshot))
  controller.setFollowing(false)
  const before = { ...controller.store.get() }
  assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head' && i.hidden))
  const event = finishEvent()
  assert.ok(event.sequence > snapshot.event_seq)
  controller.handleEvent(event)
  controller.handleEvent(finishEvent(undefined, 12))
  const after = controller.store.get()
  assert.equal(after.blocks, before.blocks)
  assert.equal(after.closures, before.closures)
  assert.equal(after.following, false)
  assert.equal(after.followSeq, before.followSeq)
  assert.deepEqual(after.readingClosureRunIds, [])
  assert.equal(after.blocks.filter((b) => b.kind === 'turn_summary').length, 1)
  assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head' && i.hidden))
})

controllerTest('live duplicate terminal is idempotent including changes and reading holds', (controller) => {
  controller.handleEvent({ kind: 'tool.prepared', run_id: 'r', payload: { call_id: 'call', tool_name: 'read_file' } })
  controller.handleEvent({ kind: 'tool.completed', run_id: 'r', payload: { call_id: 'call', status: 'success' } })
  controller.handleEvent(completedEvent())
  controller.handleEvent(finishEvent())
  const before = { ...controller.store.get() }
  controller.setFollowing(false)
  controller.handleEvent(finishEvent(undefined, 12))
  assert.equal(controller.store.get().blocks, before.blocks)
  assert.equal(controller.store.get().closures, before.closures)
  assert.equal(controller.store.get().following, false)
  assert.deepEqual(controller.store.get().activeRunIds, [])
  assert.deepEqual(controller.store.get().readingClosureRunIds, [])
  assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head'))
})

for (const conflicting of [closure('r', 'failed', ['M']), closure('r', 'succeeded', ['Other'])]) {
  for (const fromSnapshot of [true, false]) {
    controllerTest(`terminal conflict stays conservative after ${fromSnapshot ? 'snapshot' : 'live'}: ${JSON.stringify(conflicting)}`, (controller) => {
      if (fromSnapshot) controller.applySnapshot(processSnapshot())
      else {
        controller.handleEvent({ kind: 'tool.prepared', run_id: 'r', payload: { call_id: 'call', tool_name: 'read_file' } })
        controller.handleEvent({ kind: 'tool.completed', run_id: 'r', payload: { call_id: 'call', status: 'success' } })
        controller.handleEvent(completedEvent())
        controller.handleEvent(finishEvent())
      }
      const blocks = controller.store.get().blocks
      controller.handleEvent(finishEvent(conflicting))
      controller.handleEvent(finishEvent())
      controller.handleEvent(finishEvent(conflicting))
      assert.equal(controller.store.get().blocks, blocks)
      assert.deepEqual(controller.store.get().closures, [closure('r', 'succeeded', ['M']), conflicting])
      assert.ok(controllerProjection(controller).every((i) => i.kind === 'block'))
      assertRefs(controllerProjection(controller), blocks)
    })
  }
}

controllerTest('old terminal replay never consumes a newer draft or clears its active run', (controller) => {
  controller.applySnapshot(processSnapshot())
  controller.handleEvent({ kind: 'turn.started', payload: { prompt: 'Next' } })
  controller.handleEvent({ kind: 'model.text_delta', run_id: 'next', payload: { delta: 'New draft' } })
  controller.setFollowing(false)
  const before = { ...controller.store.get() }
  controller.handleEvent(finishEvent())
  controller.handleEvent(finishEvent(closure('r', 'failed', ['M'])))
  assert.equal(controller.store.get().blocks, before.blocks)
  assert.deepEqual(controller.store.get().activeRunIds, ['next'])
  assert.equal(controller.store.get().following, false)
  assert.ok(controller.store.get().blocks.some((b) => b.kind === 'stream' && b.runId === 'next'))
})

for (const state of ['running', 'awaiting_approval', 'cancelling', 'booting']) {
  controllerTest(`authoritative snapshot current run has no assistant yet (${state})`, (controller) => {
    const snapshot = processSnapshot()
    snapshot.state = state
    snapshot.run_id = 'next'
    snapshot.messages.push({ id: 'U', role: 'user', metadata: { run_id: 'next' }, parts: [{ kind: 'text', text: 'Next prompt' }] })
    controller.applySnapshot(freezeDeep(snapshot))
    assert.deepEqual(controller.store.get().activeRunIds, ['next'])
    assert.equal(controller.store.get().unknownActivity, false)
    assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head' && i.runId === 'r'))
    const old = controller.store.get().blocks.find((b) => b.sourceMessageId === 'M' && b.kind === 'assistant')
    assert.ok(old.actions)
    controller.handleEvent({ kind: 'model.text_delta', run_id: 'next', payload: { delta: 'Next answer' } })
    controller.handleEvent({ kind: 'model.response_completed', run_id: 'next', payload: { message_id: 'N', text: 'Next answer' } })
    controller.handleEvent(finishEvent(closure('next', 'succeeded', ['N'])))
    assert.deepEqual(controller.store.get().activeRunIds, [])
    assert.equal(controller.store.get().unknownActivity, false)
    assert.equal(controller.store.get().blocks.find((b) => b.id === old.id), old)
    assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head' && i.runId === 'r'))
  })
}

for (const laterUser of [false, true]) {
  controllerTest(`legacy snapshot never identifies a closed assistant as current (later prompt=${laterUser})`, (controller) => {
    const snapshot = processSnapshot()
    snapshot.state = 'running'
    if (laterUser) snapshot.messages.push({ id: 'U', role: 'user', parts: [{ kind: 'text', text: 'Next prompt' }] })
    controller.applySnapshot(snapshot)
    assert.deepEqual(controller.store.get().activeRunIds, [])
    assert.equal(controller.store.get().unknownActivity, true)
    assert.ok(controllerProjection(controller).every((i) => i.kind === 'block'))
    controller.handleEvent({ kind: 'model.response_completed', run_id: 'next', payload: { message_id: 'N', text: 'Next answer' } })
    controller.handleEvent(finishEvent(closure('next', 'succeeded', ['N'])))
    assert.deepEqual(controller.store.get().activeRunIds, [])
    assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head' && i.runId === 'r'))
  })
}

controllerTest('legacy snapshot uses an unclosed assistant run, but not across an unknown new prompt', (controller) => {
  const snapshot = processSnapshot()
  snapshot.state = 'running'
  snapshot.turn_closures = []
  controller.applySnapshot(snapshot)
  assert.deepEqual(controller.store.get().activeRunIds, ['r'])
  assert.equal(controller.store.get().unknownActivity, false)
  controller.handleEvent(finishEvent())
  assert.deepEqual(controller.store.get().activeRunIds, [])
  snapshot.messages.push({ id: 'U', role: 'user', parts: [{ kind: 'text', text: 'Unknown next turn' }] })
  controller.applySnapshot(snapshot)
  assert.deepEqual(controller.store.get().activeRunIds, [])
  assert.equal(controller.store.get().unknownActivity, true)
})

for (const kind of ['goal_continue', 'plan_reconcile']) {
  controllerTest(`internal ${kind} snapshot notice folds, genuine steer stays outside, expansion is lossless`, (controller) => {
    const snapshot = processSnapshot()
    const final = snapshot.messages[0]
    snapshot.messages = [
      { id: 'P', role: 'assistant', status: 'final', metadata: { run_id: 'r' }, parts: [{ kind: 'text', text: 'Intermediate' }] },
      { id: 'I', role: 'user', metadata: { run_id: 'r', kind }, parts: [{ kind: 'text', text: 'Internal continuation' }] },
      { id: 'S', role: 'user', metadata: { run_id: 'r', prompt_kind: 'steer' }, parts: [{ kind: 'text', text: 'Real steer' }] },
      final,
    ]
    controller.applySnapshot(freezeDeep(snapshot))
    const blocks = controller.store.get().blocks
    const internal = blocks.find((b) => b.sourceMessageId === 'I')
    const steer = blocks.find((b) => b.sourceMessageId === 'S')
    assert.equal(internal.kind, 'notice')
    assert.equal(internal.sourceRole, 'user')
    assert.equal(internal.sourcePromptKind, kind)
    assert.equal(steer.kind, 'user')
    const collapsed = controllerProjection(controller)
    assert.ok(collapsed.find((i) => i.kind === 'process-head').members.includes(internal))
    assert.ok(!collapsed.some((i) => i.id === internal.id))
    assert.ok(collapsed.some((i) => i.kind === 'block' && i.block === steer))
    const expanded = controllerProjection(controller, new Set(['r']))
    assertRefs(expanded.filter((i) => i.kind !== 'process-head'), blocks)
    assert.ok(!blocks.some((b) => b.kind === 'turn_summary'))
    controller.applySnapshot({ ...snapshot, state: 'running', run_id: 'r', turn_closures: [] })
    assert.ok(controllerProjection(controller).every((i) => i.kind === 'block'))
  })
}

controllerTest('running snapshot with committed current closure still consumes its first lifecycle terminal', (controller) => {
  const snapshot = processSnapshot()
  snapshot.state = 'running'
  snapshot.run_id = 'r'
  controller.applySnapshot(snapshot)
  assert.deepEqual(controller.store.get().activeRunIds, ['r'])
  controller.handleEvent(finishEvent())
  assert.deepEqual(controller.store.get().activeRunIds, [])
  assert.equal(controller.store.get().closures.length, 1)
  assert.ok(controllerProjection(controller).some((i) => i.kind === 'process-head'))
  const blocks = controller.store.get().blocks
  controller.handleEvent(finishEvent())
  assert.equal(controller.store.get().blocks, blocks)
})

controllerTest('failed terminal followed by success remains conflicting, not last-wins', (controller) => {
  controller.handleEvent(completedEvent())
  controller.handleEvent(finishEvent(closure('r', 'failed', ['M'])))
  controller.handleEvent(finishEvent())
  controller.handleEvent(finishEvent())
  assert.deepEqual(controller.store.get().closures, [closure('r', 'failed', ['M']), closure('r', 'succeeded', ['M'])])
  assert.ok(controllerProjection(controller).every((i) => i.kind === 'block'))
})

controllerTest('legacy unknown current run ignores old closed lifecycle replay until new terminal', (controller) => {
  const snapshot = processSnapshot()
  snapshot.state = 'running'
  controller.applySnapshot(snapshot)
  controller.handleEvent(finishEvent())
  assert.equal(controller.store.get().unknownActivity, true)
  assert.deepEqual(controller.store.get().activeRunIds, [])
  controller.handleEvent({ kind: 'model.text_delta', run_id: 'next', payload: { delta: 'Next' } })
  assert.deepEqual(controller.store.get().activeRunIds, ['next'])
  controller.handleEvent({ kind: 'model.response_completed', run_id: 'next', payload: { message_id: 'N', text: 'Next' } })
  controller.handleEvent(finishEvent(closure('next', 'succeeded', ['N'])))
  assert.equal(controller.store.get().unknownActivity, false)
  assert.deepEqual(controller.store.get().activeRunIds, [])
})

controllerTest('internal prompts do not close the same-run file review before the final answer', (controller) => {
  const snapshot = processSnapshot()
  const final = snapshot.messages[0]
  snapshot.messages = [
    { id: 'P', role: 'assistant', status: 'final', metadata: { run_id: 'r' }, parts: [{ kind: 'text', text: 'Intermediate' }] },
    { id: 'I', role: 'user', metadata: { run_id: 'r', kind: 'plan_reconcile' }, parts: [{ kind: 'text', text: 'Reconcile' }] },
    final,
  ]
  snapshot.turn_summaries = [{ run_id: 'r', changes: [{ path: 'file' }] }]
  controller.applySnapshot(snapshot)
  const blocks = controller.store.get().blocks
  assert.equal(blocks.at(-1).kind, 'turn_summary')
  assert.equal(blocks.filter((b) => b.kind === 'turn_summary').length, 1)
  assert.ok(blocks.find((b) => b.sourceMessageId === 'M' && b.kind === 'assistant').actions)
  assertRefs(controllerProjection(controller, new Set(['r'])).filter((i) => i.kind !== 'process-head'), blocks)
})

controllerTest('unknown user kinds and live steer stay user bubbles, never inferred from text', (controller) => {
  const snapshot = processSnapshot()
  snapshot.messages.unshift({ id: 'U', role: 'user', metadata: { run_id: 'r', kind: 'unknown' }, parts: [{ kind: 'text', text: 'Internal continuation' }] })
  controller.applySnapshot(snapshot)
  assert.ok(controllerProjection(controller).some((i) => i.kind === 'block' && i.block.sourceMessageId === 'U' && i.block.kind === 'user'))
  controller.clear()
  controller.handleEvent({ kind: 'steer.injected', run_id: 'r', payload: { text: 'Internal continuation' } })
  controller.handleEvent(completedEvent())
  controller.handleEvent(finishEvent())
  assert.ok(controllerProjection(controller).some((i) => i.kind === 'block' && i.block.kind === 'user'))
})

test('empty layouts and removed reading rows have no restorable anchor', () => {
  const before = geometry(projectRenderItems([block('assistant', 'old')], false), [1000])
  const after = geometry(projectRenderItems([block('assistant', 'new')], false), [1000])
  const empty = geometry([], [])
  assert.equal(readingAnchorTop(before, after, 34, 24, 24, 12, 200), undefined)
  assert.equal(readingAnchorTop(before, empty, 34, 24, 24, 12, 200), undefined)
  assert.equal(readingAnchorTop(empty, after, 0, 24, 24, 12, 200), undefined)
})
