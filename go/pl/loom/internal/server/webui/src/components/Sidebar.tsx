// Sidebar.tsx — 工作区树形侧栏（docs/WORKSPACE_DESIGN.md §11.3）。
// 顶层是每个工作区一个可折叠的文件夹节点，组内是该工作区的会话；
// 会话条目保留子 agent 层级缩进与悬停操作（归档/删除）。
// 与旧 components/sidebar.js 一一对应。

import { memo, useEffect, useRef, useState } from 'react'
import type { AppController } from '../app/controller'
import type { SessionSummary, Workspace } from '../protocol/types'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import { relTime, shortId } from '../lib/format'

const COLLAPSE_KEY = 'loom_ws_collapsed'

function loadCollapsed(): Set<string> {
  try {
    return new Set(JSON.parse(sessionStorage.getItem(COLLAPSE_KEY) || '[]') as string[])
  } catch {
    return new Set()
  }
}

function saveCollapsed(collapsed: Set<string>) {
  try {
    sessionStorage.setItem(COLLAPSE_KEY, JSON.stringify([...collapsed]))
  } catch {
    /* ignore */
  }
}

export const Sidebar = memo(function Sidebar({
  controller,
  revealWs,
}: {
  controller: AppController
  revealWs: { wsId: string; seq: number } | null
}) {
  const sessions = useStore(controller.store, (s) => s.sessions)
  const workspaces = useStore(controller.store, (s) => s.workspaces)
  const activeId = useStore(controller.store, (s) => s.sessionId)
  const showArchived = useStore(controller.store, (s) => s.showArchived)
  const [collapsed, setCollapsed] = useState<Set<string>>(loadCollapsed)
  const listRef = useRef<HTMLDivElement>(null)
  // 首次挂载：把最近活跃工作区设为视觉焦点（展开它、收起其他组）
  const focusedOnce = useRef(false)

  const toggleCollapse = (wsId: string) => {
    setCollapsed((prev) => {
      const next = new Set(prev)
      if (next.has(wsId)) next.delete(wsId)
      else next.add(wsId)
      saveCollapsed(next)
      return next
    })
  }

  // 启动焦点：desktop 每次启动都是新的页面会话，折叠态不带入。
  useEffect(() => {
    if (focusedOnce.current || sessions.length === 0) return
    focusedOnce.current = true
    const wsId = controller.recentWorkspaceId()
    if (!wsId) return
    if (!workspaces.some((w) => w.id === wsId)) return
    setCollapsed(() => {
      const next = new Set(workspaces.filter((w) => w.id !== wsId).map((w) => w.id))
      saveCollapsed(next)
      return next
    })
  }, [sessions, workspaces, controller])

  // 用户主动打开会话：所在组被折叠时展开该组，并把条目滚动到可视区域——
  // 主动选中的会话必须看得见。轮询刷新后的重设不滚动也不动折叠态（用户
  // 主动折叠包含当前会话的组是合法选择）。
  const lastActiveRef = useRef<string | null>(null)
  useEffect(() => {
    if (!activeId || lastActiveRef.current === activeId) return
    lastActiveRef.current = activeId
    const sess = sessions.find((s) => s.id === activeId)
    const wsId = sess?.workspace_id || ''
    setCollapsed((prev) => {
      if (!prev.has(wsId)) return prev
      const next = new Set(prev)
      next.delete(wsId)
      saveCollapsed(next)
      return next
    })
    requestAnimationFrame(() => {
      listRef.current?.querySelector('.sess-item.is-active')?.scrollIntoView({ block: 'nearest' })
    })
  }, [activeId, sessions])

  // 面包屑定位：展开指定工作区组并把它与组内当前会话滚动进可视区域
  const lastRevealSeq = useRef(0)
  useEffect(() => {
    if (!revealWs || revealWs.seq === lastRevealSeq.current) return
    lastRevealSeq.current = revealWs.seq
    setCollapsed((prev) => {
      if (!prev.has(revealWs.wsId)) return prev
      const next = new Set(prev)
      next.delete(revealWs.wsId)
      saveCollapsed(next)
      return next
    })
    requestAnimationFrame(() => {
      const node = listRef.current?.querySelector(
        `.ws-node[data-ws-id="${CSS.escape(revealWs.wsId)}"]`,
      )
      node?.scrollIntoView({ block: 'nearest' })
      listRef.current?.querySelector('.sess-item.is-active')?.scrollIntoView({ block: 'nearest' })
    })
  }, [revealWs])

  // 分组：workspace_id（"" = 默认/历史）→ [sessions]
  const byWs = new Map<string, SessionSummary[]>()
  for (const s of sessions) {
    const k = s.workspace_id || ''
    if (!byWs.has(k)) byWs.set(k, [])
    byWs.get(k)!.push(s)
  }
  // 工作区顺序：已注册 workspaces（newest first），再补上有会话但未注册的。
  const ordered = workspaces.map((w) => w.id)
  for (const k of byWs.keys()) {
    if (!ordered.includes(k)) ordered.push(k)
  }
  const ids = new Set(sessions.map((s) => s.id))

  return (
    <>
      <div className="ws-bar">
        <span className="ws-bar-title">工作区</span>
        <button
          id="ws-add"
          className="icon-btn"
          title="添加工作区"
          onClick={() => controller.openDirPicker()}
        >
          <Icon name="folder-plus" />
        </button>
      </div>
      <div
        id="session-list"
        className="session-list"
        ref={listRef}
        onScroll={() => {
          const list = listRef.current
          // 会话列表瀑布流：滚动接近底部时加载下一页
          if (list && list.scrollHeight - list.scrollTop - list.clientHeight < 120) {
            void controller.loadMoreSessions()
          }
        }}
      >
        {ordered.map((wsId) => {
          const ws = workspaces.find((w) => w.id === wsId)
          const wsSessions = byWs.get(wsId) || []
          // 归档视图是只读历史：跳过无归档会话的工作区组。
          if (showArchived && wsSessions.length === 0) return null
          return (
            <WorkspaceGroup
              key={wsId || '$default'}
              wsId={wsId}
              ws={ws}
              sessions={wsSessions}
              allIds={ids}
              collapsed={collapsed.has(wsId)}
              activeId={activeId}
              archivedView={showArchived}
              onToggle={() => toggleCollapse(wsId)}
              controller={controller}
            />
          )
        })}
      </div>
      <div className="sidebar-foot">
        <button
          id="toggle-archived"
          className={'foot-btn' + (showArchived ? ' is-active' : '')}
          title={showArchived ? '返回会话列表' : '查看归档会话'}
          onClick={() => controller.toggleArchivedView()}
        >
          {showArchived ? (
            <>
              <Icon name="arrow-left" /> 返回
            </>
          ) : (
            '归档'
          )}
        </button>
        <span className="sidebar-foot-brand">◆ loom</span>
      </div>
    </>
  )
})

const WorkspaceGroup = memo(function WorkspaceGroup({
  wsId,
  ws,
  sessions,
  allIds,
  collapsed,
  activeId,
  archivedView,
  onToggle,
  controller,
}: {
  wsId: string
  ws?: Workspace
  sessions: SessionSummary[]
  allIds: Set<string>
  collapsed: boolean
  activeId: string | null
  archivedView: boolean
  onToggle: () => void
  controller: AppController
}) {
  // wsId 非空但查无实体 = 所属工作区已被删除。级联删除落地前的历史数据
  // 可能留下这种悬空会话，仍归入只读的「已删除的工作区」分组展示。
  const name = ws ? ws.name : wsId ? '已删除的工作区' : '默认工作区'
  // 组内待审批会话数（折叠时唯一可见的求救信号）与当前会话归属标记。
  const attnCount = sessions.filter((s) => s.state === 'awaiting_approval').length
  const hasActive = sessions.some((s) => s.id === activeId)

  // 组内会话层级：子 agent 会话缩进挂在父会话下方，父会话不在组内时按
  // 顶层渲染（分页边界兜底）。
  const childrenOf = new Map<string, SessionSummary[]>()
  const tops: SessionSummary[] = []
  for (const s of sessions) {
    if (s.parent_session_id && allIds.has(s.parent_session_id)) {
      let arr = childrenOf.get(s.parent_session_id)
      if (!arr) childrenOf.set(s.parent_session_id, (arr = []))
      arr.push(s)
    } else {
      tops.push(s)
    }
  }
  const orderedItems: { s: SessionSummary; isChild: boolean }[] = []
  for (const s of tops) {
    orderedItems.push({ s, isChild: false })
    for (const c of childrenOf.get(s.id) || []) orderedItems.push({ s: c, isChild: true })
  }

  return (
    <div className="ws-group">
      <div
        className={
          'ws-node' + (collapsed ? ' is-collapsed' : '') + (hasActive ? ' has-active' : '')
        }
        data-ws-id={wsId}
        onClick={onToggle}
      >
        <span className="ws-caret">
          <Icon name={collapsed ? 'caret-right' : 'caret-down'} />
        </span>
        <span className="ws-icon">
          <Icon name={collapsed ? 'folder' : 'folder-open'} />
        </span>
        <span className="ws-name" title={ws?.root_path || undefined}>
          {name}
        </span>
        <span className="ws-count">{String(sessions.length)}</span>
        {attnCount > 0 && (
          <span className="ws-attn" title={`${attnCount} 个会话等待审批`}>
            {String(attnCount)}
          </span>
        )}
        {/* 新建/删除入口只在活跃视图显示（归档视图是只读历史） */}
        {!archivedView && (
          <>
            <button
              type="button"
              className="ws-new"
              title="在该工作区新建会话"
              onClick={(e) => {
                e.stopPropagation()
                controller.onNewSession(wsId)
              }}
            >
              <Icon name="file-document-plus" />
            </button>
            {ws && (
              <button
                type="button"
                className="ws-del"
                title="删除工作区（其下会话一并删除，磁盘目录保留）"
                onClick={(e) => {
                  e.stopPropagation()
                  void controller.onDeleteWorkspace(wsId)
                }}
              >
                <Icon name="trash" />
              </button>
            )}
          </>
        )}
      </div>
      {!collapsed && (
        <div className="ws-sessions">
          {sessions.length === 0 ? (
            <div className="ws-empty">无会话</div>
          ) : (
            orderedItems.map(({ s, isChild }) => (
              <SessionItem
                key={s.id}
                s={s}
                isChild={isChild}
                active={s.id === activeId}
                archivedView={archivedView}
                controller={controller}
              />
            ))
          )}
        </div>
      )}
    </div>
  )
})

const SessionItem = memo(function SessionItem({
  s,
  isChild,
  active,
  archivedView,
  controller,
}: {
  s: SessionSummary
  isChild: boolean
  active: boolean
  archivedView: boolean
  controller: AppController
}) {
  const st = s.state || ''
  const showDot = st === 'awaiting_approval' || st === 'running' || st === 'cancelling'
  return (
    <button
      type="button"
      className={'sess-item' + (active ? ' is-active' : '') + (isChild ? ' is-child' : '')}
      data-id={s.id}
      title={(s.title || shortId(s.id)) + (s.model_name ? ` · ${s.model_name}` : '')}
      onClick={() => controller.onSelectSession(s.id)}
    >
      {isChild && (
        <span className="child-mark" title="子智能体会话">
          <Icon name="robot" />
        </span>
      )}
      {/* live 状态灯：awaiting_approval 用琥珀色呼吸灯（最需要抢注意力），
          running/cancelling 用绿色；其余状态不显示，保持列表安静。 */}
      {showDot && (
        <span
          className={'st-dot ' + (st === 'awaiting_approval' ? 'is-attn' : 'is-run')}
          title={st === 'awaiting_approval' ? '等待审批' : '运行中'}
        />
      )}
      <span className="t">{s.title || shortId(s.id)}</span>
      <span className="rt">{relTime(s.created_at)}</span>
      {/* 悬停操作：归档/取消归档 + 删除（不占常态宽度，hover 时替换时间戳） */}
      <span className="acts">
        <button
          type="button"
          className="act"
          title={archivedView ? '取消归档' : '归档'}
          onClick={(e) => {
            e.stopPropagation()
            void controller.onSessionAction(s.id, archivedView ? 'unarchive' : 'archive')
          }}
        >
          <Icon name={archivedView ? 'rotate-left' : 'box-archive'} />
        </button>
        <button
          type="button"
          className="act act-del"
          title="删除会话"
          onClick={(e) => {
            e.stopPropagation()
            void controller.onSessionAction(s.id, 'delete')
          }}
        >
          <Icon name="trash" />
        </button>
      </span>
    </button>
  )
})
