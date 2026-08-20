// Header.tsx — 应用顶行：侧栏开关 / 主题 / 设置 / 工作区面包屑 /
// session id 复制 / 分享 / 只读徽标 / 会话状态徽标 / 连接徽标。

import { memo } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import { shortId } from '../lib/format'

const SESSION_STATE_BADGE: Record<string, [string, string]> = {
  idle: ['', 'idle'],
  running: ['is-running', 'running'],
  awaiting_approval: ['is-awaiting', 'awaiting approval'],
  cancelling: ['is-awaiting', 'cancelling'],
  booting: ['', 'booting'],
  fatal: ['is-dead', 'fatal'],
  closed: ['', 'closed'],
}

export const Header = memo(function Header({
  controller,
  onRevealWorkspace,
}: {
  controller: AppController
  onRevealWorkspace: (wsId: string) => void
}) {
  const theme = useStore(controller.store, (s) => s.theme)
  const sessionId = useStore(controller.store, (s) => s.sessionId)
  const sessionState = useStore(controller.store, (s) => s.sessionState)
  const connState = useStore(controller.store, (s) => s.connState)
  const connDetail = useStore(controller.store, (s) => s.connDetail)
  const readOnly = useStore(controller.store, (s) => s.readOnly)
  const archived = useStore(controller.store, (s) => s.archived)
  const readOnlyTitle = useStore(controller.store, (s) => s.readOnlyTitle)
  const hdrWorkspace = useStore(controller.store, (s) => s.hdrWorkspace)
  const hdrWorkspaceTitle = useStore(controller.store, (s) => s.hdrWorkspaceTitle)
  const noWorkspace = useStore(controller.store, (s) => s.noWorkspace)
  const landingVisible = useStore(controller.store, (s) => s.landingVisible)

  const [stateCls, stateText] = SESSION_STATE_BADGE[sessionState] || ['', sessionState || '']
  const connMap: Record<string, [string, string]> = {
    connecting: ['is-reconnecting', 'connecting…'],
    live: ['is-live', 'live'],
    reconnecting: [
      'is-reconnecting',
      connDetail ? `reconnecting (${connDetail})` : 'reconnecting…',
    ],
    draining: ['is-draining', 'draining'],
    dead: ['is-dead', connDetail || 'disconnected'],
  }
  const [connCls, connText] = connMap[connState] || ['', connState]

  const locked = readOnly || archived
  const showSessionChrome = !!sessionId && !noWorkspace && !landingVisible

  return (
    <header className="app-header">
      <button
        id="hdr-sidebar"
        className="icon-btn"
        title="折叠/展开侧栏"
        onClick={() => controller.toggleSidebar()}
      >
        <Icon name="bars" />
      </button>
      <button
        id="hdr-theme"
        className="icon-btn"
        title={theme === 'dark' ? '切换到浅色模式' : '切换到深色模式'}
        onClick={() => controller.toggleTheme()}
      >
        <Icon name="circle-half-stroke" />
      </button>
      <button
        id="hdr-settings"
        className="icon-btn"
        title="设置"
        onClick={() => controller.openSettings()}
      >
        <Icon name="gear" />
      </button>
      <button
        id="hdr-ws"
        className="hdr-ws"
        type="button"
        title={hdrWorkspaceTitle || '定位所属工作区'}
        hidden={!showSessionChrome || !hdrWorkspace}
        onClick={() => {
          const wsId = controller.revealCurrentWorkspace()
          if (wsId !== null) onRevealWorkspace(wsId)
        }}
      >
        {hdrWorkspace}
      </button>
      <button
        id="hdr-session"
        className="sess mono"
        title="copy session id"
        hidden={!showSessionChrome}
        onClick={() => void controller.copySessionId()}
      >
        {shortId(sessionId || '')}
      </button>
      <button
        id="hdr-share"
        className="icon-btn"
        title="分享会话：复制公开只读链接（Shift+点击撤销分享）"
        hidden={!showSessionChrome}
        onClick={(e) => void controller.shareSession(e.shiftKey)}
      >
        <Icon name="share" />
      </button>
      <span id="hdr-readonly" className="badge is-awaiting" hidden={!locked} title={readOnlyTitle}>
        <span className="dot" />
        <span className="txt">{readOnly ? 'sub-agent · read-only' : 'archived · read-only'}</span>
      </span>
      <span id="hdr-state" className={'badge' + (stateCls ? ' ' + stateCls : '')}>
        <span className="dot" />
        <span className="txt">{stateText}</span>
      </span>
      <span className="spacer" />
      <span id="hdr-conn" className={'badge' + (connCls ? ' ' + connCls : '')}>
        <span className="dot" />
        <span className="txt">{connText}</span>
      </span>
    </header>
  )
})
