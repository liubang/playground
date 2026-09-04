// App.tsx — app shell: gate / main UI (sidebar + top bar + message stream +
// composer + status bar) + global overlays (toast / confirm / settings /
// directory browser / banner).

import { lazy, Suspense, useEffect, useMemo, useState } from 'react'
import type { AppController } from './app/controller'
import { useStore } from './store/store'
import { Gate } from './components/Gate'
import { Header } from './components/Header'
import { Sidebar } from './components/Sidebar'
import {
  TranscriptSearch,
  TranscriptView,
  type TranscriptViewIO,
} from './components/TranscriptView'
import { Composer } from './components/Composer'
import { StatusBar } from './components/StatusBar'
import { DirPicker } from './components/DirPicker'
import { RightPanel } from './components/panel/RightPanel'
import { SettingsPanel } from './components/settings/SettingsPanel'
// 这些重页面（maze / trace / compare）都会拉入 SVG 渲染 / 迷宫数据 / 高亮 diff
// 等大依赖：用 React.lazy 拆成独立 chunk，初始会话页只加载聊天流，不背这些。
const MazePage = lazy(() =>
  import('./components/maze/MazePage').then((m) => ({ default: m.MazePage })),
)
const CompareView = lazy(() =>
  import('./components/maze/CompareView').then((m) => ({ default: m.CompareView })),
)
const TracePage = lazy(() =>
  import('./components/trace/TraceView').then((m) => ({ default: m.TracePage })),
)
import { SessionTabs } from './components/SessionTabs'
import { ToastHost } from './components/ui/Toast'
import { ConfirmHost, isConfirmOpen } from './components/ui/Confirm'
import { BlocksIOContext } from './components/blocks/context'
import { Icon } from './lib/icons'

export function App({ controller }: { controller: AppController }) {
  const view = useStore(controller.store, (s) => s.view)
  const sidebarCollapsed = useStore(controller.store, (s) => s.sidebarCollapsed)
  const noWorkspace = useStore(controller.store, (s) => s.noWorkspace)
  const landingVisible = useStore(controller.store, (s) => s.landingVisible)
  const landingHint = useStore(controller.store, (s) => s.landingHint)
  const landingShowAddWs = useStore(controller.store, (s) => s.landingShowAddWs)
  const banner = useStore(controller.store, (s) => s.banner)
  const mainView = useStore(controller.store, (s) => s.mainView)
  const rightPanelOpen = useStore(controller.store, (s) => s.rightPanelOpen)
  const sessionId = useStore(controller.store, (s) => s.sessionId)
  const sessionLoading = useStore(controller.store, (s) => s.sessionLoading)
  const connState = useStore(controller.store, (s) => s.connState)
  const [revealWs, setRevealWs] = useState<{ wsId: string; seq: number } | null>(null)

  // TranscriptView interaction callbacks (stable references: controller methods
  // are all bound arrow functions)
  const transcriptIO = useMemo<TranscriptViewIO>(
    () => ({
      onResolveApproval: (approvalId, decision, always, trust) =>
        void controller.transcript.resolveApproval(approvalId, decision, always, trust),
      onAnswerQuestion: (questionId, answer) =>
        void controller.transcript.answerQuestion(questionId, answer, answer.skipped),
      onFeedback: (runId, value) => controller.transcript.sendFeedback(runId, value),
      fetchToolOutput: controller.fetchToolOutput,
    }),
    [controller],
  )

  const blocksIO = useMemo(() => ({ fetchArtifactURL: controller.fetchArtifactURL }), [controller])

  // 全局快捷键：
  // - Cmd/Ctrl+F 打开会话内搜索（虚拟滚动下浏览器原生 Cmd+F 搜不到
  //   视口外的块，必须接管）。仅在聊天视图接管，trace/maze 列表不虛拟化，
  //   浏览器原生搜索可用。
  // - Esc 取消当前 turn。避让顺序：确认弹窗（自带处理）→ 搜索条（关
  //   闭自身）→ busy 取消；defaultPrevented 说明已被更内层消费
  //   （如补全菜单 Esc、picker Esc），不再重复响应。
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && (e.key === 'f' || e.key === 'F')) {
        if (controller.store.get().view !== 'app' || controller.store.get().mainView !== 'chat')
          return
        e.preventDefault()
        controller.transcript.openSearch()
        return
      }
      if (e.key !== 'Escape' || e.defaultPrevented) return
      if (isConfirmOpen()) return
      if (controller.store.get().settingsOpen) return // 设置面板自带 Esc（脏时确认）
      if (controller.store.get().dirPickerOpen) return
      if (controller.transcript.store.get().search) {
        controller.transcript.closeSearch()
        return
      }
      if (controller.store.get().busy) controller.cancelTurn()
    }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [controller])

  if (view === 'boot') {
    // Auth check in progress: render nothing (equivalent to the old index.html
    // keeping both gate/app hidden — avoids flashing the login box when
    // refreshing with a valid token)
    return null
  }

  if (view === 'gate') {
    return (
      <>
        <Gate controller={controller} />
        <ToastHost />
        <ConfirmHost />
      </>
    )
  }

  return (
    <BlocksIOContext.Provider value={blocksIO}>
      <div
        id="app"
        className={
          'shell' +
          (sidebarCollapsed ? ' sidebar-collapsed' : '') +
          (rightPanelOpen ? ' panel-open' : '')
        }
      >
        {/* Narrow-screen drawer backdrop: visible only within the breakpoint;
            clicking it collapses the drawer */}
        <div
          className="sidebar-backdrop"
          aria-hidden="true"
          onClick={() => controller.dismissSidebarDrawer()}
        />
        <div className="app-body">
          <aside id="sidebar" className="sidebar">
            <Sidebar controller={controller} revealWs={revealWs} />
          </aside>
          <main className="main">
            <Header
              controller={controller}
              onRevealWorkspace={(wsId) =>
                setRevealWs((prev) => ({ wsId, seq: (prev?.seq || 0) + 1 }))
              }
            />
            {/* Zero-workspace onboarding state: fills the main area when no
                workspace exists */}
            {noWorkspace ? (
              <div id="no-workspace" className="no-workspace">
                <div className="no-workspace-card">
                  <div className="brand">◆ loom</div>
                  <h2>添加你的第一个工作区</h2>
                  <p className="no-workspace-desc">
                    工作区是 Loom 的代码上下文根目录——选定一个项目目录，即可开始对话。
                  </p>
                  <button
                    id="no-ws-add"
                    className="btn btn-primary"
                    type="button"
                    onClick={() => controller.openDirPicker()}
                  >
                    <Icon name="folder-plus" /> 添加工作区
                  </button>
                </div>
              </div>
            ) : (
              <>
                {/* Session tabs: hidden on the compare page (a separate
                    two-session context with its own chrome). */}
                {!!sessionId && mainView !== 'compare' && <SessionTabs controller={controller} />}
                {mainView === 'compare' ? (
                  <Suspense fallback={<div className="lazy-pane" aria-hidden="true" />}>
                    <CompareView controller={controller} />
                  </Suspense>
                ) : (
                  <>
                    {/* The chat tree stays mounted but hidden on other
                        tabs so its state survives the switch. */}
                    <div className="chat-pane" hidden={mainView !== 'chat'}>
                      <TranscriptSearch controller={controller.transcript} />
                      <TranscriptView
                        controller={controller.transcript}
                        io={transcriptIO}
                        scrollerOut={controller.scrollerRef}
                        loading={sessionLoading}
                        className={
                          connState !== 'live' && connState !== '' ? 'is-offline' : undefined
                        }
                        empty={{
                          hidden: !landingVisible,
                          hint: landingHint,
                          showAddWs: landingShowAddWs,
                          onAddWs: () => controller.openDirPicker(),
                        }}
                      />
                      <Composer controller={controller} />
                    </div>
                    {mainView === 'trace' && (
                      <Suspense fallback={<div className="lazy-pane" aria-hidden="true" />}>
                        <TracePage controller={controller} />
                      </Suspense>
                    )}
                    {mainView === 'maze' && (
                      <Suspense fallback={<div className="lazy-pane" aria-hidden="true" />}>
                        <MazePage controller={controller} />
                      </Suspense>
                    )}
                  </>
                )}
              </>
            )}
          </main>
          {/* Right workspace panel (changes/file tree): docked right of the session
              area, collapsible */}
          {rightPanelOpen && !noWorkspace && (
            <aside className="right-panel">
              <RightPanel controller={controller} />
            </aside>
          )}
        </div>

        <div id="banner" className="banner" role="alert" aria-live="assertive" hidden={!banner}>
          {banner && (
            <>
              <span>
                <Icon name="triangle-exclamation" /> {banner.text}
              </span>
              <button
                className="banner-retry"
                type="button"
                onClick={() => void controller.resync('manual_retry')}
              >
                立即重试
              </button>
            </>
          )}
        </div>
        <StatusBar controller={controller} />
      </div>

      <SettingsPanel controller={controller} />
      <DirPicker controller={controller} />
      <ToastHost />
      <ConfirmHost />
    </BlocksIOContext.Provider>
  )
}
