// McpTab.tsx — MCP tab：概览 → 详情两级导航 + 进程级实时状态徽标/重连。
// 与旧 settings.js 的 MCP 部分一一对应。

import { useCallback, useEffect, useState } from 'react'
import type { AppController } from '../../app/controller'
import type { McpServerStatus } from '../../protocol/types'
import { ApiError } from '../../protocol/api'
import type { SettingsDraft, McpDraft, ControlState } from './SettingsPanel'
import { draftId } from './draft'
import { MCP_STDIO_FIELDS, MCP_HTTP_FIELDS, MCP_COMMON_FIELDS, type FieldSpec } from './spec'
import { collectFields } from './convert'
import { FieldRow, SetNavBar, SetCardSummary, CardDelBtn, SetTip } from './controls'
import { useSettingsCtx } from './SettingsPanel'
import { Select } from '../ui/Select'
import { Icon } from '../../lib/icons'
import { toast } from '../ui/Toast'

// 概览行元信息：传输方式 + 命令/URL 摘要（与收集逻辑同源地读当前传输组）。
function mcpMeta(card: McpDraft): string {
  const srv: Record<string, unknown> = {}
  collectFields(MCP_COMMON_FIELDS, card.common, srv)
  collectFields(
    card.transport === 'http' ? MCP_HTTP_FIELDS : MCP_STDIO_FIELDS,
    card.transport === 'http' ? card.http : card.stdio,
    srv,
  )
  if (card.transport === 'http') return 'HTTP · ' + ((srv.url as string) || '未配置 URL')
  const cmd = [srv.command, ...((srv.args as string[]) || [])].filter(Boolean).join(' ')
  return 'stdio · ' + (cmd || '未配置命令')
}

export function McpTab({
  draft,
  setDraft,
  openMcpId,
  setOpenMcpId,
  controller,
  statusToken,
}: {
  draft: SettingsDraft
  setDraft: React.Dispatch<React.SetStateAction<SettingsDraft>>
  openMcpId: string | null
  setOpenMcpId: (id: string | null) => void
  controller: AppController
  statusToken: number // 保存后递增：触发状态重拉
}) {
  const { markDirty, invalid } = useSettingsCtx()
  const [statuses, setStatuses] = useState<Map<string, McpServerStatus>>(new Map())
  const [reconnecting, setReconnecting] = useState<string | null>(null)

  // 拉取进程级 MCP 实时状态（打开面板与保存后调用）。
  const refresh = useCallback(async () => {
    try {
      const r = await controller.api.listMcpServers()
      setStatuses(new Map((r.servers || []).map((s) => [s.name, s])))
    } catch {
      // 状态查询失败不影响编辑
    }
  }, [controller])

  useEffect(() => {
    void refresh()
  }, [refresh, statusToken])

  const openCard = draft.mcpServers.find((c) => c.id === openMcpId) || null
  const crumb = openCard ? 'MCP 服务器 / ' + (openCard.name.trim() || '未命名') : ''

  const patchCard = (cardId: string, patch: Partial<Omit<McpDraft, 'id'>>) => {
    setDraft((d) => ({
      ...d,
      mcpServers: d.mcpServers.map((c) => (c.id === cardId ? { ...c, ...patch } : c)),
    }))
    markDirty()
  }

  const patchGroup = (
    cardId: string,
    group: 'stdio' | 'http' | 'common',
    key: string,
    v: ControlState,
  ) => {
    setDraft((d) => ({
      ...d,
      mcpServers: d.mcpServers.map((c) =>
        c.id === cardId ? { ...c, [group]: { ...c[group], [key]: v } } : c,
      ),
    }))
    markDirty()
  }

  const addCard = () => {
    const card: McpDraft = {
      id: draftId(),
      name: '',
      transport: 'stdio',
      stdio: {},
      http: {},
      common: {},
    }
    setDraft((d) => ({ ...d, mcpServers: [...d.mcpServers, card] }))
    markDirty()
    setOpenMcpId(card.id)
  }

  const deleteCard = (card: McpDraft) => {
    if (openMcpId === card.id) setOpenMcpId(null)
    setDraft((d) => ({ ...d, mcpServers: d.mcpServers.filter((c) => c.id !== card.id) }))
    markDirty()
  }

  const reconnect = async (card: McpDraft) => {
    const name = card.name.trim()
    if (!name) {
      toast('先填写服务器名并保存配置')
      return
    }
    setReconnecting(card.id)
    try {
      const status = await controller.api.reconnectMcpServer(name)
      setStatuses((prev) => new Map(prev).set(name, status))
      if (status.connected) toast(`MCP 服务器 ${name} 已连接`, true)
      else toast(`MCP 服务器 ${name} 连接失败: ${status.error || 'unknown'}`)
    } catch (e) {
      const err = e as ApiError
      if (err.status === 401) return
      if (err.message && err.message.includes('unknown mcp server')) {
        toast('该服务器不在已保存的配置中（改名或新增后请先保存）')
      } else {
        toast('重连失败: ' + err.message)
      }
    } finally {
      setReconnecting(null)
    }
  }

  const renderGroup = (card: McpDraft, group: 'stdio' | 'http' | 'common', specs: FieldSpec[]) => (
    <div
      className="set-group"
      data-transport={group}
      hidden={group !== 'common' && card.transport !== group}
    >
      {specs.map((spec) => (
        <div key={spec.key} data-field-id={`${card.id}:${spec.key}`}>
          <FieldRow
            spec={spec}
            value={
              card[group][spec.key] ??
              (spec.type === 'bool' || spec.type === 'flag-list' ? false : '')
            }
            onChange={(v) => patchGroup(card.id, group, spec.key, v)}
            invalid={invalid === `${card.id}:${spec.key}`}
          />
        </div>
      ))}
    </div>
  )

  return (
    <>
      <SetTip text="两种传输二选一：command（stdio 子进程）或 url（远程 HTTP）。header 值支持 ${VAR} 环境变量引用（令牌不落盘）。工具名格式 mcp__{服务器名}__{工具名}。" />

      <SetNavBar hidden={!openCard} crumb={crumb} onBack={() => setOpenMcpId(null)} />

      <div className="set-list-sec">
        <div className="set-cards">
          {draft.mcpServers.map((card) => {
            const name = card.name.trim()
            // badge 三态：未命名卡片 → 不显示；已命名但未连接 → 「保存后连接」；
            // status → 已连接 N 工具 / 连接失败。
            const status = name ? statuses.get(name) || null : undefined
            const isOpen = openMcpId === card.id
            return (
              <div key={card.id} className={'set-card' + (isOpen ? ' is-open' : '')}>
                <div className="set-card-head">
                  <SetCardSummary
                    name={name || '（未命名服务器）'}
                    nameEmpty={!name}
                    meta={mcpMeta(card)}
                    onOpen={() => setOpenMcpId(card.id)}
                  />
                  <input
                    className="set-input"
                    type="text"
                    placeholder="服务器名（必填）"
                    spellCheck={false}
                    data-field-id={card.id + ':name'}
                    value={card.name}
                    onChange={(e) => patchCard(card.id, { name: e.target.value })}
                  />
                  <McpBadge status={status} />
                  <button
                    type="button"
                    className={
                      'icon-btn mcp-reconnect' + (reconnecting === card.id ? ' is-spinning' : '')
                    }
                    title="重新连接"
                    disabled={reconnecting === card.id}
                    onClick={() => void reconnect(card)}
                  >
                    <Icon name="rotate-left" />
                  </button>
                  <CardDelBtn title="删除该服务器" onDelete={() => deleteCard(card)} />
                </div>
                <div className="set-card-body">
                  {/* 「已注册工具」只读小节（随徽标刷新） */}
                  <div className="set-group mcp-tools-sec">
                    <McpTools status={status} serverName={name} />
                  </div>

                  {/* 传输形态切换：由 command/url 哪个有值推定；切换只影响展示哪组字段 */}
                  <div className="set-row">
                    <label className="set-label">传输方式</label>
                    <div className="set-field">
                      <Select
                        className="set-input set-transport"
                        options={[
                          { value: 'stdio', label: 'command（stdio 子进程）' },
                          { value: 'http', label: 'url（远程 HTTP）' },
                        ]}
                        value={card.transport}
                        onChange={(v) => patchCard(card.id, { transport: v as 'stdio' | 'http' })}
                      />
                    </div>
                  </div>

                  {renderGroup(card, 'stdio', MCP_STDIO_FIELDS)}
                  {renderGroup(card, 'http', MCP_HTTP_FIELDS)}
                  {renderGroup(card, 'common', MCP_COMMON_FIELDS)}
                </div>
              </div>
            )
          })}
        </div>
        <button type="button" className="btn btn-secondary btn-sm set-add" onClick={addCard}>
          + 添加 MCP 服务器
        </button>
      </div>
    </>
  )
}

function McpBadge({ status }: { status: McpServerStatus | null | undefined }) {
  if (status === undefined) return <span className="mcp-status" />
  if (status === null) return <span className="mcp-status">保存后连接</span>
  if (status.connected) {
    return (
      <span
        className="mcp-status is-live"
        title={(status.tools || []).map((t) => t.name).join('\n')}
      >
        {`已连接 · ${(status.tools || []).length} 工具`}
      </span>
    )
  }
  return (
    <span className="mcp-status is-dead" title={status.error || ''}>
      连接失败
    </span>
  )
}

// 「已注册工具」小节（详情表单上方，只读）：每行显示服务器本地名 +
// 简介（两行截断，点击展开），hover 名称见完整限定名。
function McpTools({
  status,
  serverName,
}: {
  status: McpServerStatus | null | undefined
  serverName: string
}) {
  const [expandedIdx, setExpandedIdx] = useState<number | null>(null)
  if (!status || !status.connected) {
    const hint =
      status && status.error
        ? '连接失败，修复后点击右上角重连'
        : '保存并连接成功后展示该服务器的工具列表'
    return (
      <>
        <div className="set-subtitle">已注册工具（实时状态 · 白/黑名单过滤后的生效集）</div>
        <div className="set-hint">{hint}</div>
      </>
    )
  }
  if (!status.tools || !status.tools.length) {
    return (
      <>
        <div className="set-subtitle">已注册工具（实时状态 · 白/黑名单过滤后的生效集）</div>
        <div className="set-hint">该服务器未暴露工具（或已被白/黑名单全部过滤）</div>
      </>
    )
  }
  const prefix = `mcp__${serverName}__`
  // 简介里的 [MCP server "…"] 前缀是适配层给模型看的归因（mcp/tool.go），
  // UI 上服务器归属显而易见，剥掉更干净
  const descPrefix = `[MCP server "${serverName}"] `
  return (
    <>
      <div className="set-subtitle">已注册工具（实时状态 · 白/黑名单过滤后的生效集）</div>
      <div className="mcp-tool-list">
        {status.tools.map((t, i) => {
          let descText = t.description || ''
          if (descText.startsWith(descPrefix)) descText = descText.slice(descPrefix.length)
          return (
            <div className="mcp-tool-row" key={t.name}>
              <span className="mcp-tool-name mono" title={t.name}>
                {t.name.startsWith(prefix) ? t.name.slice(prefix.length) : t.name}
              </span>
              <span
                className={
                  'mcp-tool-desc' +
                  (descText ? '' : ' is-empty') +
                  (expandedIdx === i ? ' is-expanded' : '')
                }
                title={descText || undefined}
                onClick={
                  descText ? () => setExpandedIdx((prev) => (prev === i ? null : i)) : undefined
                }
              >
                {descText || '（无简介）'}
              </span>
            </div>
          )
        })}
      </div>
    </>
  )
}
