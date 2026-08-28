// Sidebar.tsx — workspace tree sidebar (docs/WORKSPACE_DESIGN.md §11.3).
// Top level: one collapsible folder node per workspace, with that workspace's
// sessions inside the group; session items keep sub-agent hierarchy indentation
// and hover actions (archive/delete).
// One-to-one with the old components/sidebar.js.

import { memo, useEffect, useRef, useState } from 'react'
import type { AppController } from '../app/controller'
import type { SessionSummary, Workspace } from '../protocol/types'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import { relTime, shortId } from '../lib/format'
import { useRafScroll } from '../lib/rafScroll'

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
  const mainView = useStore(controller.store, (s) => s.mainView)
  const sessionsLoading = useStore(controller.store, (s) => s.sessionsLoading)
  const [collapsed, setCollapsed] = useState<Set<string>>(loadCollapsed)
  const listRef = useRef<HTMLDivElement>(null)
  // First mount: make the most recently active workspace the visual focus
  // (expand it, collapse the other groups)
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

  // Startup focus: desktop starts a fresh page session every launch, so the
  // collapsed state is not carried over.
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

  // User-initiated session open: expand the containing group if collapsed and
  // scroll the item into view — an explicitly selected session must be visible.
  // Re-selecting after a polling refresh neither scrolls nor touches the
  // collapsed state (deliberately collapsing the group containing the current
  // session is a legitimate choice).
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

  // Breadcrumb reveal: expand the given workspace group and scroll it — plus the
  // current session inside it — into view
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

  // Grouping: workspace_id ("" = default/legacy) → [sessions]
  const byWs = new Map<string, SessionSummary[]>()
  for (const s of sessions) {
    const k = s.workspace_id || ''
    if (!byWs.has(k)) byWs.set(k, [])
    byWs.get(k)!.push(s)
  }
  // Workspace order: registered workspaces (newest first), then any that have
  // sessions but are not registered.
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
        onScroll={useRafScroll<HTMLDivElement>((el) => {
          // Session-list waterfall: load the next page when scrolled near the bottom
          if (el.scrollHeight - el.scrollTop - el.clientHeight < 120) {
            void controller.loadMoreSessions()
          }
        })}
      >
        {ordered.map((wsId) => {
          const ws = workspaces.find((w) => w.id === wsId)
          const wsSessions = byWs.get(wsId) || []
          // The archived view is read-only history: skip workspace groups with no
          // archived sessions.
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
        {sessionsLoading && (
          <div className="session-list-more" aria-hidden="true">
            <span className="spinner" /> 加载更多会话…
          </div>
        )}
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
        <button
          id="open-compare"
          className={'foot-btn' + (mainView === 'compare' ? ' is-active' : '')}
          title="轨迹对比：选两个会话同轴对比执行过程"
          onClick={() => controller.openCompare()}
        >
          <Icon name="layer-group" /> 对比
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
  // A non-empty wsId with no matching entity = the owning workspace was deleted.
  // Historical data from before cascading deletes shipped may leave such dangling
  // sessions; they still render under the read-only "deleted workspace" group.
  const name = ws ? ws.name : wsId ? '已删除的工作区' : '默认工作区'
  // Count of sessions awaiting approval in the group (the only distress signal
  // visible while collapsed) and the current-session membership flag.
  const attnCount = sessions.filter((s) => s.state === 'awaiting_approval').length
  const hasActive = sessions.some((s) => s.id === activeId)

  // In-group session hierarchy: sub-agent sessions render indented under their
  // parent; a session whose parent is not in the group renders at top level
  // (fallback for pagination boundaries).
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
        {/* No folder icon: a whole column of same-color high-saturation icons is
            the main source of the "template look"; the caret already conveys the
            collapsed state */}
        <span className="ws-name" title={ws?.root_path || undefined}>
          {name}
        </span>
        <span className="ws-count">{String(sessions.length)}</span>
        {attnCount > 0 && (
          <span className="ws-attn" title={`${attnCount} 个会话等待审批`}>
            {String(attnCount)}
          </span>
        )}
        {/* New/delete entries only show in the active view (the archived view is
            read-only history) */}
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
      {/* Live status dot: awaiting_approval gets an amber breathing light (needs
          attention the most), running/cancelling get green; other states show
          nothing, keeping the list quiet. */}
      {showDot && (
        <span
          className={'st-dot ' + (st === 'awaiting_approval' ? 'is-attn' : 'is-run')}
          title={st === 'awaiting_approval' ? '等待审批' : '运行中'}
        />
      )}
      <span className="t">{s.title || shortId(s.id)}</span>
      <span className="rt">{relTime(s.created_at)}</span>
      {/* Hover actions: archive/unarchive + delete (take no resting width; they
          replace the timestamp on hover) */}
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
