import test from 'node:test'
import assert from 'node:assert/strict'
import { registerHooks } from 'node:module'
import { globalFieldSpecs } from '../components/settings/spec.ts'
import { getPath, KNOWN_TOP_KEYS, preserveUnmanaged } from '../components/settings/cfgpath.ts'

// Match toolGroups.test.mjs: resolve production bundler-style imports to
// their source files while loading real modules under Node's TS runner.
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
let fillValue, collectValue, collectFields
try {
  ;({ fillValue, collectValue, collectFields } = await import('../components/settings/convert.ts'))
} finally {
  sourceImports.deregister()
}

const specs = globalFieldSpecs()
const webSearchSpec = specs.find((spec) => spec.key === 'web_search.enabled')

test('web search spec is a top-level optional tristate defaulting to 关', () => {
  assert.ok(webSearchSpec)
  assert.equal(webSearchSpec.type, 'tristate')
  assert.equal(webSearchSpec.def, '关')
  assert.ok(!webSearchSpec.required)
  assert.match(`${webSearchSpec.label} ${webSearchSpec.hint}`, /重启/)
  assert.ok(KNOWN_TOP_KEYS.has('web_search'))
})

for (const [name, value, state] of [
  ['absent', undefined, ''],
  ['false', false, 'false'],
  ['true', true, 'true'],
]) {
  test(`web search ${name}: actual spec fill, collection and JSON round trips`, () => {
    assert.ok(webSearchSpec)
    const config = value === undefined ? {} : { web_search: { enabled: value } }
    const loaded = JSON.parse(JSON.stringify(config))
    assert.equal(fillValue(webSearchSpec, getPath(loaded, webSearchSpec.key)), state)
    const collected = {}
    collectValue(webSearchSpec, state, collected)
    assert.deepEqual(collected, config)
    assert.equal(getPath(collected, 'tools.web_search.enabled'), undefined)
    const serialized = JSON.parse(JSON.stringify(collected))
    assert.deepEqual(serialized, config)
    assert.equal(fillValue(webSearchSpec, getPath(serialized, webSearchSpec.key)), state)
    const recollected = {}
    collectFields([webSearchSpec], { [webSearchSpec.key]: state }, recollected)
    preserveUnmanaged(recollected, loaded)
    assert.deepEqual(JSON.parse(JSON.stringify(recollected)), config)
    if (value === undefined) {
      assert.ok(!Object.hasOwn(collected, 'web_search'), 'Default off must omit the override')
    } else {
      assert.equal(typeof serialized.web_search.enabled, 'boolean')
    }
  })
}

for (const enabled of [false, true]) {
  test(`reset web search ${enabled} to default does not preserve the old override`, () => {
    const original = {
      web_search: { enabled },
      future_section: { keep: true },
      ui: { keymap: 'vim' },
    }
    const before = JSON.stringify(original)
    const reset = {}
    collectFields(specs, { 'web_search.enabled': '' }, reset)
    preserveUnmanaged(reset, original)
    assert.ok(!Object.hasOwn(reset, 'web_search'))
    assert.equal(getPath(reset, 'tools.web_search.enabled'), undefined)
    assert.deepEqual(reset.future_section, original.future_section)
    assert.equal(getPath(reset, 'ui.keymap'), 'vim')
    assert.equal(JSON.stringify(original), before, 'Preservation must not mutate the baseline')
    const serialized = JSON.parse(JSON.stringify(reset))
    assert.ok(!Object.hasOwn(serialized, 'web_search'))
    assert.equal(fillValue(webSearchSpec, getPath(serialized, webSearchSpec.key)), '')
  })
}
