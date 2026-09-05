// SettingsPanel.tsx — Settings panel (graphical editor for config.yaml).
// Fields are described by a declarative spec (key is the key path in config.yaml); the same spec
// drives rendering and collection — adding a new config option only takes one line of spec (see spec.ts).
// See draft.ts for the draft model; see convert.ts for load fill / save collect.

import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import type { AppController } from '../../app/controller'
import { ApiError } from '../../protocol/api'
import type { PutConfigResult, SecretRef } from '../../protocol/types'
import { useStore } from '../../store/store'
import { toast } from '../ui/Toast'
import { confirmDialog, isConfirmOpen } from '../ui/Confirm'
import { Icon, type IconName } from '../../lib/icons'
import { getPath, setPath, preserveUnmanaged } from './cfgpath'
import {
  TABS,
  globalFieldSpecs,
  PROVIDER_BASE_FIELDS,
  PROVIDER_ADV_FIELDS,
  MODEL_FIELDS,
  MCP_STDIO_FIELDS,
  MCP_HTTP_FIELDS,
  MCP_COMMON_FIELDS,
  SKILLS_CONFIG_FIELDS,
  DEFAULT_MODEL_FIELD,
  type FieldSpec,
} from './spec'
import { fillValue, collectValue, collectFields, type ControlState } from './convert'
import type { CardDraft, McpDraft, McpTransport, ProviderDraft, SettingsDraft } from './draft'
import { draftId } from './draft'
import { SetLoading } from './controls'
import { ProvidersTab } from './ProvidersTab'
import { McpTab } from './McpTab'
import { SkillsTab } from './SkillsTab'
import { SectionsTab, SystemExtras } from './SectionsTab'
import { RulePacks } from './RulePacks'

// --- Panel-shared context (reveal / dirty flag / validation highlight) ---

export interface SettingsCtx {
  reveal: (ref: SecretRef) => Promise<string | null>
  markDirty: () => void
  invalid: string | null // currently highlighted field (locator id)
  clearInvalid: () => void
}

const Ctx = createContext<SettingsCtx>({
  reveal: () => Promise.resolve(null),
  markDirty: () => {},
  invalid: null,
  clearInvalid: () => {},
})

export function useSettingsCtx(): SettingsCtx {
  return useContext(Ctx)
}

// --- Location info for a validation failure ---

interface InvalidTarget {
  msg: string
  tab: string
  fieldId: string // control locator id (globals: spec.key; cards: cardId:key)
  providerCardId?: string // card that must enter detail state
  modelCardId?: string
  mcpCardId?: string
}

const GLOBAL_SPECS = globalFieldSpecs()

// Initial control state of spec.key in globals
function emptyState(spec: FieldSpec): ControlState {
  return spec.type === 'bool' || spec.type === 'flag-list' ? false : ''
}

export function SettingsPanel({ controller }: { controller: AppController }) {
  const open = useStore(controller.store, (s) => s.settingsOpen)
  const [draft, setDraft] = useState<SettingsDraft>({
    globals: {},
    providers: [],
    mcpServers: [],
    workspaces: [],
  })
  const [revision, setRevision] = useState('')
  const [origCfg, setOrigCfg] = useState<Record<string, unknown>>({})
  const [cfgPath, setCfgPath] = useState('')
  const [dirty, setDirty] = useState(false)
  const [activeTab, setActiveTab] = useState('providers')
  const [msg, setMsg] = useState('')
  const [msgErr, setMsgErr] = useState(false)
  const [saving, setSaving] = useState(false)
  const [loading, setLoading] = useState(false)
  const [loadErr, setLoadErr] = useState('')
  const [loaded, setLoaded] = useState(false)
  const [invalid, setInvalid] = useState<string | null>(null)
  const [flashSave, setFlashSave] = useState(false)
  // Green flash on successful save fades after 1.3s; the timer is cleaned up on unmount/rapid re-trigger.
  useEffect(() => {
    if (!flashSave) return
    const t = setTimeout(() => setFlashSave(false), 1300)
    return () => clearTimeout(t)
  }, [flashSave])
  // Hierarchical navigation: providers three levels / mcp two levels
  const [openProviderId, setOpenProviderId] = useState<string | null>(null)
  const [openModelId, setOpenModelId] = useState<string | null>(null)
  const [openMcpId, setOpenMcpId] = useState<string | null>(null)
  // MCP status refresh token (re-fetch badges after save); env report invalidation token (path_extra change)
  const [mcpStatusToken, setMcpStatusToken] = useState(0)
  const [envReloadToken, setEnvReloadToken] = useState(0)
  const closingRef = useRef(false)

  const showMsg = useCallback((text: string, isError = false) => {
    setMsg(text)
    setMsgErr(isError)
  }, [])

  const markDirty = useCallback(() => {
    setDirty(true)
    setInvalid(null)
  }, [])

  // Fetch the plaintext of a stored secret on demand; failures are already toasted, returns null.
  const reveal = useCallback(
    async (ref: SecretRef): Promise<string | null> => {
      if (!ref || (ref.name === '' && ref.kind !== 'tracing')) {
        toast('先填写名称并保存配置后才能查看')
        return null
      }
      try {
        const r = await controller.api.revealSecret(ref)
        return r.value || ''
      } catch (e) {
        const err = e as ApiError
        if (err.status === 401) return null
        toast(
          err.status === 404
            ? '该位置没有已保存的密钥（先保存配置）'
            : '查看密钥失败: ' + err.message,
        )
        return null
      }
    },
    [controller],
  )

  // --- Load (fill) ---

  const buildDraft = useCallback((config: Record<string, unknown>): SettingsDraft => {
    const globals: Record<string, ControlState> = {}
    for (const spec of GLOBAL_SPECS) {
      globals[spec.key] = fillValue(spec, getPath(config, spec.key))
    }
    const providers: ProviderDraft[] = ((config.providers as Record<string, unknown>[]) || []).map(
      (p) => {
        const fields: Record<string, ControlState> = {}
        for (const spec of [...PROVIDER_BASE_FIELDS, ...PROVIDER_ADV_FIELDS, { key: 'name' }]) {
          fields[spec.key] = fillValue(spec, p[spec.key])
        }
        return {
          id: draftId(),
          fields,
          models: ((p.models as Record<string, unknown>[]) || []).map((m) => {
            const mf: Record<string, ControlState> = {}
            for (const spec of MODEL_FIELDS) mf[spec.key] = fillValue(spec, m[spec.key])
            return { id: draftId(), fields: mf }
          }),
        }
      },
    )
    const mcpServers: McpDraft[] = Object.entries(
      (config.mcp_servers as Record<string, Record<string, unknown>>) || {},
    ).map(([name, srv]) => {
      const fill = (specs: FieldSpec[]) => {
        const o: Record<string, ControlState> = {}
        for (const spec of specs) o[spec.key] = fillValue(spec, srv[spec.key])
        return o
      }
      return {
        id: draftId(),
        name,
        transport: (srv.url ? 'http' : 'stdio') as McpTransport,
        stdio: fill(MCP_STDIO_FIELDS),
        http: fill(MCP_HTTP_FIELDS),
        common: fill(MCP_COMMON_FIELDS),
      }
    })
    const workspaces: CardDraft[] = ((config.workspaces as Record<string, unknown>[]) || []).map(
      (ws) => ({
        id: draftId(),
        fields: {
          name: fillValue({ key: 'name' }, ws.name),
          root: fillValue({ key: 'root' }, ws.root),
        },
      }),
    )
    return { globals, providers, mcpServers, workspaces }
  }, [])

  // When manual=true (the "Reload" button): unsaved changes need confirmation (reload discards them),
  // and a toast reports the result — revision comparison distinguishes "already up to date" from "reloaded".
  // Skip rebuilding the whole draft when revision is unchanged and the reload doesn't discard changes:
  // scroll position, detail level, and expansion state are all preserved.
  const load = useCallback(
    async (manual = false) => {
      if (manual && dirty) {
        const ok = await confirmDialog({
          title: '重新加载',
          body: '设置中有未保存的修改，重新加载将丢弃它们。',
          okLabel: '重新加载',
        })
        if (!ok) return
      }
      showMsg('加载中…')
      if (!loaded) {
        setLoading(true)
        setLoadErr('')
      }
      try {
        const prevRevision = revision
        const wasDirty = dirty
        const r = await controller.api.getConfig()
        setRevision(r.revision || '')
        setOrigCfg(r.config || {})
        setCfgPath(r.exists ? r.path : `${r.path}（尚未创建，保存后生成）`)
        setDirty(false)
        showMsg(r.exists ? '' : '首次配置：请先在「模型」页添加至少一个 provider')
        // Exception for a dirty-state reload — the user confirmed discarding changes, so rebuild to restore server values
        if (!loaded || wasDirty || r.revision !== prevRevision) {
          setDraft(buildDraft(r.config || {}))
          setLoaded(true)
        } else {
          // Config unchanged but runtime state may have changed (external reconnects, etc.): refresh MCP badges
          setMcpStatusToken((t) => t + 1)
        }
        if (manual) {
          toast(r.revision === prevRevision ? '已是最新，配置无变化' : '配置已重新加载', true)
        }
      } catch (e) {
        const err = e as ApiError
        if (err.status === 401) {
          controller.closeSettings() // the gate is about to pop up; the panel yields
          return
        }
        showMsg('加载配置失败: ' + err.message, true)
        if (!loaded) setLoadErr('加载配置失败: ' + err.message)
        if (manual) toast('加载配置失败: ' + err.message)
      } finally {
        setLoading(false)
      }
    },
    [controller, dirty, loaded, revision, buildDraft, showMsg],
  )

  // Load as soon as the panel opens
  useEffect(() => {
    if (open) void load()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open])

  // --- Close (confirm when dirty) ---

  const close = useCallback(async () => {
    if (!open || closingRef.current) return
    if (dirty) {
      closingRef.current = true // re-entry guard: Esc/× re-triggering while the confirm is pending
      try {
        const ok = await confirmDialog({
          title: '放弃修改',
          body: '设置中有未保存的修改，关闭后将丢失。',
          okLabel: '放弃修改',
        })
        if (!ok) return
      } finally {
        closingRef.current = false
      }
    }
    setDirty(false)
    controller.closeSettings()
  }, [open, dirty, controller])

  // Esc closes (confirm when dirty); while the confirm dialog is open it consumes Esc itself, avoiding duplicate dialogs.
  useEffect(() => {
    if (!open) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      if (isConfirmOpen()) return
      e.stopPropagation()
      void close()
    }
    document.addEventListener('keydown', onKey, true)
    return () => document.removeEventListener('keydown', onKey, true)
  }, [open, close])

  // --- Draft editing helpers ---

  const setGlobal = useCallback(
    (key: string, v: ControlState) => {
      setDraft((d) => ({ ...d, globals: { ...d.globals, [key]: v } }))
      markDirty()
    },
    [markDirty],
  )

  // --- Validation (same rules as the old _firstInvalid, reading the draft directly) ---

  const firstInvalid = useCallback((): InvalidTarget | null => {
    let namedProviders = 0
    for (const card of draft.providers) {
      const p: Record<string, unknown> = {}
      collectFields(
        [...PROVIDER_BASE_FIELDS, ...PROVIDER_ADV_FIELDS, { key: 'name' }],
        card.fields,
        p,
      )
      if (!p.name) {
        if (Object.keys(p).length || card.models.length) {
          return {
            msg: '有 provider 缺少名称',
            tab: 'providers',
            fieldId: card.id + ':name',
            providerCardId: card.id,
          }
        }
        continue
      }
      namedProviders++
      if (!p.base_url) {
        return {
          msg: `provider「${p.name}」缺少 Base URL`,
          tab: 'providers',
          fieldId: card.id + ':base_url',
          providerCardId: card.id,
        }
      }
      if (p.api_key && p.api_key_env) {
        return {
          msg: `provider「${p.name}」的 API Key 与 Key 环境变量只能二选一`,
          tab: 'providers',
          fieldId: card.id + ':api_key_env',
          providerCardId: card.id,
        }
      }
      let namedModels = 0
      for (const mc of card.models) {
        const m: Record<string, unknown> = {}
        collectFields(MODEL_FIELDS, mc.fields, m)
        if (m.name) namedModels++
        else if (Object.keys(m).length) {
          return {
            msg: `provider「${p.name}」下有模型缺少名称`,
            tab: 'providers',
            fieldId: mc.id + ':name',
            providerCardId: card.id,
            modelCardId: mc.id,
          }
        }
      }
      if (!namedModels) {
        return {
          msg: `provider「${p.name}」至少需要一个模型`,
          tab: 'providers',
          fieldId: card.id + ':add-model',
          providerCardId: card.id,
        }
      }
    }
    if (!namedProviders) {
      return {
        msg: '请先在「模型」页添加至少一个 provider',
        tab: 'providers',
        fieldId: 'add-provider',
      }
    }
    for (const card of draft.mcpServers) {
      const srv: Record<string, unknown> = {}
      collectFields(MCP_COMMON_FIELDS, card.common, srv)
      collectFields(
        card.transport === 'http' ? MCP_HTTP_FIELDS : MCP_STDIO_FIELDS,
        card.transport === 'http' ? card.http : card.stdio,
        srv,
      )
      const name = card.name.trim()
      if (!name) {
        if (Object.keys(srv).length) {
          return {
            msg: '有 MCP 服务器缺少名称',
            tab: 'mcp',
            fieldId: card.id + ':name',
            mcpCardId: card.id,
          }
        }
        continue
      }
      if (card.transport === 'stdio' && !srv.command) {
        return {
          msg: `MCP 服务器「${name}」缺少命令`,
          tab: 'mcp',
          fieldId: card.id + ':command',
          mcpCardId: card.id,
        }
      }
      if (card.transport === 'http' && !srv.url) {
        return {
          msg: `MCP 服务器「${name}」缺少 URL`,
          tab: 'mcp',
          fieldId: card.id + ':url',
          mcpCardId: card.id,
        }
      }
    }
    for (const card of draft.workspaces) {
      const ws: Record<string, unknown> = {}
      collectFields([{ key: 'name' }, { key: 'root' }], card.fields, ws)
      if (!ws.root && ws.name) {
        return {
          msg: `工作区「${ws.name}」缺少根目录`,
          tab: 'system',
          fieldId: card.id + ':root',
        }
      }
    }
    return null
  }, [draft])

  // Locate the field that failed validation: switch tab → enter card detail state → scroll into view and focus → highlight.
  const locate = useCallback(
    (target: InvalidTarget) => {
      if (target.tab !== activeTab) setActiveTab(target.tab)
      if (target.providerCardId) {
        setOpenModelId(null)
        setOpenProviderId(target.providerCardId)
      }
      if (target.modelCardId) setOpenModelId(target.modelCardId)
      if (target.mcpCardId) setOpenMcpId(target.mcpCardId)
      setInvalid(target.fieldId)
      requestAnimationFrame(() => {
        const el = document.querySelector(`[data-field-id="${CSS.escape(target.fieldId)}"]`)
        const ctl = el?.querySelector('input, textarea, button.sel') as HTMLElement | null
        ;(ctl || (el as HTMLElement | null))?.scrollIntoView({
          block: 'center',
          behavior: 'smooth',
        })
        if (ctl && 'focus' in ctl) (ctl as HTMLElement).focus({ preventScroll: true })
      })
    },
    [activeTab],
  )

  // --- Save ---

  // Save result message: reports when each class of config takes effect, per the tiered report returned by the server.
  const applyMsg = (resp: PutConfigResult): string => {
    if (resp.apply_error) return `已保存，但热应用失败（重启后生效）: ${resp.apply_error}`
    const a = resp.applied
    if (!a) return '已保存'
    const parts: string[] = []
    if (a.immediate && a.immediate.length) parts.push('立即生效: ' + a.immediate.join('、'))
    if (a.next_turn && a.next_turn.length) parts.push('下一轮生效: ' + a.next_turn.join('、'))
    if (a.restart && a.restart.length) parts.push('重启后生效: ' + a.restart.join('、'))
    return parts.length ? '已保存 — ' + parts.join('；') : '已保存（配置无变化）'
  }

  const save = useCallback(async () => {
    if (saving) return // double-click/rapid-click guard: a repeated PUT carries the old revision and inevitably 409s
    setSaving(true)
    try {
      // Validate before collecting: failures can be located to a specific control
      const bad = firstInvalid()
      if (bad) {
        locate(bad)
        showMsg(bad.msg, true)
        toast(bad.msg)
        return
      }
      const cfg: Record<string, unknown> = {}
      let skippedCards = 0
      // Fields in the global scope
      for (const spec of GLOBAL_SPECS) {
        collectValue(spec, draft.globals[spec.key] ?? emptyState(spec), cfg)
      }
      // providers
      const providers: Record<string, unknown>[] = []
      for (const card of draft.providers) {
        const p: Record<string, unknown> = {}
        collectFields(
          [...PROVIDER_BASE_FIELDS, ...PROVIDER_ADV_FIELDS, { key: 'name' }],
          card.fields,
          p,
        )
        const models: Record<string, unknown>[] = []
        for (const mc of card.models) {
          const m: Record<string, unknown> = {}
          collectFields(MODEL_FIELDS, mc.fields, m)
          if (m.name) models.push(m)
          else if (Object.keys(m).length) skippedCards++
        }
        if (models.length) p.models = models
        if (p.name) providers.push(p)
        else if (Object.keys(p).length) skippedCards++
      }
      if (providers.length) cfg.providers = providers
      // mcp_servers
      const servers: Record<string, unknown> = {}
      for (const card of draft.mcpServers) {
        const name = card.name.trim()
        if (!name) continue
        const srv: Record<string, unknown> = {}
        collectFields(MCP_COMMON_FIELDS, card.common, srv)
        collectFields(
          card.transport === 'http' ? MCP_HTTP_FIELDS : MCP_STDIO_FIELDS,
          card.transport === 'http' ? card.http : card.stdio,
          srv,
        )
        if (servers[name]) skippedCards++ // duplicate name: the later one overwrites the earlier
        servers[name] = srv
      }
      if (Object.keys(servers).length) cfg.mcp_servers = servers
      // workspaces
      const wss: Record<string, unknown>[] = []
      for (const card of draft.workspaces) {
        const ws: Record<string, unknown> = {}
        collectFields([{ key: 'name' }, { key: 'root' }], card.fields, ws)
        if (ws.root) wss.push(ws)
        else if (ws.name) skippedCards++
      }
      if (wss.length) cfg.workspaces = wss
      if (skippedCards > 0) {
        toast(`${skippedCards} 张卡片因缺少必填字段（名称/根目录）未被保存`)
      }

      preserveUnmanaged(cfg, origCfg)
      showMsg('保存中…（MCP 变更需连接，可能耗时数秒）')
      const r = await controller.api.putConfig(revision, cfg)
      setRevision(r.revision || revision)
      // Only a path_extra change rewrites the PATH report: invalidate the environment card precisely by key
      const pathExtraChanged =
        JSON.stringify(getPath(cfg, 'tools.path_extra') ?? null) !==
        JSON.stringify(getPath(origCfg, 'tools.path_extra') ?? null)
      setOrigCfg(cfg) // the successfully saved config becomes the baseline for later preserve/compare
      setDirty(false)
      const text = applyMsg(r)
      showMsg(text)
      toast(text, true)
      // Green outline flashes on success (echoing the warning-colored outline of is-dirty);
      // the fade is held by the timer in the effect below (cleaned up on unmount/rapid re-trigger, no dangling setState)
      setFlashSave(true)
      // Hot-apply may change MCP connection state (add/remove/reconnect); refresh badges
      setMcpStatusToken((t) => t + 1)
      // Notify the caller to refresh config-dependent UI (model catalog, picker badges, attachment gating)
      void controller.refreshModelCatalog()
      if (pathExtraChanged) setEnvReloadToken((t) => t + 1)
    } catch (e) {
      const err = e as ApiError
      if (err.status === 401) return
      if (err.code === 'config_conflict') {
        showMsg('配置文件已被外部修改 — 点击「重新加载」后再保存', true)
      } else {
        showMsg('保存失败: ' + err.message, true)
      }
    } finally {
      setSaving(false)
    }
  }, [saving, firstInvalid, locate, showMsg, draft, origCfg, revision, controller])

  const ctx = useMemo<SettingsCtx>(
    () => ({ reveal, markDirty, invalid, clearInvalid: () => setInvalid(null) }),
    [reveal, markDirty, invalid],
  )

  if (!open) return null

  // Mount on demand: only render the activeTab's panel. Previously all tabs stayed mounted, toggled via
  // hidden — any keystroke fully re-rendered all four Tab subtrees (including each card's collectFields/
  // summary computation). Form values are hosted in the parent's draft and survive unmount; in-tab
  // expansion state (openProviderId etc.) also lives in the parent. Validation/save are full-data operations, independent of mounting.
  const activeSpec = TABS.find((t) => t.id === activeTab)

  return (
    <Ctx.Provider value={ctx}>
      <div
        id="settings-wrap"
        className="settings-wrap"
        onClick={(e) => {
          if (e.target === e.currentTarget) void close()
        }}
      >
        <div className="settings" role="dialog" aria-modal="true" aria-labelledby="settings-title">
          <header className="settings-head">
            <span className="settings-title" id="settings-title">
              设置
            </span>
            <span id="settings-path" className="settings-path mono" title="配置文件路径">
              {cfgPath}
            </span>
            <span className="spacer" />
            <button
              id="settings-close"
              className="icon-btn"
              title="关闭 (Esc)"
              onClick={() => void close()}
            >
              <Icon name="xmark" />
            </button>
          </header>
          <div className="settings-body">
            <nav id="settings-tabs" className="settings-tabs" aria-label="设置分类">
              {TABS.map((t) => (
                <button
                  key={t.id}
                  type="button"
                  className={'settings-tab' + (t.id === activeTab ? ' is-active' : '')}
                  onClick={() => setActiveTab(t.id)}
                >
                  <Icon name={t.icon as IconName} />
                  {t.label}
                </button>
              ))}
            </nav>
            <div id="settings-content" className="settings-content">
              {!loaded && (loading || loadErr) ? (
                <SetLoading text={loadErr || '加载配置中…'} isError={!!loadErr} />
              ) : activeSpec ? (
                <div
                  className={
                    'settings-panel' +
                    (activeSpec.id === 'providers' || activeSpec.id === 'mcp' ? ' set-hier' : '') +
                    (activeSpec.id === 'providers' && openProviderId ? ' is-detail' : '') +
                    (activeSpec.id === 'mcp' && openMcpId ? ' is-detail' : '')
                  }
                  data-tab-id={activeSpec.id}
                >
                  {activeSpec.id === 'providers' && (
                    <ProvidersTab
                      draft={draft}
                      setDraft={setDraft}
                      openProviderId={openProviderId}
                      openModelId={openModelId}
                      setOpenProviderId={setOpenProviderId}
                      setOpenModelId={setOpenModelId}
                      setGlobal={setGlobal}
                    />
                  )}
                  {activeSpec.id === 'mcp' && (
                    <McpTab
                      draft={draft}
                      setDraft={setDraft}
                      openMcpId={openMcpId}
                      setOpenMcpId={setOpenMcpId}
                      controller={controller}
                      statusToken={mcpStatusToken}
                    />
                  )}
                  {activeSpec.id === 'skills' && (
                    <SkillsTab
                      draft={draft}
                      setGlobal={setGlobal}
                      controller={controller}
                      active={activeTab === 'skills'}
                      onDisabledChanged={(rev, disabled) => {
                        // The endpoint rewrote the config file: sync the revision and the disabled
                        // list held by the panel, otherwise a later settings save will 409-conflict,
                        // or roll skills.disabled back to stale values
                        if (rev) setRevision(rev)
                        setOrigCfg((prev) => {
                          const next = { ...prev }
                          setPath(next, 'skills.disabled', disabled)
                          return next
                        })
                      }}
                    />
                  )}
                  {activeSpec.sections && (
                    <SectionsTab
                      tab={activeSpec}
                      draft={draft}
                      setGlobal={setGlobal}
                      extras={
                        activeSpec.id === 'system' ? (
                          <SystemExtras
                            draft={draft}
                            setDraft={setDraft}
                            controller={controller}
                            active={activeTab === 'system'}
                            envReloadToken={envReloadToken}
                          />
                        ) : activeSpec.id === 'permission' ? (
                          <RulePacks controller={controller} active={activeTab === 'permission'} />
                        ) : null
                      }
                    />
                  )}
                </div>
              ) : null}
            </div>
          </div>
          <footer className="settings-foot">
            <span className="spacer" />
            <span
              id="settings-msg"
              className={'settings-msg' + (msgErr ? ' is-error' : '')}
              title={msg}
            >
              {msg}
            </span>
            <button
              id="settings-reload"
              className="btn btn-secondary"
              type="button"
              onClick={() => void load(true)}
            >
              重新加载
            </button>
            <button
              id="settings-close-foot"
              className="btn btn-secondary"
              type="button"
              onClick={() => void close()}
            >
              关闭
            </button>
            <button
              id="settings-save"
              className={
                'btn btn-primary' + (dirty ? ' is-dirty' : '') + (flashSave ? ' flash-success' : '')
              }
              type="button"
              disabled={saving}
              onClick={() => void save()}
            >
              保存
            </button>
          </footer>
        </div>
      </div>
    </Ctx.Provider>
  )
}

// Type re-exports shared by the tab components
export type { SettingsDraft, ProviderDraft, McpDraft, CardDraft, ControlState, FieldSpec }
export { DEFAULT_MODEL_FIELD, SKILLS_CONFIG_FIELDS, draftId, setPath }
