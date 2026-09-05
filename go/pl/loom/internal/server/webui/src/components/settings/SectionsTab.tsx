// SectionsTab.tsx — simple tabs (spec-driven section rendering) + system tab extras
// (dev-environment runtime report + pre-registered workspace cards).

import { useCallback, useEffect, useState, type ReactNode } from 'react'
import type { AppController } from '../../app/controller'
import type { EnvironmentReport } from '../../protocol/types'
import { ApiError } from '../../protocol/api'
import type { SettingsDraft, ControlState, FieldSpec, CardDraft } from './SettingsPanel'
import { draftId } from './draft'
import type { TabSpec } from './spec'
import { FieldRow, SetSection, SetTip, SetLoading, CardDelBtn } from './controls'
import { useSettingsCtx } from './SettingsPanel'
import { Icon } from '../../lib/icons'
import { toast } from '../ui/Toast'

// SectionsTab — a simple tab: all fields belong to the global scope.
export function SectionsTab({
  tab,
  draft,
  setGlobal,
  extras,
}: {
  tab: TabSpec
  draft: SettingsDraft
  setGlobal: (key: string, v: ControlState) => void
  extras?: ReactNode
}) {
  const { invalid } = useSettingsCtx()
  const renderField = (spec: FieldSpec) => (
    <div key={spec.key} data-field-id={spec.key}>
      <FieldRow
        spec={spec}
        value={
          draft.globals[spec.key] ??
          (spec.type === 'bool' || spec.type === 'flag-list' ? false : '')
        }
        onChange={(v) => setGlobal(spec.key, v)}
        invalid={invalid === spec.key}
      />
    </div>
  )
  return (
    <>
      {(tab.sections || []).map(([title, fields]) => (
        <SetSection key={title} title={title}>
          {fields.map(renderField)}
        </SetSection>
      ))}
      {extras}
    </>
  )
}

// --- System tab extras: dev-environment card (read-only runtime view) + pre-registered workspaces ---

export function SystemExtras({
  draft,
  setDraft,
  controller,
  active,
  envReloadToken,
}: {
  draft: SettingsDraft
  setDraft: React.Dispatch<React.SetStateAction<SettingsDraft>>
  controller: AppController
  active: boolean
  envReloadToken: number
}) {
  const { markDirty, invalid } = useSettingsCtx()
  return (
    <>
      <EnvironmentCard controller={controller} active={active} reloadToken={envReloadToken} />
      <section className="set-sec">
        <h3 className="set-sec-title">预注册工作区</h3>
        <SetTip text="启动时注册的固定工作区（启动目录始终自动注册，无需在此列出）。root 支持 ~ 开头。" />
        <div className="set-cards">
          {draft.workspaces.map((card) => (
            <WorkspaceCard
              key={card.id}
              card={card}
              invalid={invalid}
              onPatch={(key, v) => {
                setDraft((d) => ({
                  ...d,
                  workspaces: d.workspaces.map((c) =>
                    c.id === card.id ? { ...c, fields: { ...c.fields, [key]: v } } : c,
                  ),
                }))
                markDirty()
              }}
              onDelete={() => {
                setDraft((d) => ({
                  ...d,
                  workspaces: d.workspaces.filter((c) => c.id !== card.id),
                }))
                markDirty()
              }}
            />
          ))}
        </div>
        <button
          type="button"
          className="btn btn-secondary btn-sm set-add"
          onClick={() => {
            setDraft((d) => ({
              ...d,
              workspaces: [...d.workspaces, { id: draftId(), fields: {} }],
            }))
            markDirty()
          }}
        >
          + 添加工作区
        </button>
      </section>
    </>
  )
}

function WorkspaceCard({
  card,
  invalid,
  onPatch,
  onDelete,
}: {
  card: CardDraft
  invalid: string | null
  onPatch: (key: string, v: ControlState) => void
  onDelete: () => void
}) {
  return (
    <div className="set-card">
      <div className="set-card-head">
        <input
          className="set-input"
          type="text"
          placeholder="显示名（可选）"
          spellCheck={false}
          data-field-id={card.id + ':name'}
          value={String(card.fields.name || '')}
          onChange={(e) => onPatch('name', e.target.value)}
        />
        <CardDelBtn title="删除该工作区" onDelete={onDelete} />
      </div>
      <div data-field-id={card.id + ':root'}>
        <FieldRow
          spec={{ key: 'root', label: '根目录', required: true, ph: '~/workspace/project' }}
          value={String(card.fields.root || '')}
          onChange={(v) => onPatch('root', v)}
          invalid={invalid === card.id + ':root'}
        />
      </div>
    </div>
  )
}

// --- Dev-environment card (read-only runtime report) ---

function EnvironmentCard({
  controller,
  active,
  reloadToken,
}: {
  controller: AppController
  active: boolean
  reloadToken: number
}) {
  const [report, setReport] = useState<EnvironmentReport | null>(null)
  const [loaded, setLoaded] = useState(false)
  const [loading, setLoading] = useState(false)
  const [err, setErr] = useState('')
  const [refreshing, setRefreshing] = useState(false)

  const load = useCallback(
    async (force = false) => {
      if (loading || (loaded && !force)) return
      setLoading(true)
      setErr('')
      if (force) setRefreshing(true)
      try {
        const r = await controller.api.metaEnvironment()
        setReport(r)
        setLoaded(true) // set only on success: after a failure, re-entering the tab allows auto-retry
      } catch (e) {
        const err = e as ApiError
        if (err.status !== 401) {
          setErr('环境报告不可用: ' + err.message)
          if (force) toast('环境报告加载失败: ' + err.message)
        }
      } finally {
        setLoading(false)
        setRefreshing(false)
      }
    },
    [controller, loading, loaded],
  )

  useEffect(() => {
    if (active) void load()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [active])

  // Precisely invalidate by key after a saved path_extra change (the panel increments reloadToken)
  const lastToken = useState(() => reloadToken)[0]
  useEffect(() => {
    if (reloadToken !== lastToken) {
      setLoaded(false)
      if (active) void load()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [reloadToken])

  return (
    <section className="set-sec set-sec-card">
      <div className="set-skills-bar">
        <h3 className="set-sec-title">开发环境（运行时状态）</h3>
        <button
          type="button"
          className={
            'btn btn-secondary btn-sm set-skills-refresh' + (refreshing ? ' is-spinning' : '')
          }
          title="重新读取运行时报告"
          disabled={refreshing}
          onClick={() => void load(true)}
        >
          <Icon name="rotate-left" />
          刷新
        </button>
      </div>
      <SetTip text="loom 启动时把常见工具目录补进进程 PATH（GUI 启动时 PATH 只有系统目录），优先级：上方「额外 PATH 目录」> 内置清单 > 通配展开 > 继承的 PATH。沙箱内命令看到的就是这份 PATH——「未检测到」的工具可考虑把其目录登记到上方配置。" />
      <div className="set-env">
        {loading && !loaded ? (
          <SetLoading text="读取环境报告中…" />
        ) : err ? (
          <SetLoading text={err} isError />
        ) : report ? (
          <>
            <h4 className="set-env-sub">关键工具解析</h4>
            {(report.tools || []).map((t) => (
              <div className="env-row" key={t.name}>
                <span className="env-name mono">{t.name}</span>
                <span className={'env-val mono' + (t.found ? '' : ' is-missing')}>
                  {t.found ? t.path : '未检测到'}
                </span>
              </div>
            ))}
            <h4 className="set-env-sub">候选目录（按优先级）</h4>
            {(report.dirs || []).length === 0 && <div className="set-hint">（无候选目录）</div>}
            {(report.dirs || []).map((d, i) => (
              <div className={'env-row' + (d.status === 'missing' ? ' is-dim' : '')} key={i}>
                <span className="env-val mono">{d.path}</span>
                <span className={'env-badge env-src-' + d.source}>
                  {{ config: '配置', static: '内置', glob: '通配' }[d.source as string] || d.source}
                </span>
                <span className={'env-badge env-st-' + d.status}>
                  {{ prepended: '已注入', existing: '已在 PATH', missing: '未安装' }[
                    d.status as string
                  ] || d.status}
                </span>
              </div>
            ))}
            {report.effective_path && (
              <>
                <h4 className="set-env-sub">生效 PATH</h4>
                <div className="set-hint mono set-env-path">{report.effective_path}</div>
              </>
            )}
          </>
        ) : null}
      </div>
    </section>
  )
}
