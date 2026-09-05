// Palette.tsx — Cmd/Ctrl+K command palette: keyboard-first global navigation entry point.
// Two groups — session jumps (filtered by title/model/workspace substring) and common commands — merged into one list
// with unified filtering; ↑/↓ cycle the selection, Enter executes, Esc / clicking the backdrop closes. Open/close is driven by App's global hotkey.

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { Icon, type IconName } from '../lib/icons'
import { shortId } from '../lib/format'

// Number of recent sessions listed directly on an empty query; unlimited while filtering
const RECENT_LIMIT = 8

interface PaletteItem {
  key: string
  sect: '会话' | '命令'
  label: string
  sub?: string
  icon: IconName
  isActive?: boolean
  run: () => void
}

export const Palette = memo(function Palette({
  controller,
  onClose,
}: {
  controller: AppController
  onClose: () => void
}) {
  const sessions = useStore(controller.store, (s) => s.sessions)
  const workspaces = useStore(controller.store, (s) => s.workspaces)
  const activeId = useStore(controller.store, (s) => s.sessionId)
  const [query, setQuery] = useState('')
  const [sel, setSel] = useState(0)
  const listRef = useRef<HTMLDivElement>(null)

  const wsName = useCallback(
    (wsId: string) => workspaces.find((w) => w.id === wsId)?.name ?? '',
    [workspaces],
  )

  const items = useMemo<PaletteItem[]>(() => {
    const q = query.trim().toLowerCase()
    const exec = (fn: () => void) => () => {
      onClose()
      fn()
    }
    const sessItems: PaletteItem[] = sessions
      .filter((s) => {
        if (!q) return true
        const hay = `${s.title || ''}\n${s.model_name || ''}\n${wsName(s.workspace_id || '')}\n${s.id}`
        return hay.toLowerCase().includes(q)
      })
      .slice(0, q ? sessions.length : RECENT_LIMIT)
      .map((s): PaletteItem => ({
        key: 's:' + s.id,
        sect: '会话',
        label: s.title || shortId(s.id),
        sub:
          [wsName(s.workspace_id || ''), s.model_name || ''].filter(Boolean).join(' · ') ||
          undefined,
        icon: 'terminal',
        isActive: s.id === activeId,
        run: exec(() => controller.onSelectSession(s.id)),
      }))
    const targetWs = sessions.find((s) => s.id === activeId)?.workspace_id ?? ''
    const cmd = (
      key: string,
      label: string,
      icon: IconName,
      run: () => void,
      sub?: string,
    ): PaletteItem => ({ key, sect: '命令', label, icon, run, ...(sub ? { sub } : {}) })
    const commands = [
      cmd(
        'c:new',
        '新建会话',
        'file-document-plus',
        exec(() => controller.onNewSession(targetWs)),
        wsName(targetWs) || undefined,
      ),
      cmd(
        'c:chat',
        '切换到对话',
        'arrow-left',
        exec(() => controller.setMainView('chat')),
      ),
      cmd(
        'c:trace',
        '查看执行轨迹',
        'bars',
        exec(() => controller.setMainView('trace')),
      ),
      cmd(
        'c:maze',
        '查看执行迷宫',
        'chart-gantt',
        exec(() => controller.setMainView('maze')),
      ),
      cmd(
        'c:compare',
        '轨迹对比',
        'layer-group',
        exec(() => controller.openCompare()),
      ),
      cmd(
        'c:settings',
        '打开设置',
        'gear',
        exec(() => controller.openSettings()),
      ),
    ].filter((c) => !q || c.label.toLowerCase().includes(q))
    // Groups keep a fixed order: sessions first; either way, everything is concatenated into a flat list carrying sect tags
    return [...sessItems, ...commands]
  }, [query, sessions, activeId, controller, onClose, wsName])

  // Return to the first item after the query changes; clamp the selection when the list shrinks
  useEffect(() => {
    setSel((i) => (items.length === 0 ? 0 : Math.min(i, items.length - 1)))
  }, [items.length])

  // Scroll the selected item into view
  useEffect(() => {
    listRef.current?.querySelector(`[data-idx="${sel}"]`)?.scrollIntoView({ block: 'nearest' })
  }, [sel])

  const onKeyDown = (e: React.KeyboardEvent) => {
    switch (e.key) {
      case 'ArrowDown':
        e.preventDefault()
        if (items.length) setSel((i) => (i + 1) % items.length)
        break
      case 'ArrowUp':
        e.preventDefault()
        if (items.length) setSel((i) => (i - 1 + items.length) % items.length)
        break
      case 'Enter':
        e.preventDefault()
        items[sel]?.run()
        break
      case 'Escape':
        // Same convention as Confirm/DirPicker: the overlay consumes Esc itself and doesn't pass it through to the outer layer
        e.stopPropagation()
        onClose()
        break
    }
  }

  let lastSect = ''
  return (
    // eslint-disable-next-line jsx-a11y/no-static-element-interactions
    <div
      className="palette-wrap"
      onClick={(e) => {
        if (e.target === e.currentTarget) onClose()
      }}
    >
      <div className="palette" role="dialog" aria-modal="true" aria-label="命令面板">
        <input
          className="palette-input"
          type="text"
          placeholder="搜索会话或命令…"
          autoFocus
          value={query}
          aria-label="搜索会话或命令"
          aria-activedescendant={items.length ? `palette-item-${sel}` : undefined}
          onChange={(e) => {
            setQuery(e.target.value)
            setSel(0)
          }}
          onKeyDown={onKeyDown}
        />
        <div className="palette-list" role="listbox" ref={listRef} onKeyDown={onKeyDown}>
          {items.length === 0 && <div className="palette-empty">无匹配结果</div>}
          {items.map((it, i) => {
            const head = it.sect !== lastSect ? (lastSect = it.sect) : ''
            return (
              <div key={it.key}>
                {head && <div className="palette-sect">{head}</div>}
                <button
                  type="button"
                  id={`palette-item-${i}`}
                  data-idx={i}
                  role="option"
                  aria-selected={i === sel}
                  className={'palette-item' + (i === sel ? ' is-active' : '')}
                  onMouseEnter={() => setSel(i)}
                  onClick={it.run}
                >
                  <Icon name={it.icon} />
                  <span className="pi-label">{it.label}</span>
                  {it.isActive && <span className="pi-live">当前</span>}
                  {it.sub && <span className="pi-sub">{it.sub}</span>}
                </button>
              </div>
            )
          })}
        </div>
        <div className="palette-foot">
          <span>
            <kbd>↑↓</kbd> 选择
          </span>
          <span>
            <kbd>Enter</kbd> 执行
          </span>
          <span>
            <kbd>Esc</kbd> 关闭
          </span>
        </div>
      </div>
    </div>
  )
})
