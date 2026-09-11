// RightPanel.tsx — right-side workspace panel.
//
// Tabs: Changes (git working-tree status + inline diff) and Files (searchable,
// lazy-loaded file tree + preview). Both tabs share a single git-status query
// owned by RightPanel, so the changes count badge and the tree's per-file git
// colouring come from one fetch.
//
// Data principle: git is the source of truth for changes (covers both agent
// edits and the user's own); transcript tool.completed / turn.finished events
// only bump gitStamp (invalidation). Same for the file tree: on gitStamp the
// expanded directories are revalidated (ETag → 304 keeps the cache warm).
// The panel is read-only — no write/commit entry points.

import {
  memo,
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
  type MouseEvent as ReactMouseEvent,
  type PointerEvent as ReactPointerEvent,
} from 'react'
import type { AppController } from '../../app/controller'
import { useStore } from '../../store/store'
import { Icon } from '../../lib/icons'
import { langFromPath } from '../../lib/diff'
import { highlightToHtml } from '../../lib/markdown'
import { copyText } from '../../lib/format'
import { toast } from '../ui/Toast'
import { DiffView } from '../blocks/DiffView'
import { VirtualList, type VirtualListHandle } from './VirtualList'
import type {
  GitFileEntry,
  WorkspaceFileContent,
  WorkspaceFileEntry,
  WorkspaceFileMatch,
  WorkspaceGitDiff,
  WorkspaceGitStatus,
} from '../../protocol/types'

// Fixed row heights the virtualizer depends on.
const TREE_ROW_H = 26
const SEARCH_ROW_H = 34
const CHANGE_ROW_H = 40

// The panel binds to the current session's workspace; falls back to the default
// workspace when there is no session.
function usePanelWorkspace(controller: AppController): string {
  const sessionWs = useStore(
    controller.store,
    (s) => s.sessions.find((x) => x.id === s.sessionId)?.workspace_id || '',
  )
  const defaultWs = useStore(
    controller.store,
    (s) => s.workspaces.find((w) => w.is_default)?.id || s.workspaces[0]?.id || '',
  )
  return sessionWs || defaultWs
}

export function RightPanel({ controller }: { controller: AppController }) {
  const tab = useStore(controller.store, (s) => s.rightPanelTab)
  const wsId = usePanelWorkspace(controller)
  const gitStamp = useStore(controller.store, (s) => s.gitStamp)

  // Shared git status: one query feeds the changes tab, its badge and the file
  // tree's colouring.
  const [git, setGit] = useState<WorkspaceGitStatus | null>(null)
  const [gitError, setGitError] = useState('')
  const seq = useRef(0)
  const wsRef = useRef(wsId)
  wsRef.current = wsId

  const reloadGit = useCallback(async () => {
    const forWs = wsId
    const s = ++seq.current
    try {
      const d = await controller.api.workspaceGitStatus(wsId)
      if (s !== seq.current || wsRef.current !== forWs) return
      setGit(d)
      setGitError('')
    } catch (e) {
      if (s !== seq.current || wsRef.current !== forWs) return
      setGitError((e as Error).message)
    }
  }, [controller, wsId])

  useEffect(() => {
    setGit(null)
    setGitError('')
  }, [wsId])

  useEffect(() => {
    if (!wsId) return
    void reloadGit()
  }, [reloadGit, gitStamp, wsId])

  const changeCount = git?.is_git ? git.files?.length || 0 : 0

  // Drag-to-resize: width is a user preference layered over the responsive CSS
  // default (null → CSS controls it). Double-click restores the default.
  const onResizeStart = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      e.preventDefault()
      const panel = e.currentTarget.parentElement
      const startW = panel ? panel.getBoundingClientRect().width : 360
      const startX = e.clientX
      const onMove = (ev: PointerEvent) =>
        controller.setRightPanelWidth(startW - (ev.clientX - startX))
      const onUp = () => {
        window.removeEventListener('pointermove', onMove)
        window.removeEventListener('pointerup', onUp)
        document.body.classList.remove('rp-resizing')
      }
      document.body.classList.add('rp-resizing')
      window.addEventListener('pointermove', onMove)
      window.addEventListener('pointerup', onUp)
    },
    [controller],
  )

  return (
    <div className="rp">
      <div
        className="rp-resizer"
        role="separator"
        aria-orientation="vertical"
        aria-label="Resize workspace panel"
        title="Drag to resize · double-click to reset"
        onPointerDown={onResizeStart}
        onDoubleClick={() => controller.resetRightPanelWidth()}
      />
      <div className="rp-head">
        <div className="rp-tabs" role="tablist" aria-label="Workspace panel">
          <button
            type="button"
            role="tab"
            id="rp-tab-changes"
            aria-selected={tab === 'changes'}
            className={'rp-tab' + (tab === 'changes' ? ' is-active' : '')}
            onClick={() => controller.setRightPanelTab('changes')}
          >
            Changes
            {changeCount > 0 && <span className="rp-tab-badge">{changeCount}</span>}
          </button>
          <button
            type="button"
            role="tab"
            id="rp-tab-files"
            aria-selected={tab === 'files'}
            className={'rp-tab' + (tab === 'files' ? ' is-active' : '')}
            onClick={() => controller.setRightPanelTab('files')}
          >
            Files
          </button>
        </div>
        <button
          type="button"
          className="icon-btn"
          title="Collapse panel (⌘B)"
          onClick={() => controller.toggleRightPanel()}
        >
          <Icon name="xmark" />
        </button>
      </div>
      {!wsId ? (
        <div className="rp-empty">No workspace</div>
      ) : tab === 'changes' ? (
        <ChangesPane
          controller={controller}
          wsId={wsId}
          git={git}
          error={gitError}
          reload={reloadGit}
        />
      ) : (
        <FilesPane controller={controller} wsId={wsId} git={git} />
      )}
    </div>
  )
}

// ---------- shared bits ----------

function splitPath(p: string): [string, string] {
  const i = p.lastIndexOf('/')
  return i < 0 ? [p, ''] : [p.slice(i + 1), p.slice(0, i)]
}

const STATUS_LABEL: Record<string, string> = {
  M: 'Modified',
  A: 'Added',
  D: 'Deleted',
  R: 'Renamed',
  T: 'Type changed',
  U: 'Untracked',
}

// git status → CSS class suffix (colour-coded names in the tree / changes list).
function statusClass(status?: string): string {
  switch ((status || '').toUpperCase()) {
    case 'M':
      return 'is-m'
    case 'A':
    case 'U':
    case '?':
      return 'is-a'
    case 'D':
      return 'is-d'
    default:
      return ''
  }
}

// File-type icon tint: a coarse category is enough to scan a tree quickly.
const EXT_CATEGORY: Record<string, string> = {
  go: 'code',
  rs: 'code',
  py: 'code',
  rb: 'code',
  java: 'code',
  kt: 'code',
  scala: 'code',
  c: 'code',
  h: 'code',
  cc: 'code',
  cpp: 'code',
  cxx: 'code',
  hpp: 'code',
  cs: 'code',
  js: 'code',
  mjs: 'code',
  cjs: 'code',
  jsx: 'code',
  ts: 'code',
  tsx: 'code',
  swift: 'code',
  php: 'code',
  lua: 'code',
  pl: 'code',
  r: 'code',
  sh: 'code',
  bash: 'code',
  zsh: 'code',
  json: 'data',
  yml: 'data',
  yaml: 'data',
  toml: 'data',
  ini: 'data',
  xml: 'data',
  csv: 'data',
  tsv: 'data',
  sql: 'data',
  proto: 'data',
  md: 'doc',
  markdown: 'doc',
  txt: 'doc',
  rst: 'doc',
  adoc: 'doc',
  html: 'web',
  htm: 'web',
  css: 'web',
  scss: 'web',
  less: 'web',
  vue: 'web',
  svelte: 'web',
  png: 'img',
  jpg: 'img',
  jpeg: 'img',
  gif: 'img',
  svg: 'img',
  webp: 'img',
  ico: 'img',
}

function fileKindClass(name: string): string {
  const i = name.lastIndexOf('.')
  const ext = i > 0 ? name.slice(i + 1).toLowerCase() : ''
  const cat = EXT_CATEGORY[ext]
  return cat ? 'ft-ico-' + cat : 'ft-ico-generic'
}

interface MenuItem {
  label: string
  run: () => void
}

// Lightweight read-only context menu (copy paths / open in panel).
function ContextMenu({
  x,
  y,
  items,
  onClose,
}: {
  x: number
  y: number
  items: MenuItem[]
  onClose: () => void
}) {
  useEffect(() => {
    const close = () => onClose()
    window.addEventListener('click', close)
    window.addEventListener('keydown', close)
    window.addEventListener('scroll', close, true)
    return () => {
      window.removeEventListener('click', close)
      window.removeEventListener('keydown', close)
      window.removeEventListener('scroll', close, true)
    }
  }, [onClose])
  return (
    <div
      className="rp-menu"
      style={{ left: x, top: y }}
      role="menu"
      onClick={(e) => e.stopPropagation()}
    >
      {items.map((it) => (
        <button
          key={it.label}
          type="button"
          role="menuitem"
          className="rp-menu-item"
          onClick={() => {
            it.run()
            onClose()
          }}
        >
          {it.label}
        </button>
      ))}
    </div>
  )
}

async function copyToClipboard(text: string, label: string) {
  if (await copyText(text)) toast(label, true)
  else toast('Copy failed — clipboard unavailable')
}

// ---------- Changes tab ----------

type ChangeRow =
  { kind: 'head'; label: string; count: number } | { kind: 'file'; file: GitFileEntry }

const ChangeFileButton = memo(function ChangeFileButton({
  file,
  open,
  onToggle,
  onContext,
}: {
  file: GitFileEntry
  open: boolean
  onToggle: () => void
  onContext: (e: ReactMouseEvent) => void
}) {
  const [name, dir] = splitPath(file.path)
  const status = file.status || 'M'
  return (
    <button
      type="button"
      className={'gf-row' + (open ? ' is-open' : '')}
      title={file.path}
      onClick={onToggle}
      onContextMenu={onContext}
    >
      <span
        className={'gf-badge st-' + status.toLowerCase()}
        title={STATUS_LABEL[status] || status}
      >
        {status}
      </span>
      <span className="gf-path">
        <span className="gf-name">{name}</span>
        {dir && <span className="gf-dir">{dir}</span>}
      </span>
      {!file.no_stat && (
        <span className="gf-stats mono">
          {(file.adds ?? 0) > 0 && <span className="st-add">+{file.adds}</span>}
          {(file.dels ?? 0) > 0 && <span className="st-del">−{file.dels}</span>}
        </span>
      )}
    </button>
  )
})

const ChangesPane = memo(function ChangesPane({
  controller,
  wsId,
  git,
  error,
  reload,
}: {
  controller: AppController
  wsId: string
  git: WorkspaceGitStatus | null
  error: string
  reload: () => void
}) {
  const [openPath, setOpenPath] = useState('')
  const [filter, setFilter] = useState('')
  const [diffs, setDiffs] = useState<Record<string, WorkspaceGitDiff>>({})
  const [diffErrors, setDiffErrors] = useState<Record<string, string>>({})
  const [menu, setMenu] = useState<{ x: number; y: number; path: string } | null>(null)

  // In-flight diff responses use this to detect a workspace switch.
  const wsIdRef = useRef(wsId)
  useEffect(() => {
    wsIdRef.current = wsId
  }, [wsId])

  const fetchDiff = useCallback(
    (path: string) => {
      controller.api
        .workspaceGitDiff(wsId, path)
        .then((d) => {
          if (wsIdRef.current !== wsId) return
          setDiffs((m) => ({ ...m, [path]: d }))
        })
        .catch((e) => {
          if (wsIdRef.current !== wsId) return
          setDiffErrors((m) => ({ ...m, [path]: (e as Error).message }))
        })
    },
    [controller, wsId],
  )

  // git is refetched per gitStamp; a changed working tree invalidates the
  // cached diffs (otherwise an expanded file shows its pre-edit diff forever).
  useEffect(() => {
    setDiffs({})
    setDiffErrors({})
    if (openPath) fetchDiff(openPath)
  }, [git]) // eslint-disable-line react-hooks/exhaustive-deps

  // Workspace switch: clear expansion + diff cache.
  useEffect(() => {
    setOpenPath('')
    setDiffs({})
    setDiffErrors({})
    setFilter('')
  }, [wsId])

  const toggleFile = (path: string) => {
    if (openPath === path) {
      setOpenPath('')
      return
    }
    setOpenPath(path)
    if (diffs[path] || diffErrors[path]) return
    fetchDiff(path)
  }

  const files = git?.files || []
  const q = filter.trim().toLowerCase()
  const shown = useMemo(
    () => (q ? files.filter((f) => f.path.toLowerCase().includes(q)) : files),
    [files, q],
  )
  const staged = useMemo(() => shown.filter((f) => f.staged && !f.unstaged), [shown])
  const unstaged = useMemo(() => shown.filter((f) => !(f.staged && !f.unstaged)), [shown])

  const rows = useMemo<ChangeRow[]>(() => {
    const out: ChangeRow[] = []
    if (staged.length) {
      out.push({ kind: 'head', label: 'Staged', count: staged.length })
      for (const f of staged) out.push({ kind: 'file', file: f })
    }
    if (unstaged.length) {
      out.push({ kind: 'head', label: 'Changes', count: unstaged.length })
      for (const f of unstaged) out.push({ kind: 'file', file: f })
    }
    return out
  }, [staged, unstaged])

  const menuItems = useCallback(
    (path: string): MenuItem[] => {
      const root = controller.store
        .get()
        .workspaces.find((w) => w.id === wsId)
        ?.root_path?.replace(/\/+$/, '')
      const items: MenuItem[] = [
        { label: 'Open in file panel', run: () => controller.revealFileInPanel(path) },
        { label: 'Copy relative path', run: () => void copyToClipboard(path, 'Copied path') },
      ]
      if (root) {
        items.push({
          label: 'Copy absolute path',
          run: () => void copyToClipboard(root + '/' + path, 'Copied absolute path'),
        })
      }
      return items
    },
    [controller, wsId],
  )

  if (error) return <div className="rp-empty">Failed to load: {error}</div>
  if (!git) return <div className="rp-empty">Loading…</div>
  if (!git.is_git)
    return <div className="rp-empty">The current workspace is not a git repository</div>

  const virtualize = rows.length > 60 && !openPath

  const renderChangeRow = (r: ChangeRow, key: string) => {
    if (r.kind === 'head') {
      return (
        <div className="gc-head" key={key}>
          <span>{r.label}</span>
          <span className="gc-count">{r.count}</span>
        </div>
      )
    }
    const f = r.file
    const open = openPath === f.path
    return (
      <div className={'gf' + (open ? ' is-open' : '')} key={key}>
        <ChangeFileButton
          file={f}
          open={open}
          onToggle={() => toggleFile(f.path)}
          onContext={(e) => {
            e.preventDefault()
            setMenu({
              x: Math.min(e.clientX, window.innerWidth - 200),
              y: Math.min(e.clientY, window.innerHeight - 90),
              path: f.path,
            })
          }}
        />
        {open && (
          <div className="gf-diff">
            {diffErrors[f.path] ? (
              <div className="rp-empty">Failed to load diff: {diffErrors[f.path]}</div>
            ) : !diffs[f.path] ? (
              <div className="rp-empty">Loading…</div>
            ) : diffs[f.path].is_dir ? (
              <div className="rp-empty">New directory (no diff to show)</div>
            ) : diffs[f.path].diff ? (
              <>
                <DiffView diffText={diffs[f.path].diff!} />
                {diffs[f.path].truncated && <div className="notice">Diff too large; truncated</div>}
              </>
            ) : (
              <div className="rp-empty">No content changes (mode or rename only)</div>
            )}
          </div>
        )}
      </div>
    )
  }

  return (
    <>
      <div className="rp-subhead">
        <span className="rp-branch mono" title="Current branch">
          {git.branch || 'HEAD'}
        </span>
        <span className="gf-stats mono">
          {(git.adds ?? 0) > 0 && <span className="st-add">+{git.adds}</span>}
          {(git.dels ?? 0) > 0 && <span className="st-del">−{git.dels}</span>}
        </span>
        {openPath && (
          <button
            type="button"
            className="icon-btn"
            title="Collapse all diffs"
            onClick={() => setOpenPath('')}
          >
            <Icon name="box-archive" />
          </button>
        )}
        <button type="button" className="icon-btn" title="Refresh" onClick={reload}>
          <Icon name="rotate-left" />
        </button>
      </div>
      {files.length > 0 && (
        <div className="rp-filter">
          <Icon name="magnifying-glass" />
          <input
            type="text"
            value={filter}
            placeholder="Filter changed files…"
            aria-label="Filter changed files"
            onChange={(e) => setFilter(e.target.value)}
          />
          {filter && (
            <button
              type="button"
              className="rp-filter-clear"
              title="Clear"
              onClick={() => setFilter('')}
            >
              <Icon name="xmark" />
            </button>
          )}
        </div>
      )}
      {files.length === 0 ? (
        <div className="rp-empty">Working tree clean — no uncommitted changes</div>
      ) : rows.length === 0 ? (
        <div className="rp-empty">No changed files match “{filter}”</div>
      ) : virtualize ? (
        <VirtualList
          className="rp-body vlist"
          count={rows.length}
          itemHeight={CHANGE_ROW_H}
          tabIndex={0}
          id="rp-panel"
          role="tabpanel"
          ariaLabelledBy="rp-tab-changes"
          ariaLabel="Changed files"
          render={(i) => renderChangeRow(rows[i], String(i))}
        />
      ) : (
        <div className="rp-body" id="rp-panel" role="tabpanel" aria-labelledby="rp-tab-changes">
          {rows.map((r, i) => renderChangeRow(r, r.kind === 'file' ? r.file.path : 'h' + i))}
        </div>
      )}
      {menu && (
        <ContextMenu
          x={menu.x}
          y={menu.y}
          items={menuItems(menu.path)}
          onClose={() => setMenu(null)}
        />
      )}
    </>
  )
})

// ---------- Files tab ----------

interface TreeRow {
  entry: WorkspaceFileEntry
  depth: number
}

function flattenTree(
  tree: Record<string, WorkspaceFileEntry[]>,
  expanded: ReadonlySet<string>,
): TreeRow[] {
  const out: TreeRow[] = []
  const walk = (path: string, depth: number) => {
    const entries = tree[path]
    if (!entries) return
    for (const e of entries) {
      out.push({ entry: e, depth })
      if (e.kind === 'dir' && expanded.has(e.path)) walk(e.path, depth + 1)
    }
  }
  walk('', 0)
  return out
}

const FilesPane = memo(function FilesPane({
  controller,
  wsId,
  git,
}: {
  controller: AppController
  wsId: string
  git: WorkspaceGitStatus | null
}) {
  const gitStamp = useStore(controller.store, (s) => s.gitStamp)
  const request = useStore(controller.store, (s) => s.panelRequest)
  const rootPath = useStore(
    controller.store,
    (s) => s.workspaces.find((w) => w.id === wsId)?.root_path || '',
  )

  // tree: dir path → children ("" is the root); only expanded directories are
  // cached — lazy loading. etags hold the last validator per directory so an
  // unchanged directory revalidates to 304.
  const [tree, setTree] = useState<Record<string, WorkspaceFileEntry[]>>({})
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set())
  const [preview, setPreview] = useState('')
  const [error, setError] = useState('')
  const [showHidden, setShowHidden] = useState(() => localStorage.getItem('loom_rp_hidden') === '1')
  const [query, setQuery] = useState('')
  const [results, setResults] = useState<WorkspaceFileMatch[]>([])
  const [cursor, setCursor] = useState(0)
  const [menu, setMenu] = useState<{ x: number; y: number; path: string; isDir: boolean } | null>(
    null,
  )
  const etags = useRef<Record<string, string>>({})

  const wsRef = useRef(wsId)
  wsRef.current = wsId
  const searchSeq = useRef(0)
  const vlistRef = useRef<VirtualListHandle>(null)

  // Concurrent duplicate loads of the same directory (mount effect + gitStamp
  // bump + reveal racing) collapse into one fetch — the shared promise keeps
  // each caller informed without tripling identical requests.
  const inFlight = useRef(new Map<string, Promise<void>>())
  const loadDir = useCallback(
    (path: string): Promise<void> => {
      const pending = inFlight.current.get(path)
      if (pending) return pending
      const forWs = wsId
      const etag = etags.current[path] || ''
      const task = (async () => {
        try {
          const res = await controller.api.listWorkspaceFiles(wsId, path, showHidden, etag)
          if (wsRef.current !== forWs) return
          if (res.notModified) return // cache still valid — no re-render
          etags.current[path] = res.etag
          setTree((m) => ({ ...m, [path]: res.data?.entries || [] }))
          setError('')
        } catch (e) {
          if (wsRef.current !== forWs) return
          setError((e as Error).message)
        }
      })().finally(() => {
        if (inFlight.current.get(path) === task) inFlight.current.delete(path)
      })
      inFlight.current.set(path, task)
      return task
    },
    [controller, wsId, showHidden],
  )

  // Workspace switch / hidden-files toggle: rebuild the whole tree.
  useEffect(() => {
    etags.current = {}
    setTree({})
    setExpanded(new Set())
    setPreview('')
    setCursor(0)
    void loadDir('')
  }, [wsId, showHidden]) // eslint-disable-line react-hooks/exhaustive-deps

  // gitStamp: revalidate expanded directories (new/deleted files appear;
  // unchanged directories 304 and keep their cached subtree).
  useEffect(() => {
    for (const dir of expanded) void loadDir(dir)
    void loadDir('')
  }, [gitStamp]) // eslint-disable-line react-hooks/exhaustive-deps

  // Jump request from the transcript: open the file and reveal its ancestors.
  useEffect(() => {
    if (!request) return
    const p = request.path
    setQuery('')
    setPreview(p)
    setCursor(0)
    const parts = p.split('/')
    parts.pop()
    const dirs: string[] = []
    let acc = ''
    for (const seg of parts) {
      acc = acc ? acc + '/' + seg : seg
      dirs.push(acc)
    }
    if (dirs.length) {
      setExpanded((prev) => new Set([...prev, ...dirs]))
      for (const d of dirs) void loadDir(d)
    }
  }, [request]) // eslint-disable-line react-hooks/exhaustive-deps

  // Debounced fuzzy search (flat result list replaces the tree while active).
  useEffect(() => {
    const q = query.trim()
    if (!q) {
      setResults([])
      return
    }
    const seq = ++searchSeq.current
    const t = setTimeout(() => {
      controller.api
        .searchWorkspaceFiles(wsId, q)
        .then((r) => {
          if (seq !== searchSeq.current) return
          setResults(r.matches || [])
          setCursor(0)
        })
        .catch(() => {
          if (seq === searchSeq.current) setResults([])
        })
    }, 150)
    return () => clearTimeout(t)
  }, [query, wsId, controller])

  const toggleDir = useCallback(
    (path: string) => {
      setExpanded((prev) => {
        const next = new Set(prev)
        if (next.has(path)) {
          next.delete(path)
        } else {
          next.add(path)
          if (!tree[path]) void loadDir(path)
        }
        return next
      })
    },
    [tree, loadDir],
  )

  // git colouring: changed leaves + the directories that contain them.
  const statusByPath = useMemo(() => {
    const m = new Map<string, string>()
    for (const f of git?.files || []) m.set(f.path, f.status || 'M')
    return m
  }, [git])
  const dirtyDirs = useMemo(() => {
    const s = new Set<string>()
    for (const f of git?.files || []) {
      const parts = f.path.split('/')
      parts.pop()
      let acc = ''
      for (const seg of parts) {
        acc = acc ? acc + '/' + seg : seg
        s.add(acc)
      }
    }
    return s
  }, [git])

  const treeRows = useMemo(() => flattenTree(tree, expanded), [tree, expanded])

  // Rows driving the virtualizer / keyboard cursor: search results when
  // searching, otherwise the flattened tree.
  const searching = query.trim().length > 0
  const rowCount = searching ? results.length : treeRows.length

  useEffect(() => {
    setCursor((c) => (rowCount === 0 ? 0 : Math.min(c, rowCount - 1)))
  }, [rowCount])
  useEffect(() => {
    vlistRef.current?.scrollToIndex(cursor)
  }, [cursor])

  const openRow = useCallback(
    (index: number) => {
      if (searching) {
        const m = results[index]
        if (m) setPreview(m.path)
        return
      }
      const row = treeRows[index]
      if (!row) return
      if (row.entry.kind === 'dir') toggleDir(row.entry.path)
      else setPreview(row.entry.path)
    },
    [searching, results, treeRows, toggleDir],
  )

  const onTreeKeyDown = useCallback(
    (e: ReactKeyboardEvent<HTMLDivElement>) => {
      if (!rowCount) return
      if (e.key === 'ArrowDown') {
        e.preventDefault()
        setCursor((c) => Math.min(rowCount - 1, c + 1))
      } else if (e.key === 'ArrowUp') {
        e.preventDefault()
        setCursor((c) => Math.max(0, c - 1))
      } else if (e.key === 'Enter') {
        e.preventDefault()
        openRow(cursor)
      } else if (e.key === 'ArrowRight' && !searching) {
        const row = treeRows[cursor]
        if (row?.entry.kind === 'dir' && !expanded.has(row.entry.path)) {
          e.preventDefault()
          toggleDir(row.entry.path)
        }
      } else if (e.key === 'ArrowLeft' && !searching) {
        const row = treeRows[cursor]
        if (row?.entry.kind === 'dir' && expanded.has(row.entry.path)) {
          e.preventDefault()
          toggleDir(row.entry.path)
        }
      }
    },
    [rowCount, cursor, searching, treeRows, expanded, toggleDir, openRow],
  )

  const menuItems = useCallback(
    (path: string, isDir: boolean): MenuItem[] => {
      const root = rootPath.replace(/\/+$/, '')
      const items: MenuItem[] = [
        { label: 'Copy relative path', run: () => void copyToClipboard(path, 'Copied path') },
      ]
      if (root) {
        items.push({
          label: 'Copy absolute path',
          run: () => void copyToClipboard(root + '/' + path, 'Copied absolute path'),
        })
      }
      if (!isDir) {
        items.unshift({ label: 'Preview', run: () => setPreview(path) })
      }
      return items
    },
    [rootPath],
  )

  const toggleHidden = () => {
    const next = !showHidden
    setShowHidden(next)
    localStorage.setItem('loom_rp_hidden', next ? '1' : '0')
  }

  if (preview) {
    return (
      <FilePreview
        controller={controller}
        wsId={wsId}
        path={preview}
        onBack={() => setPreview('')}
      />
    )
  }

  const renderTreeRow = (row: TreeRow, i: number) => {
    const e = row.entry
    const isDir = e.kind === 'dir'
    const isOpen = expanded.has(e.path)
    const st = statusByPath.get(e.path)
    const cls =
      'ft-row' +
      (i === cursor ? ' is-cursor' : '') +
      (isDir && dirtyDirs.has(e.path) ? ' is-dirty' : '')
    return (
      <div
        key={e.path}
        className={cls}
        style={{ paddingLeft: 8 }}
        title={e.path}
        role="treeitem"
        aria-level={row.depth + 1}
        aria-expanded={isDir ? isOpen : undefined}
        onClick={() => (isDir ? toggleDir(e.path) : setPreview(e.path))}
        onContextMenu={(ev) => {
          ev.preventDefault()
          setMenu({
            x: Math.min(ev.clientX, window.innerWidth - 210),
            y: Math.min(ev.clientY, window.innerHeight - 110),
            path: e.path,
            isDir,
          })
        }}
      >
        {row.depth > 0 && (
          <span className="ft-guides" aria-hidden="true">
            {Array.from({ length: row.depth }).map((_, d) => (
              <span className="ft-guide" key={d} />
            ))}
          </span>
        )}
        {isDir ? (
          <>
            <Icon name={isOpen ? 'caret-down' : 'caret-right'} className="ft-caret" />
            <Icon name={isOpen ? 'folder-open' : 'folder'} className="ft-ico ft-ico-dir" />
            <span className="ft-name">{e.name}</span>
          </>
        ) : (
          <>
            <span className="ft-caret-spacer" aria-hidden="true" />
            <Icon name="file" className={'ft-ico ' + fileKindClass(e.name)} />
            <span className={'ft-name ' + statusClass(st)}>{e.name}</span>
          </>
        )}
      </div>
    )
  }

  const renderSearchRow = (m: WorkspaceFileMatch, i: number) => {
    const [name, dir] = splitPath(m.path)
    return (
      <div
        key={m.path}
        className={'ft-row ft-search-row' + (i === cursor ? ' is-cursor' : '')}
        title={m.path}
        onClick={() => setPreview(m.path)}
        onContextMenu={(ev) => {
          ev.preventDefault()
          setMenu({
            x: Math.min(ev.clientX, window.innerWidth - 210),
            y: Math.min(ev.clientY, window.innerHeight - 110),
            path: m.path,
            isDir: m.kind === 'dir',
          })
        }}
      >
        <Icon
          name={m.kind === 'dir' ? 'folder' : 'file'}
          className={'ft-ico ' + (m.kind === 'dir' ? 'ft-ico-dir' : fileKindClass(m.name))}
        />
        <span className="ft-name">{name}</span>
        {dir && <span className="ft-dir">{dir}</span>}
      </div>
    )
  }

  return (
    <>
      <div className="rp-subhead">
        <div className="rp-search">
          <Icon name="magnifying-glass" />
          <input
            type="text"
            value={query}
            placeholder="Search files…"
            aria-label="Search workspace files"
            onChange={(e) => setQuery(e.target.value)}
          />
          {query && (
            <button
              type="button"
              className="rp-filter-clear"
              title="Clear search"
              onClick={() => setQuery('')}
            >
              <Icon name="xmark" />
            </button>
          )}
        </div>
        <button
          type="button"
          className={'icon-btn' + (showHidden ? ' is-active' : '')}
          title={showHidden ? 'Hide dotfiles' : 'Show dotfiles'}
          aria-pressed={showHidden}
          onClick={toggleHidden}
        >
          <Icon name={showHidden ? 'eye' : 'eye-slash'} />
        </button>
      </div>
      {error && <div className="rp-empty">Failed to load: {error}</div>}
      {searching && rowCount === 0 && !error && <div className="rp-empty">No matching files</div>}
      {!searching && !tree[''] && !error && <div className="rp-empty">Loading…</div>}
      {rowCount > 0 && (
        <VirtualList
          ref={vlistRef}
          className="rp-body vlist"
          count={rowCount}
          itemHeight={searching ? SEARCH_ROW_H : TREE_ROW_H}
          tabIndex={0}
          id="rp-panel"
          role="tabpanel"
          ariaLabelledBy="rp-tab-files"
          ariaLabel={searching ? 'File search results' : 'Workspace files'}
          onKeyDown={onTreeKeyDown}
          render={(i) =>
            searching ? renderSearchRow(results[i], i) : renderTreeRow(treeRows[i], i)
          }
        />
      )}
      {menu && (
        <ContextMenu
          x={menu.x}
          y={menu.y}
          items={menuItems(menu.path, menu.isDir)}
          onClose={() => setMenu(null)}
        />
      )}
    </>
  )
})

// ---------- file preview ----------

// Above this size, whole-file hljs costs more than the readability it buys (plus
// the server truncates at 256KB anyway): fall through to plain <code>{text}</code>.
const HIGHLIGHT_CHAR_LIMIT = 120_000
// Above this many lines, skip the line-number gutter (would be thousands of nodes).
const MAX_GUTTER_LINES = 3000

const FilePreview = memo(function FilePreview({
  controller,
  wsId,
  path,
  onBack,
}: {
  controller: AppController
  wsId: string
  path: string
  onBack: () => void
}) {
  const [data, setData] = useState<WorkspaceFileContent | null>(null)
  const [error, setError] = useState('')
  const [wrap, setWrap] = useState(false)
  const [copied, setCopied] = useState(false)

  useEffect(() => {
    let cancelled = false
    setData(null)
    setError('')
    controller.api
      .readWorkspaceFile(wsId, path)
      .then((d) => {
        if (!cancelled) setData(d)
      })
      .catch((e) => {
        if (!cancelled) setError((e as Error).message)
      })
    // rapid A→B file clicks: a slow response for A must not render under B
    return () => {
      cancelled = true
    }
  }, [controller, wsId, path])

  const content = data?.content ?? ''
  const html = useMemo(
    () =>
      data && !data.binary && content && content.length <= HIGHLIGHT_CHAR_LIMIT
        ? highlightToHtml(content, langFromPath(path))
        : '', // over the limit: plain text — hljs on a 256KB buffer blocks the main thread
    [data, content, path],
  )
  const lineCount = useMemo(() => (content ? content.split('\n').length : 0), [content])
  const showGutter = !wrap && lineCount > 1 && lineCount <= MAX_GUTTER_LINES

  const [name, dir] = splitPath(path)

  const copyFile = async () => {
    if (!(await copyText(content))) {
      toast('Copy failed — clipboard unavailable')
      return
    }
    setCopied(true)
    setTimeout(() => setCopied(false), 1500)
  }

  return (
    <div className="rp-body fp" id="rp-panel" role="tabpanel" aria-labelledby="rp-tab-files">
      <div className="rp-subhead">
        <button type="button" className="icon-btn" title="Back to file list" onClick={onBack}>
          <Icon name="arrow-left" />
        </button>
        <span className="fp-path mono" title={path}>
          {dir && <span className="fp-path-dir">{dir}/</span>}
          <span className="fp-path-name">{name}</span>
        </span>
        <button
          type="button"
          className={'rp-chip' + (wrap ? ' is-active' : '')}
          title="Toggle word wrap"
          aria-pressed={wrap}
          onClick={() => setWrap((w) => !w)}
        >
          Wrap
        </button>
        <button
          type="button"
          className={'icon-btn' + (copied ? ' is-active' : '')}
          title="Copy file contents"
          disabled={!data || data.binary}
          onClick={() => void copyFile()}
        >
          <Icon name={copied ? 'check' : 'copy'} />
        </button>
      </div>
      {error ? (
        <div className="rp-empty">Failed to load: {error}</div>
      ) : !data ? (
        <div className="rp-empty">Loading…</div>
      ) : data.binary ? (
        <div className="rp-empty">Binary files cannot be previewed</div>
      ) : (
        <>
          <div className={'fp-scroll' + (wrap ? ' is-wrap' : '')}>
            {showGutter && (
              <div className="fp-lines mono" aria-hidden="true">
                {Array.from({ length: lineCount }, (_, i) => (
                  <div key={i}>{i + 1}</div>
                ))}
              </div>
            )}
            <pre className="fp-code mono">
              {html ? <code dangerouslySetInnerHTML={{ __html: html }} /> : <code>{content}</code>}
            </pre>
          </div>
          {data.truncated && (
            <div className="notice">File too large; showing the first 256KB only</div>
          )}
        </>
      )}
    </div>
  )
})
