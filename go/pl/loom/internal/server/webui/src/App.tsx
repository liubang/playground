// App.tsx — 应用外壳：gate / 主界面（侧栏 + 顶行 + 消息流 + composer +
// 状态栏）+ 全局浮层（toast / confirm / 设置 / 目录浏览 / banner）。

import { useMemo, useState } from 'react'
import type { AppController } from './app/controller'
import { useStore } from './store/store'
import { Gate } from './components/Gate'
import { Header } from './components/Header'
import { Sidebar } from './components/Sidebar'
import { TranscriptView, type TranscriptViewIO } from './components/TranscriptView'
import { Composer } from './components/Composer'
import { StatusBar } from './components/StatusBar'
import { DirPicker } from './components/DirPicker'
import { SettingsPanel } from './components/settings/SettingsPanel'
import { MazePage } from './components/maze/MazePage'
import { CompareView } from './components/maze/CompareView'
import { TracePage } from './components/trace/TraceView'
import { SessionTabs } from './components/SessionTabs'
import { ToastHost } from './components/ui/Toast'
import { ConfirmHost } from './components/ui/Confirm'
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
  const sessionId = useStore(controller.store, (s) => s.sessionId)
  const [revealWs, setRevealWs] = useState<{ wsId: string; seq: number } | null>(null)

  // TranscriptView 的交互回调（稳定引用：controller 方法均为绑定箭头函数）
  const transcriptIO = useMemo<TranscriptViewIO>(
    () => ({
      onResolveApproval: (approvalId, decision, always) =>
        void controller.transcript.resolveApproval(approvalId, decision, always),
      onAnswerQuestion: (questionId, answer) =>
        void controller.transcript.answerQuestion(questionId, answer, answer.skipped),
      onFeedback: (runId, value) => controller.transcript.sendFeedback(runId, value),
      fetchToolOutput: controller.fetchToolOutput,
    }),
    [controller],
  )

  const blocksIO = useMemo(() => ({ fetchArtifactURL: controller.fetchArtifactURL }), [controller])

  if (view === 'boot') {
    // 鉴权校验中：什么都不渲染（旧版 index.html 里 gate/app 均 hidden 的等价行为，
    // 避免持有效 token 刷新时闪现登录框）
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
      <div id="app" className={'shell' + (sidebarCollapsed ? ' sidebar-collapsed' : '')}>
        {/* 窄屏抽屉遮罩：仅断点内显示，点击收起抽屉 */}
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
            {/* 零工作区引导态：无任何工作区时占据主区域 */}
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
                  <CompareView controller={controller} />
                ) : (
                  <>
                    {/* The chat tree stays mounted but hidden on other
                        tabs so its state survives the switch. */}
                    <div className="chat-pane" hidden={mainView !== 'chat'}>
                      <TranscriptView
                        controller={controller.transcript}
                        io={transcriptIO}
                        scrollerOut={controller.scrollerRef}
                        empty={{
                          hidden: !landingVisible,
                          hint: landingHint,
                          showAddWs: landingShowAddWs,
                          onAddWs: () => controller.openDirPicker(),
                        }}
                      />
                      <Composer controller={controller} />
                    </div>
                    {mainView === 'trace' && <TracePage controller={controller} />}
                    {mainView === 'maze' && <MazePage controller={controller} />}
                  </>
                )}
              </>
            )}
          </main>
        </div>

        <div id="banner" className="banner" hidden={!banner}>
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
