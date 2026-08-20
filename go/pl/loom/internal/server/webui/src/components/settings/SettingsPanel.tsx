// SettingsPanel.tsx — 设置面板（config.yaml 的图形化编辑）。
// 字段用声明式 spec 描述（key 即 config.yaml 的键路径），同一套 spec
// 驱动渲染与收集 —— 新增配置项只需加一行 spec（见 spec.ts）。
// 草稿模型见 draft.ts；加载 fill / 保存 collect 见 convert.ts。

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

// --- 面板共享上下文（reveal / 脏标记 / 校验标红） ---

export interface SettingsCtx {
  reveal: (ref: SecretRef) => Promise<string | null>
  markDirty: () => void
  invalid: string | null // 当前标红字段（定位 id）
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

// --- 校验失败的定位信息 ---

interface InvalidTarget {
  msg: string
  tab: string
  fieldId: string // 控件定位 id（globals: spec.key；卡片: cardId:key）
  providerCardId?: string // 需要进入详情态的卡片
  modelCardId?: string
  mcpCardId?: string
}

const GLOBAL_SPECS = globalFieldSpecs()

// spec.key 在 globals 里的初始控件态
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
  // 层级导航：providers 三级 / mcp 两级
  const [openProviderId, setOpenProviderId] = useState<string | null>(null)
  const [openModelId, setOpenModelId] = useState<string | null>(null)
  const [openMcpId, setOpenMcpId] = useState<string | null>(null)
  // MCP 状态刷新令牌（保存后重拉徽标）；env 报告失效令牌（path_extra 变更）
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

  // 按需取回一个已存密钥的明文；失败已 toast，返回 null。
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

  // --- 加载（fill） ---

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

  // manual=true（「重新加载」按钮）时：未保存修改需确认（重载会丢弃），
  // 完成后 toast 反馈——对比 revision 区分「已是最新」与「已重新加载」。
  // revision 未变且非放弃修改的重载时跳过整棵草稿重建：滚动位置、详情
  // 层级、展开态全部保留。
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
        // 脏状态重载例外——用户已确认放弃修改，必须重建以恢复服务端值
        if (!loaded || wasDirty || r.revision !== prevRevision) {
          setDraft(buildDraft(r.config || {}))
          setLoaded(true)
        } else {
          // 配置没变但运行态可能变了（外部重连等）：刷新 MCP 徽标
          setMcpStatusToken((t) => t + 1)
        }
        if (manual) {
          toast(r.revision === prevRevision ? '已是最新，配置无变化' : '配置已重新加载', true)
        }
      } catch (e) {
        const err = e as ApiError
        if (err.status === 401) {
          controller.closeSettings() // gate 即将弹出，面板让位
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

  // 打开面板即加载
  useEffect(() => {
    if (open) void load()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open])

  // --- 关闭（脏时确认） ---

  const close = useCallback(async () => {
    if (!open || closingRef.current) return
    if (dirty) {
      closingRef.current = true // 重入守卫：Esc/× 在 confirm 等待期间再次触发
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

  // Esc 关闭（脏时确认）；确认弹窗开着时由它自己消费 Esc，避免重复弹窗。
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

  // --- 草稿编辑助手 ---

  const setGlobal = useCallback(
    (key: string, v: ControlState) => {
      setDraft((d) => ({ ...d, globals: { ...d.globals, [key]: v } }))
      markDirty()
    },
    [markDirty],
  )

  // --- 校验（与旧 _firstInvalid 同规则，直接读草稿） ---

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

  // 定位到校验失败的字段：切 tab → 进入卡片详情态 → 滚动到可视区并聚焦 → 标红。
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

  // --- 保存 ---

  // 保存结果消息：按服务端返回的分级报告说明每类配置的生效时机。
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
    if (saving) return // 双击/连点保护：重复 PUT 会带旧 revision 必然 409
    setSaving(true)
    try {
      // 先校验后收集：失败可定位到具体控件
      const bad = firstInvalid()
      if (bad) {
        locate(bad)
        showMsg(bad.msg, true)
        toast(bad.msg)
        return
      }
      const cfg: Record<string, unknown> = {}
      let skippedCards = 0
      // 全局 scope 字段
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
        if (servers[name]) skippedCards++ // 重名：后者覆盖前者
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
      // path_extra 变化才会重写 PATH 报告：按 key 精确失效环境卡片
      const pathExtraChanged =
        JSON.stringify(getPath(cfg, 'tools.path_extra') ?? null) !==
        JSON.stringify(getPath(origCfg, 'tools.path_extra') ?? null)
      setOrigCfg(cfg) // 保存成功的配置成为后续 preserve/比较 的基准
      setDirty(false)
      const text = applyMsg(r)
      showMsg(text)
      toast(text, true)
      // 成功瞬间绿色 outline 一闪（与 is-dirty 的 warning 色 outline 呼应）
      setFlashSave(true)
      setTimeout(() => setFlashSave(false), 1300)
      // 热应用可能改变 MCP 连接状态（新增/删除/重连），刷新徽标
      setMcpStatusToken((t) => t + 1)
      // 通知调用方刷新依赖配置的 UI（模型目录、picker 角标、附件门控）
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
              ) : (
                TABS.map((tab) => (
                  <div
                    key={tab.id}
                    className={
                      'settings-panel' +
                      (tab.id === 'providers' || tab.id === 'mcp' ? ' set-hier' : '') +
                      (tab.id === 'providers' && openProviderId ? ' is-detail' : '') +
                      (tab.id === 'mcp' && openMcpId ? ' is-detail' : '')
                    }
                    data-tab-id={tab.id}
                    hidden={tab.id !== activeTab}
                  >
                    {tab.id === 'providers' && (
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
                    {tab.id === 'mcp' && (
                      <McpTab
                        draft={draft}
                        setDraft={setDraft}
                        openMcpId={openMcpId}
                        setOpenMcpId={setOpenMcpId}
                        controller={controller}
                        statusToken={mcpStatusToken}
                      />
                    )}
                    {tab.id === 'skills' && (
                      <SkillsTab
                        draft={draft}
                        setGlobal={setGlobal}
                        controller={controller}
                        active={activeTab === 'skills'}
                        onDisabledChanged={(rev, disabled) => {
                          // 端点改写了 config 文件：同步面板持有的 revision 与
                          // disabled 列表，否则后续保存设置会 409 冲突，或把
                          // skills.disabled 回滚成旧值
                          if (rev) setRevision(rev)
                          setOrigCfg((prev) => {
                            const next = { ...prev }
                            setPath(next, 'skills.disabled', disabled)
                            return next
                          })
                        }}
                      />
                    )}
                    {tab.sections && (
                      <SectionsTab
                        tab={tab}
                        draft={draft}
                        setGlobal={setGlobal}
                        extras={
                          tab.id === 'system' ? (
                            <SystemExtras
                              draft={draft}
                              setDraft={setDraft}
                              controller={controller}
                              active={activeTab === 'system'}
                              envReloadToken={envReloadToken}
                            />
                          ) : tab.id === 'permission' ? (
                            <RulePacks
                              controller={controller}
                              active={activeTab === 'permission'}
                            />
                          ) : null
                        }
                      />
                    )}
                  </div>
                ))
              )}
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

// 供 tab 组件复用的类型 re-export
export type { SettingsDraft, ProviderDraft, McpDraft, CardDraft, ControlState, FieldSpec }
export { DEFAULT_MODEL_FIELD, SKILLS_CONFIG_FIELDS, draftId, setPath }
