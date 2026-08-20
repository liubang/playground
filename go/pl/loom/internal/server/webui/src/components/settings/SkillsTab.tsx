// SkillsTab.tsx — Skills tab：配置小节（全局 scope）+ 运行时发现视图
// （禁用/启用按名称跨工作区生效并热应用；删除从磁盘移除目录）。
// 与旧 settings.js 的 Skills 部分一一对应。

import { useCallback, useEffect, useState } from 'react'
import type { AppController } from '../../app/controller'
import type { SkillGroup, SkillInfo } from '../../protocol/types'
import { ApiError } from '../../protocol/api'
import type { SettingsDraft, ControlState } from './SettingsPanel'
import { SKILLS_CONFIG_FIELDS, SKILLS_EMPTY_HINT } from './spec'
import { FieldRow, SetTip, SetLoading } from './controls'
import { useSettingsCtx } from './SettingsPanel'
import { confirmDialog } from '../ui/Confirm'
import { toast } from '../ui/Toast'
import { Icon } from '../../lib/icons'

interface SkillsState {
  loaded: boolean
  loading: boolean
  enabled: boolean
  reason: string
  groups: SkillGroup[]
}

export function SkillsTab({
  draft,
  setGlobal,
  controller,
  active,
  onDisabledChanged,
}: {
  draft: SettingsDraft
  setGlobal: (key: string, v: ControlState) => void
  controller: AppController
  active: boolean
  // 禁用开关经专用端点直写 config：同步面板持有的 revision 与 disabled
  // 列表，否则后续保存设置会 409 冲突，或把 skills.disabled 回滚成旧值
  onDisabledChanged: (revision: string | undefined, disabled: string[]) => void
}) {
  const { invalid } = useSettingsCtx()
  const [state, setState] = useState<SkillsState>({
    loaded: false,
    loading: false,
    enabled: true,
    reason: '',
    groups: [],
  })
  const [refreshing, setRefreshing] = useState(false)

  const load = useCallback(
    async (force = false) => {
      let proceed = false
      setState((s) => {
        if (s.loading || (s.loaded && !force)) return s
        proceed = true
        return { ...s, loading: true }
      })
      if (!proceed) return
      if (force) setRefreshing(true)
      try {
        const r = await controller.api.listSkills()
        setState({
          loaded: true, // 成功才置位：失败后下次切入 tab 允许自动重试
          loading: false,
          enabled: r.enabled !== false,
          reason: r.reason || '',
          groups: r.groups || [],
        })
        if (force) {
          const total = (r.groups || []).reduce((n, g) => n + (g.skills || []).length, 0)
          const issues = (r.groups || []).reduce((n, g) => n + (g.issues || []).length, 0)
          toast(`发现 ${total} 个 skill` + (issues ? `，${issues} 个加载失败` : ''), issues === 0)
        }
      } catch (e) {
        setState((s) => ({ ...s, loading: false }))
        if ((e as ApiError).status !== 401 && force) {
          toast('技能扫描失败: ' + (e as Error).message)
        }
      } finally {
        setRefreshing(false)
      }
    },
    [controller],
  )

  useEffect(() => {
    if (active) void load()
  }, [active, load])

  const toggleSkill = async (sk: SkillInfo) => {
    try {
      const resp = await controller.api.setSkillDisabled(sk.name, !sk.disabled)
      onDisabledChanged(resp.revision, resp.disabled || [])
      toast(sk.disabled ? `已启用 ${sk.name}` : `已禁用 ${sk.name}（立即生效）`, true)
      // 就地同步行状态（禁用按名称生效，可能涉及多行），不整表重扫——
      // 滚动位置与已展开的简介因此保留
      const off = new Set(resp.disabled || [])
      setState((s) => ({
        ...s,
        groups: s.groups.map((g) => ({
          ...g,
          skills: (g.skills || []).map((x) =>
            x.name === sk.name || off.has(x.name) ? { ...x, disabled: off.has(x.name) } : x,
          ),
        })),
      }))
    } catch (e) {
      if ((e as ApiError).status !== 401) {
        toast((sk.disabled ? '启用失败: ' : '禁用失败: ') + (e as Error).message)
      }
    }
  }

  const deleteSkill = async (sk: SkillInfo) => {
    const ok = await confirmDialog({
      title: '删除 skill',
      body: `将从磁盘删除「${sk.name}」所在目录（含目录内全部文件）：${sk.path}。该操作不可恢复。`,
      okLabel: '删除',
    })
    if (!ok) return
    try {
      await controller.api.deleteSkill(sk.path)
      toast(`已删除 ${sk.name}`, true)
      // 删除后就地移除对应行；分组清空后整组移除
      setState((s) => ({
        ...s,
        groups: s.groups
          .map((g) => ({ ...g, skills: (g.skills || []).filter((x) => x.path !== sk.path) }))
          .filter((g) => (g.skills || []).length > 0 || (g.issues || []).length > 0),
      }))
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('删除失败: ' + (e as Error).message)
    }
  }

  return (
    <>
      {/* 配置小节：控件归属全局 scope */}
      <section className="set-sec set-sec-card">
        <h3 className="set-sec-title">技能配置</h3>
        {SKILLS_CONFIG_FIELDS.map((spec) => (
          <div key={spec.key} data-field-id={spec.key}>
            <FieldRow
              spec={spec}
              value={draft.globals[spec.key] ?? ''}
              onChange={(v) => setGlobal(spec.key, v)}
              invalid={invalid === spec.key}
            />
          </div>
        ))}
      </section>

      <div className="set-skills-bar">
        <h3 className="set-sec-title">发现的技能（运行时状态）</h3>
        <button
          type="button"
          className={
            'btn btn-secondary btn-sm set-skills-refresh' + (refreshing ? ' is-spinning' : '')
          }
          title="重新扫描磁盘"
          disabled={refreshing}
          onClick={() => void load(true)}
        >
          <Icon name="rotate-left" />
          刷新
        </button>
      </div>
      <SetTip text="各工作区发现的 skill（未发现 skill 的工作区不列出）。禁用按名称对所有工作区生效（立即生效，写入 config 的 skills.disabled）；删除会从磁盘移除整个目录。编辑内容请直接修改对应的 SKILL.md；上方的配置改动保存后生效（重启后应用）。" />

      <div className="set-skills">
        {state.loading && !state.loaded ? (
          <SetLoading text="扫描技能目录中…" />
        ) : !state.enabled ? (
          <div className="set-hint">{`技能已禁用（${state.reason || '未知原因'}）。可在上方「启用技能」开启并保存。`}</div>
        ) : state.groups.length === 0 ? (
          <div className="set-hint">{SKILLS_EMPTY_HINT}</div>
        ) : (
          state.groups.map((g) => (
            <SkillGroupView
              key={g.workspace_name}
              g={g}
              onToggle={toggleSkill}
              onDelete={deleteSkill}
            />
          ))
        )}
      </div>
    </>
  )
}

function SkillGroupView({
  g,
  onToggle,
  onDelete,
}: {
  g: SkillGroup
  onToggle: (sk: SkillInfo) => void
  onDelete: (sk: SkillInfo) => void
}) {
  return (
    <section className="set-sec">
      <h3 className="set-sec-title">{g.workspace_name}</h3>
      {g.root && <div className="set-hint mono set-skill-root">{g.root}</div>}
      {(g.skills || []).map((sk) => (
        <SkillRow key={sk.path} sk={sk} onToggle={onToggle} onDelete={onDelete} />
      ))}
      {(g.issues || []).map((issue, i) => (
        <div className="skill-issue" key={i}>
          <Icon name="triangle-exclamation" />
          {issue}
        </div>
      ))}
    </section>
  )
}

function SkillRow({
  sk,
  onToggle,
  onDelete,
}: {
  sk: SkillInfo
  onToggle: (sk: SkillInfo) => void
  onDelete: (sk: SkillInfo) => void
}) {
  const [expanded, setExpanded] = useState(false)
  const [busy, setBusy] = useState(false)
  return (
    <div className={'skill-row' + (sk.disabled ? ' is-disabled' : '')}>
      <div className="skill-head">
        <span className="skill-name mono">{sk.name}</span>
        <span className={'skill-scope' + (sk.scope === 'repo' ? ' is-repo' : '')}>{sk.scope}</span>
        {sk.disabled && <span className="skill-scope is-off skill-off">已禁用</span>}
        <div className="skill-actions">
          <button
            type="button"
            className="icon-btn skill-action skill-toggle"
            title={(sk.disabled ? '启用' : '禁用') + '（按名称对所有工作区生效）'}
            disabled={busy}
            onClick={async () => {
              setBusy(true)
              await onToggle(sk)
              setBusy(false)
            }}
          >
            <Icon name={sk.disabled ? 'check' : 'ban'} />
          </button>
          <button
            type="button"
            className="icon-btn skill-action skill-del"
            title="从磁盘删除该 skill"
            onClick={() => void onDelete(sk)}
          >
            <Icon name="trash" />
          </button>
        </div>
      </div>
      {/* 简介默认截断为两行（CSS line-clamp），hover 见全文，点击展开/收起 */}
      <div
        className={'skill-desc' + (expanded ? ' is-expanded' : '')}
        title={sk.description}
        onClick={() => setExpanded((v) => !v)}
      >
        {sk.description}
      </div>
      <div className="skill-path mono">{sk.path}</div>
    </div>
  )
}
