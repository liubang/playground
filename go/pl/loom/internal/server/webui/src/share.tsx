// share.tsx — 分享页入口（/share/{token}）：公开只读渲染，复用主界面的
// Transcript 渲染管线；无 SSE、无 composer、无鉴权（token 即凭证）。

import { useEffect, useMemo, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import './app.css'
import { TranscriptController } from './app/transcript'
import { TranscriptView } from './components/TranscriptView'
import { MazeView } from './components/maze/MazeView'
import { BlocksIOContext, type ArtifactEntry } from './components/blocks/context'
import { Icon } from './lib/icons'
import type { MazeData, SharedView } from './protocol/types'

const THEME_KEY = 'loom_theme'

// 分享页 artifact 走公开端点（/v1/shared/* 免 bearer）；内容寻址 + 不可变，
// 按 id+size 缓存避免重复下载。
function makeArtifactFetcher(token: string) {
  const cache = new Map<string, ArtifactEntry>()
  return async (id: string, size: number): Promise<ArtifactEntry> => {
    const key = `${id}:${size}`
    const cached = cache.get(key)
    if (cached) return cached
    const res = await fetch(
      `/v1/shared/${encodeURIComponent(token)}/artifacts/${encodeURIComponent(id)}?size=${size}`,
    )
    if (!res.ok) throw new Error(`artifact fetch failed (HTTP ${res.status})`)
    const blob = await res.blob()
    const entry: ArtifactEntry = {
      url: URL.createObjectURL(blob),
      mediaType: blob.type || '',
      blob,
    }
    cache.set(key, entry)
    return entry
  }
}

function fmtTime(iso?: string): string {
  const d = new Date(iso || '')
  if (Number.isNaN(d.getTime())) return ''
  return d.toLocaleString()
}

// 回到顶部按钮：滚动离开顶部超过阈值时显示，点击平滑回到顶部。
const TOP_BTN_SHOW_PX = 400

function ShareApp() {
  const [view, setView] = useState<SharedView | null>(null)
  const [error, setError] = useState('')
  const [showTop, setShowTop] = useState(false)
  // Trace view: shared sessions can show the execution maze too
  // (read-only, no chat jump). The view rides the URL hash (#maze) so a
  // link copied while viewing the trace opens on the trace directly.
  const [showMaze, setShowMaze] = useState(() => location.hash === '#maze')
  const [maze, setMaze] = useState<MazeData | null>(null)
  const [mazeError, setMazeError] = useState('')
  const scrollerRef = useRef<{ el: HTMLDivElement | null }>({ el: null })

  // token 取自路径最后一段；格式非法（非 32 位 hex）直接算无效链接。
  const token = location.pathname.split('/').filter(Boolean).pop() || ''

  // Toggle and reflect the view into the hash (replaceState: no history
  // spam; clearing restores the bare share URL).
  const toggleMaze = () => {
    setShowMaze((v) => {
      const next = !v
      history.replaceState(null, '', next ? '#maze' : location.pathname)
      return next
    })
  }

  // 只读渲染：io 只提供 artifact 解析；无 sendFeedback → 不渲染投票按钮；
  // state 固定 "closed"，applySnapshot 会为最后一轮补齐操作行（复制/时间）。
  const transcript = useMemo(
    () =>
      new TranscriptController({
        resolveApproval: () => Promise.reject(new Error('read-only share view')),
        answerQuestion: () => Promise.reject(new Error('read-only share view')),
        onError: () => {},
      }),
    [],
  )
  const blocksIO = useMemo(() => ({ fetchArtifactURL: makeArtifactFetcher(token) }), [token])

  useEffect(() => {
    // 主题与主应用一致：默认深色，仅显式存了 "light" 才用浅色。
    const saved = sessionStorage.getItem(THEME_KEY)
    document.documentElement.dataset.theme = saved !== 'light' ? 'dark' : 'light'
  }, [])

  useEffect(() => {
    if (!/^[0-9a-f]{32}$/.test(token)) {
      setError('链接无效或已撤销。')
      return
    }
    void (async () => {
      let v: SharedView
      try {
        const res = await fetch(`/v1/shared/${encodeURIComponent(token)}`)
        if (res.status === 404) {
          setError('链接无效或已撤销。')
          return
        }
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        v = (await res.json()) as SharedView
      } catch (e) {
        setError('加载失败：' + (e as Error).message)
        return
      }
      setView(v)
      document.title = `${v.title || 'shared session'} · loom`
      transcript.applySnapshot({ messages: v.messages || [], state: 'closed' })
    })()
  }, [token, transcript])

  // Maze data loads lazily on first switch to the trace tab.
  useEffect(() => {
    if (!showMaze || maze !== null || mazeError) return
    void (async () => {
      try {
        const res = await fetch(`/v1/shared/${encodeURIComponent(token)}/maze`)
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        setMaze((await res.json()) as MazeData)
      } catch (e) {
        setMazeError((e as Error).message)
      }
    })()
  }, [showMaze, maze, mazeError, token])

  return (
    <>
      <header className="share-header">
        <span className="brand">◆ loom</span>
        <span id="share-title" className="share-title" title={view?.session_id || ''}>
          {error ? '' : view?.title || ''}
        </span>
        <button
          id="share-maze-toggle"
          className={'maze-btn' + (showMaze ? ' is-on' : '')}
          type="button"
          title={showMaze ? '返回对话记录' : '查看执行轨迹迷宫'}
          hidden={!!error}
          onClick={toggleMaze}
        >
          <Icon name="layer-group" /> {showMaze ? '对话' : '轨迹'}
        </button>
        <span className="spacer" />
        <span id="share-meta" className="share-meta">
          {!error && view?.updated_at ? `更新于 ${fmtTime(view.updated_at)}` : ''}
        </span>
      </header>

      {showMaze ? (
        <div className="maze-page">
          {mazeError ? (
            <div className="maze-error">轨迹加载失败：{mazeError}</div>
          ) : !maze ? (
            <div className="maze-empty">正在构建轨迹…</div>
          ) : maze.lanes.length === 0 || maze.lanes[0].stats.steps === 0 ? (
            <div className="maze-empty">该会话暂无执行轨迹</div>
          ) : (
            <MazeView data={maze} />
          )}
        </div>
      ) : (
        <BlocksIOContext.Provider value={blocksIO}>
          <TranscriptView
            controller={transcript}
            io={{}}
            className="share-transcript"
            scrollerOut={scrollerRef.current}
          >
            <div id="share-error" className="share-error" hidden={!error}>
              <div className="brand">◆ loom</div>
              <p id="share-error-text">{error || '链接无效或已撤销。'}</p>
            </div>
          </TranscriptView>
        </BlocksIOContext.Provider>
      )}

      <button
        id="share-top"
        className="share-top"
        type="button"
        title="back to top"
        hidden={!showTop}
        onClick={() => scrollerRef.current.el?.scrollTo({ top: 0, behavior: 'smooth' })}
      >
        <Icon name="arrow-up" />
      </button>
      {/* 滚动监听：离开顶部超过阈值时显示回顶按钮 */}
      <ScrollWatcher scrollerRef={scrollerRef.current} onChange={setShowTop} />

      <footer className="share-footer">shared session · read-only</footer>
    </>
  )
}

function ScrollWatcher({
  scrollerRef,
  onChange,
}: {
  scrollerRef: { el: HTMLDivElement | null }
  onChange: (show: boolean) => void
}) {
  useEffect(() => {
    const el = scrollerRef.el
    if (!el) return
    const update = () => onChange(el.scrollTop >= TOP_BTN_SHOW_PX)
    el.addEventListener('scroll', update, { passive: true })
    update()
    return () => el.removeEventListener('scroll', update)
  }, [scrollerRef, onChange])
  return null
}

createRoot(document.getElementById('root')!).render(<ShareApp />)
