// DirPicker.tsx — directory browsing modal (add workspace): drills down level by level starting from $HOME;
// selecting a directory registers it. One-to-one with the dirPicker logic in the old main.js.

import { memo, useCallback, useEffect, useRef, useState } from 'react'
import type { AppController } from '../app/controller'
import type { DirEntry } from '../protocol/types'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import { toast } from './ui/Toast'
import { ApiError } from '../protocol/api'

interface BrowseState {
  path: string
  parent: string
  home: string
  entries: DirEntry[]
}

export const DirPicker = memo(function DirPicker({ controller }: { controller: AppController }) {
  const open = useStore(controller.store, (s) => s.dirPickerOpen)
  const [state, setState] = useState<BrowseState>({ path: '', parent: '', home: '', entries: [] })
  const [loading, setLoading] = useState(false)
  // seq guard: on rapid consecutive clicks on directories/breadcrumbs, only the response of the latest browse is honored, and loading never falls back to an older request.
  const browseSeq = useRef(0)

  const browseDir = useCallback(
    async (path: string) => {
      const seq = ++browseSeq.current
      setLoading(true)
      try {
        const r = await controller.api.browseDirectories(path)
        if (seq !== browseSeq.current) return
        setState({
          path: r.path,
          parent: r.parent || '',
          home: r.home || '',
          entries: r.entries || [],
        })
      } catch (e) {
        if (seq !== browseSeq.current) return
        if ((e as ApiError).status !== 401) toast('浏览目录失败: ' + (e as Error).message)
      } finally {
        if (seq === browseSeq.current) setLoading(false)
      }
    },
    [controller],
  )

  useEffect(() => {
    if (open) void browseDir('')
  }, [open, browseDir])

  // Esc closes the modal + in-modal focus trap (Tab/Esc must not escape to the input behind it).
  const rootRef = useRef<HTMLDivElement | null>(null)
  useEffect(() => {
    if (!open) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation()
        controller.closeDirPicker()
        return
      }
      if (e.key !== 'Tab') return
      const root = rootRef.current
      if (!root) return
      const focusable = root.querySelectorAll<HTMLElement>(
        'button:not([disabled]), [tabindex]:not([tabindex="-1"])',
      )
      if (focusable.length === 0) return
      const first = focusable[0]
      const last = focusable[focusable.length - 1]
      if (e.shiftKey && document.activeElement === first) {
        e.preventDefault()
        last.focus()
      } else if (!e.shiftKey && document.activeElement === last) {
        e.preventDefault()
        first.focus()
      }
    }
    document.addEventListener('keydown', onKey, true)
    // Initial focus lands on the cancel button (safe default, consistent with Confirm)
    const t = setTimeout(() => {
      rootRef.current?.querySelector<HTMLElement>('#dir-cancel')?.focus()
    }, 0)
    return () => {
      document.removeEventListener('keydown', onKey, true)
      clearTimeout(t)
    }
  }, [open, controller])

  if (!open) return null

  // Breadcrumbs: split the current path into clickable segments; the root segment renders as ~ inside $HOME, otherwise as /.
  const crumbs: { label: string; path: string; current: boolean }[] = []
  {
    const inHome =
      !!state.home && (state.path === state.home || state.path.startsWith(state.home + '/'))
    const rootLabel = inHome ? '~' : '/'
    const rootPath = inHome ? state.home : '/'
    const rel = inHome ? state.path.slice(state.home.length) : state.path.slice(1)
    const parts = rel.split('/').filter(Boolean)
    crumbs.push({ label: rootLabel, path: rootPath, current: parts.length === 0 })
    let acc = rootPath
    parts.forEach((p, i) => {
      acc = (acc === '/' ? '' : acc) + '/' + p
      crumbs.push({ label: p, path: acc, current: i === parts.length - 1 })
    })
  }

  return (
    <div
      id="dir-modal"
      className="modal-wrap"
      ref={rootRef}
      onClick={(e) => {
        if (e.target === e.currentTarget) controller.closeDirPicker()
      }}
    >
      <div className="modal dir-modal" role="dialog" aria-modal="true" aria-labelledby="dir-title">
        <div className="modal-title" id="dir-title">
          选择工作区目录
          {loading && (
            // Visible feedback while a browse request is in flight (previously the old directory kept rendering while in flight, looking like the click had no effect)
            <span className="dir-loading" aria-hidden="true">
              <span className="spinner" />
            </span>
          )}
        </div>
        <div className="dir-current">
          <button
            id="dir-up"
            className="icon-btn"
            title="上一级"
            type="button"
            disabled={!state.parent}
            onClick={() => {
              if (state.parent) void browseDir(state.parent)
            }}
          >
            <Icon name="turn-up" />
          </button>
          <nav
            id="dir-path"
            className="dir-crumbs"
            aria-label="当前目录"
            ref={(nav) => {
              // Scroll the deepest level (the current directory) into view
              if (nav) nav.scrollLeft = nav.scrollWidth
            }}
          >
            {crumbs.map((c, i) => (
              <span key={c.path}>
                {i > 0 && <span className="dir-sep">/</span>}
                <button
                  type="button"
                  className={'dir-crumb' + (c.current ? ' is-current' : '')}
                  onClick={c.current ? undefined : () => void browseDir(c.path)}
                >
                  {c.label}
                </button>
              </span>
            ))}
          </nav>
        </div>
        <div id="dir-list" className="dir-list">
          {state.entries.length === 0 ? (
            <div className="dir-empty">（无子目录）</div>
          ) : (
            state.entries.map((e) => (
              <button
                key={e.path}
                className="dir-item"
                type="button"
                onClick={() => void browseDir(e.path)}
              >
                {e.name}
              </button>
            ))
          )}
        </div>
        <div className="modal-actions">
          <button
            id="dir-cancel"
            className="btn btn-secondary"
            type="button"
            onClick={() => controller.closeDirPicker()}
          >
            取消
          </button>
          <button
            id="dir-select"
            className="btn btn-primary"
            type="button"
            onClick={() => void controller.confirmDirPicker(state.path)}
          >
            选择此目录
          </button>
        </div>
      </div>
    </div>
  )
})
