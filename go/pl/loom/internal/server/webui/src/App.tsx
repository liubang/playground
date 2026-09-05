// App.tsx — app shell: gate / main UI (sidebar + top bar + message stream +
// composer + status bar) + global overlays (toast / confirm / settings /
// directory browser / banner).

import { lazy, Suspense, useCallback, useEffect, useMemo, useState } from 'react'
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
import { Palette } from './components/Palette'
// These heavy pages (maze / trace / compare) pull in big dependencies such as SVG rendering / maze data / diff
// highlighting: split them into separate chunks with React.lazy so the initial session page only loads the chat stream, not these.
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

  // Cmd/Ctrl+K: open/close the command palette (global hotkey; the palette itself handles Esc and internal navigation)
  const [paletteOpen, setPaletteOpen] = useState(false)
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault()
        setPaletteOpen((v) => !v)
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

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

  // Stable props: Header (memo)/TranscriptView must not re-render passively on unrelated App slice changes (conn
  // badge, landing state, etc.) — the previous inline arrows/objects were new references on every render.
  const onRevealWorkspace = useCallback(
    (wsId: string) => setRevealWs((prev) => ({ wsId, seq: (prev?.seq || 0) + 1 })),
    [],
  )
  const transcriptEmpty = useMemo(
    () => ({
      hidden: !landingVisible,
      hint: landingHint,
      showAddWs: landingShowAddWs,
      onAddWs: () => controller.openDirPicker(),
    }),
    [landingVisible, landingHint, landingShowAddWs, controller],
  )

  // Global shortcuts:
  // - Cmd/Ctrl+F opens in-session search (with virtual scrolling the browser's native Cmd+F can't find
  //   blocks outside the viewport, so this must take over). Only take over in the chat view; trace/maze lists aren't
  //   virtualized, so the browser's native search works there.
  // - Esc cancels the current turn. Yield order: confirm modal (handles it itself) → search bar (closes
  //   itself) → busy cancel; defaultPrevented means an inner layer already consumed it
  //   (e.g. completion-menu Esc, picker Esc), so it's not handled again.
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
      if (controller.store.get().settingsOpen) return // the settings panel has its own Esc (confirms when dirty)
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
            <Header controller={controller} onRevealWorkspace={onRevealWorkspace} />
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
                        empty={transcriptEmpty}
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
      {paletteOpen && <Palette controller={controller} onClose={() => setPaletteOpen(false)} />}
      <ToastHost />
      <ConfirmHost />
    </BlocksIOContext.Provider>
  )
}
