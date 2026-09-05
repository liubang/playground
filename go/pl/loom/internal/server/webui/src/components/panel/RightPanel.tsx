// RightPanel.tsx — right-side workspace panel: the Changes tab (git working-tree
// status + inline diff viewing) and the Files tab (lazy-loaded file tree + code
// preview).
//
// Data principle: git is the source of truth for changes (covers both agent edits
// via run_cmd/sed and the user's own edits); the transcript's tool.completed /
// turn.finished events serve only as invalidation triggers (the controller bumps
// gitStamp → git status is refetched). Same for the file tree: on gitStamp change,
// cached expanded directories are invalidated and refetched. The panel is
// read-only — no write/commit entry points.

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { AppController } from '../../app/controller'
import { useStore } from '../../store/store'
import { Icon } from '../../lib/icons'
import { langFromPath } from '../../lib/diff'
import { highlightToHtml } from '../../lib/markdown'
import { DiffView } from '../blocks/DiffView'
import type {
  GitFileEntry,
  WorkspaceFileContent,
  WorkspaceFileEntry,
  WorkspaceGitDiff,
  WorkspaceGitStatus,
} from '../../protocol/types'

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
  return (
    <div className="rp">
      <div className="rp-head">
        <div className="rp-tabs" role="tablist">
          <button
            type="button"
            role="tab"
            aria-selected={tab === 'changes'}
            className={'rp-tab' + (tab === 'changes' ? ' is-active' : '')}
            onClick={() => controller.setRightPanelTab('changes')}
          >
            变更
          </button>
          <button
            type="button"
            role="tab"
            aria-selected={tab === 'files'}
            className={'rp-tab' + (tab === 'files' ? ' is-active' : '')}
            onClick={() => controller.setRightPanelTab('files')}
          >
            文件
          </button>
        </div>
        <button
          type="button"
          className="icon-btn"
          title="收起面板"
          onClick={() => controller.toggleRightPanel()}
        >
          <Icon name="xmark" />
        </button>
      </div>
      {!wsId ? (
        <div className="rp-empty">暂无工作区</div>
      ) : tab === 'changes' ? (
        <ChangesPane controller={controller} wsId={wsId} />
      ) : (
        <FilesPane controller={controller} wsId={wsId} />
      )}
    </div>
  )
}

// ---------- Changes tab ----------

const STATUS_LABEL: Record<string, string> = {
  M: '已修改',
  A: '新增',
  D: '已删除',
  R: '重命名',
  T: '类型变更',
  U: '未跟踪',
}

function splitPath(p: string): [string, string] {
  const i = p.lastIndexOf('/')
  return i < 0 ? [p, ''] : [p.slice(i + 1), p.slice(0, i)]
}

const ChangesPane = memo(function ChangesPane({
  controller,
  wsId,
}: {
  controller: AppController
  wsId: string
}) {
  const gitStamp = useStore(controller.store, (s) => s.gitStamp)
  const [data, setData] = useState<WorkspaceGitStatus | null>(null)
  const [error, setError] = useState('')
  const [openPath, setOpenPath] = useState('')
  const [diffs, setDiffs] = useState<Record<string, WorkspaceGitDiff>>({})
  const [diffErrors, setDiffErrors] = useState<Record<string, string>>({})

  const reloadSeq = useRef(0)
  // Current workspace: in-flight fetchDiff responses use it to detect staleness (same role as reload's seq guard).
  const wsIdRef = useRef(wsId)
  useEffect(() => {
    wsIdRef.current = wsId
  }, [wsId])

  // seq guard: a slow response from a superseded reload (e.g. previous
  // workspace) must never overwrite newer state.
  const reload = useCallback(async () => {
    const seq = ++reloadSeq.current
    try {
      const d = await controller.api.workspaceGitStatus(wsId)
      if (seq !== reloadSeq.current) return
      setData(d)
      setError('')
    } catch (e) {
      if (seq !== reloadSeq.current) return
      setError((e as Error).message)
    }
  }, [controller, wsId])

  const fetchDiff = useCallback(
    (path: string) => {
      controller.api
        .workspaceGitDiff(wsId, path)
        // A slow response returning after a workspace switch is dropped — a stale workspace's diff must not hit the cache
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

  // gitStamp is the invalidation signal: tool writes / turn end trigger a
  // refetch — and diff caches must be dropped too, or an expanded file keeps
  // showing its pre-edit diff forever.
  useEffect(() => {
    void reload()
    setDiffs({})
    setDiffErrors({})
    if (openPath) fetchDiff(openPath)
  }, [reload, gitStamp]) // eslint-disable-line react-hooks/exhaustive-deps

  // Workspace switch: clear expansion state and the diff cache
  useEffect(() => {
    setOpenPath('')
    setDiffs({})
    setDiffErrors({})
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

  if (error) return <div className="rp-empty">加载失败：{error}</div>
  if (!data) return <div className="rp-empty">加载中…</div>
  if (!data.is_git) return <div className="rp-empty">当前工作区不是 git 仓库</div>

  const files = data.files || []
  return (
    <div className="rp-body">
      <div className="rp-subhead">
        <span className="rp-branch mono" title="当前分支">
          {data.branch || 'HEAD'}
        </span>
        <span className="gf-stats mono">
          {(data.adds ?? 0) > 0 && <span className="st-add">+{data.adds}</span>}
          {(data.dels ?? 0) > 0 && <span className="st-del">−{data.dels}</span>}
        </span>
        <button type="button" className="icon-btn" title="刷新" onClick={() => void reload()}>
          <Icon name="rotate-left" />
        </button>
      </div>
      {files.length === 0 && <div className="rp-empty">工作区干净，没有未提交的变更</div>}
      {files.map((f) => (
        <GitFileRow
          key={f.path}
          file={f}
          open={openPath === f.path}
          diff={diffs[f.path]}
          diffError={diffErrors[f.path]}
          onToggle={() => toggleFile(f.path)}
        />
      ))}
    </div>
  )
})

const GitFileRow = memo(function GitFileRow({
  file,
  open,
  diff,
  diffError,
  onToggle,
}: {
  file: GitFileEntry
  open: boolean
  diff?: WorkspaceGitDiff
  diffError?: string
  onToggle: () => void
}) {
  const [name, dir] = splitPath(file.path)
  const status = file.status || 'M'
  return (
    <div className={'gf' + (open ? ' is-open' : '')}>
      <button type="button" className="gf-row" title={file.path} onClick={onToggle}>
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
      {open && (
        <div className="gf-diff">
          {diffError ? (
            <div className="rp-empty">diff 加载失败：{diffError}</div>
          ) : !diff ? (
            <div className="rp-empty">加载中…</div>
          ) : diff.is_dir ? (
            <div className="rp-empty">新增目录（无 diff 可展示）</div>
          ) : diff.diff ? (
            <>
              <DiffView diffText={diff.diff} />
              {diff.truncated && <div className="notice">diff 过大，已截断</div>}
            </>
          ) : (
            <div className="rp-empty">无内容差异（可能仅是模式/重命名变更）</div>
          )}
        </div>
      )}
    </div>
  )
})

// ---------- Files tab ----------

const FilesPane = memo(function FilesPane({
  controller,
  wsId,
}: {
  controller: AppController
  wsId: string
}) {
  const gitStamp = useStore(controller.store, (s) => s.gitStamp)
  // tree: dir path → children ("" is the root); only expanded directories are
  // cached — lazy loading
  const [tree, setTree] = useState<Record<string, WorkspaceFileEntry[]>>({})
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(new Set())
  const [preview, setPreview] = useState('')
  const [error, setError] = useState('')

  const wsRef = useRef(wsId)
  wsRef.current = wsId

  const loadDir = useCallback(
    async (path: string) => {
      const forWs = wsId
      try {
        const res = await controller.api.listWorkspaceFiles(wsId, path)
        if (wsRef.current !== forWs) return // workspace switched mid-flight
        setTree((m) => ({ ...m, [path]: res.entries || [] }))
        setError('')
      } catch (e) {
        if (wsRef.current !== forWs) return
        setError((e as Error).message)
      }
    },
    [controller, wsId],
  )

  // Workspace switch: rebuild the whole tree
  useEffect(() => {
    setTree({})
    setExpanded(new Set())
    setPreview('')
    void loadDir('')
  }, [wsId, loadDir])

  // gitStamp: invalidate and refetch cached expanded directories (new/deleted
  // files reflected on the tree)
  useEffect(() => {
    for (const dir of expanded) void loadDir(dir)
    void loadDir('')
  }, [gitStamp]) // eslint-disable-line react-hooks/exhaustive-deps

  const toggleDir = (path: string) => {
    const next = new Set(expanded)
    if (next.has(path)) {
      next.delete(path)
    } else {
      next.add(path)
      if (!tree[path]) void loadDir(path)
    }
    setExpanded(next)
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
  return (
    <div className="rp-body">
      {error && <div className="rp-empty">加载失败：{error}</div>}
      {!tree[''] && !error && <div className="rp-empty">加载中…</div>}
      {tree[''] && (
        <DirChildren
          path=""
          depth={0}
          tree={tree}
          expanded={expanded}
          onToggleDir={toggleDir}
          onOpenFile={setPreview}
        />
      )}
    </div>
  )
})

function DirChildren({
  path,
  depth,
  tree,
  expanded,
  onToggleDir,
  onOpenFile,
}: {
  path: string
  depth: number
  tree: Record<string, WorkspaceFileEntry[]>
  expanded: ReadonlySet<string>
  onToggleDir: (path: string) => void
  onOpenFile: (path: string) => void
}) {
  const entries = tree[path]
  if (!entries) return <div className="ft-loading">加载中…</div>
  return (
    <>
      {entries.map((e) =>
        e.kind === 'dir' ? (
          <div key={e.path}>
            <button
              type="button"
              className="ft-row"
              style={{ paddingLeft: 10 + depth * 14 }}
              title={e.path}
              onClick={() => onToggleDir(e.path)}
            >
              <Icon name={expanded.has(e.path) ? 'folder-open' : 'folder'} />
              <span className="ft-name">{e.name}</span>
            </button>
            {expanded.has(e.path) && (
              <DirChildren
                path={e.path}
                depth={depth + 1}
                tree={tree}
                expanded={expanded}
                onToggleDir={onToggleDir}
                onOpenFile={onOpenFile}
              />
            )}
          </div>
        ) : (
          <button
            key={e.path}
            type="button"
            className="ft-row"
            style={{ paddingLeft: 10 + depth * 14 }}
            title={e.path}
            onClick={() => onOpenFile(e.path)}
          >
            <Icon name="file" />
            <span className="ft-name">{e.name}</span>
          </button>
        ),
      )}
    </>
  )
}

// Above this size, whole-file hljs costs more than the readability it buys (plus the
// server truncates at 256KB anyway): fall through to plain <code>{text}</code>.
const HIGHLIGHT_CHAR_LIMIT = 120_000

// File preview: whole-file syntax highlighting (highlightToHtml already passes
// through the sanitize whitelist internally); binary and over-limit truncation
// are flagged by the server.
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

  const html = useMemo(
    () =>
      data && !data.binary && data.content && data.content.length <= HIGHLIGHT_CHAR_LIMIT
        ? highlightToHtml(data.content, langFromPath(path))
        : '', // over the limit: plain text — hljs on a 256KB buffer blocks the main thread for seconds
    [data, path],
  )

  return (
    <div className="rp-body fp">
      <div className="rp-subhead">
        <button type="button" className="icon-btn" title="返回文件树" onClick={onBack}>
          <Icon name="arrow-left" />
        </button>
        <span className="fp-path mono" title={path}>
          {path}
        </span>
      </div>
      {error ? (
        <div className="rp-empty">加载失败：{error}</div>
      ) : !data ? (
        <div className="rp-empty">加载中…</div>
      ) : data.binary ? (
        <div className="rp-empty">二进制文件不支持预览</div>
      ) : (
        <>
          <pre className="fp-code mono">
            {html ? (
              <code dangerouslySetInnerHTML={{ __html: html }} />
            ) : (
              <code>{data.content}</code>
            )}
          </pre>
          {data.truncated && <div className="notice">文件过大，仅显示前 256KB</div>}
        </>
      )}
    </div>
  )
})
