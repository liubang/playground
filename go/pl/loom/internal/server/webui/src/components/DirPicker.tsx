// DirPicker.tsx — 目录浏览弹窗（添加工作区）：从 $HOME 起逐级下钻，
// 选择目录即注册。与旧 main.js 的 dirPicker 逻辑一一对应。

import { memo, useCallback, useEffect, useState } from 'react'
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

  const browseDir = useCallback(
    async (path: string) => {
      try {
        const r = await controller.api.browseDirectories(path)
        setState({
          path: r.path,
          parent: r.parent || '',
          home: r.home || '',
          entries: r.entries || [],
        })
      } catch (e) {
        if ((e as ApiError).status !== 401) toast('浏览目录失败: ' + (e as Error).message)
      }
    },
    [controller],
  )

  useEffect(() => {
    if (open) void browseDir('')
  }, [open, browseDir])

  if (!open) return null

  // 面包屑：把当前路径拆成可点段，根段在 $HOME 内显示为 ~，否则为 /。
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
      onClick={(e) => {
        if (e.target === e.currentTarget) controller.closeDirPicker()
      }}
    >
      <div className="modal dir-modal" role="dialog" aria-modal="true" aria-labelledby="dir-title">
        <div className="modal-title" id="dir-title">
          选择工作区目录
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
              // 让最深一级（当前目录）滚入可视区
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
