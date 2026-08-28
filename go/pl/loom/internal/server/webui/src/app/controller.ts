// controller.ts — framework-agnostic app controller: boot orchestration, session
// lifecycle, batched SSE event dispatch, connection health, model/reasoning
// switching, workspace management, sharing, feedback.
// Logic mirrors the legacy static/js/main.js; all DOM manipulation is replaced by
// AppState projection, rendered by the subscribing React view layer.

import { createApi, ApiError, type Api } from '../protocol/api'
import { EventStream, type ConnState } from '../protocol/sse'
import type { RuntimeEvent, Plan, SessionState, TokenUsage } from '../protocol/events'
import type {
  ApprovalMode,
  ContextWindow,
  ModelEntry,
  SessionSummary,
  Snapshot,
  Workspace,
} from '../protocol/types'
import { Store } from '../store/store'
import { TranscriptController } from './transcript'
import type { ArtifactEntry } from '../components/blocks/context'
import { randomId, shortId, copyText } from '../lib/format'
import { toast } from '../components/ui/Toast'
import { confirmDialog } from '../components/ui/Confirm'

export const TOKEN_KEY = 'loom_token'
const THEME_KEY = 'loom_theme'
const SIDEBAR_KEY = 'loom_sidebar_collapsed'
const RIGHT_PANEL_KEY = 'loom_right_panel'
const RIGHT_PANEL_TAB_KEY = 'loom_right_panel_tab'
// Local feedback vote state: key carries session+run, value is "up"/"down".
// For UI restore only; Langfuse is the source of truth (score id is idempotent,
// re-voting overwrites instead of duplicating).
const fbKey = (sessionId: string, runId: string) => `loom_fb_${sessionId}_${runId}`

const SESSION_PAGE_SIZE = 30
// Batched SSE event rendering: catch-up after reconnect delivers events in
// bursts (hundreds to thousands); handling each synchronously would chain the
// main thread into multi-second long tasks. Queue + rAF batching, at most
// EVENTS_PER_FRAME per frame: yields the main thread between frames, keeps
// catch-up progress visible and the UI responsive.
const EVENTS_PER_FRAME = 120
// Reconnect badge debounce: delay 400ms before showing connecting/reconnecting
// after leaving live — transient drops that recover within the delay never
// disturb the visuals.
const CONN_BADGE_DELAY_MS = 400
// Disconnect escalation: raise a banner once consecutive failures hit the
// threshold (backoff sequence ~15s+) or the connection enters the dead state.
const CONN_WARN_ATTEMPTS = 5

export interface BannerState {
  text: string
  draining: boolean
}

export interface AppState {
  // boot: initial / auth check in progress (render nothing, so a valid-token
  // reload doesn't flash the gate)
  view: 'boot' | 'gate' | 'app'
  gateError: string
  gateLocked: boolean // desktop 401: in-process credential lost; only recourse is restart
  theme: 'dark' | 'light'
  sidebarCollapsed: boolean
  sessionId: string | null
  sessionState: SessionState | ''
  busy: boolean
  connState: ConnState | ''
  connDetail?: string
  sessionsLoading: boolean // sidebar infinite-scroll 加载中指示
  banner: BannerState | null
  sessions: SessionSummary[]
  showArchived: boolean
  workspaces: Workspace[]
  noWorkspace: boolean // zero-workspace onboarding state
  landingHint: string // landing page copy
  landingShowAddWs: boolean
  landingVisible: boolean
  models: ModelEntry[]
  defaultModelRef: string
  curModelRef: string
  curReasoning: string
  reasoningOverridden: boolean
  readOnly: boolean
  readOnlyLabel: string
  readOnlyTitle: string
  archived: boolean
  usage?: TokenUsage
  turnCount: number
  window: ContextWindow | null
  occupancy: number
  plan: Plan | null
  version: string
  settingsOpen: boolean
  dirPickerOpen: boolean
  imagesEnabled: boolean
  imagesDisabledReason: string
  hdrWorkspace: string // header breadcrumb: workspace name of the current session
  hdrWorkspaceTitle: string
  // Main-area view: session tabs (chat / trace list / maze) plus the
  // two-session compare page (sidebar compare entry).
  mainView: 'chat' | 'trace' | 'maze' | 'compare'
  // Right workspace panel (changes / file tree): collapse preference is
  // persisted; gitStamp is the changes-list invalidation signal — bumped on
  // tool.completed / turn.finished, prompting the panel to refetch git status.
  rightPanelOpen: boolean
  rightPanelTab: 'changes' | 'files'
  gitStamp: number
  // Baseline approval mode (quick switch in composer; initial value from
  // config approval.mode)
  approvalMode: string
}

function initialState(): AppState {
  return {
    view: 'boot',
    gateError: '',
    gateLocked: false,
    theme: 'dark',
    sidebarCollapsed: false,
    sessionId: null,
    sessionState: '',
    busy: false,
    connState: '',
    sessionsLoading: false,
    connDetail: undefined,
    banner: null,
    sessions: [],
    showArchived: false,
    workspaces: [],
    noWorkspace: false,
    landingHint: 'No session selected. Start a new one, or pick one from the list.',
    landingShowAddWs: false,
    landingVisible: true,
    models: [],
    defaultModelRef: '',
    curModelRef: '',
    curReasoning: '',
    reasoningOverridden: false,
    readOnly: false,
    readOnlyLabel: '',
    readOnlyTitle: '',
    archived: false,
    usage: undefined,
    turnCount: 0,
    window: null,
    occupancy: 0,
    plan: null,
    version: '',
    settingsOpen: false,
    dirPickerOpen: false,
    imagesEnabled: true,
    imagesDisabledReason: '',
    hdrWorkspace: '',
    hdrWorkspaceTitle: '',
    mainView: 'chat',
    rightPanelOpen: false,
    rightPanelTab: 'changes',
    gitStamp: 0,
    approvalMode: 'on-request',
  }
}

export class AppController {
  readonly store = new Store<AppState>(initialState())
  readonly transcript: TranscriptController
  readonly api: Api
  readonly stream: EventStream
  // View-layer scroll container (for preserving scroll position on resync),
  // attached by TranscriptView
  readonly scrollerRef: { el: HTMLDivElement | null } = { el: null }
  // TraceView's scroller; maze nodes locate their step here.
  readonly traceScrollerRef: { el: HTMLDivElement | null } = { el: null }

  token: string
  readonly isDesktopShell: boolean
  private sessCursor = ''
  private sessLoading = false
  private sessSig = ''
  private lastSubmit: { fp: string; key: string } | null = null // idempotent resend
  private eventQueue: RuntimeEvent[] = []
  private eventFlushScheduled = false
  // Event watermark: snapshot's event_seq is the server-side projection
  // watermark; effects of events with seq ≤ it are already in the snapshot.
  // Late frames arriving after reattach are necessarily ≤ the watermark — drop
  // them to avoid double-rendering on the rebuilt transcript (source of the
  // duplicate-output-per-command bug).
  private eventFloor = 0
  private connBadgeTimer: ReturnType<typeof setTimeout> | null = null
  private connPending: { state: ConnState; detail?: string } | null = null
  private connShownState: ConnState | '' = ''
  private drainPoll: ReturnType<typeof setInterval> | null = null
  // Composer drafts are per-session: stash the current input before switching
  // away, restore on switch back.
  private composerDrafts = new Map<string, string>()
  // Content-addressed artifact cache (blob URLs); LRU-bounded so image-heavy
  // sessions don't accumulate unbounded blob URLs. Map preserves insertion
  // order: get() re-inserts to mark recent use; evict oldest on overflow.
  // Resolved entries live here; in-flight requests live in artifactInflight
  // so a concurrent second caller .await's the same fetch instead of stampeding.
  private artifactURLCache = new Map<string, ArtifactEntry>()
  private artifactInflight = new Map<string, Promise<ArtifactEntry>>()
  private static ARTIFACT_CACHE_MAX = 50
  // Current composer draft text (written back by the controlled view component,
  // for draft stashing)
  composerText = ''
  onComposerRestore: ((text: string) => void) | null = null

  constructor() {
    this.token = sessionStorage.getItem(TOKEN_KEY) || ''
    // Desktop shell bootstrap (docs/DESKTOP_DESIGN.md §4.2): the embedded token
    // arrives via <meta name="loom-token"> or the URL fragment; the native
    // message bridge exists only in the desktop webview and still identifies
    // the desktop shell after a page reload.
    const embeddedToken =
      document.querySelector('meta[name="loom-token"]')?.getAttribute('content') ||
      '' ||
      new URLSearchParams(location.hash.slice(1)).get('token') ||
      ''
    const wailsDrag = (
      window as unknown as { webkit?: { messageHandlers?: { external?: unknown } } }
    ).webkit?.messageHandlers?.external
    this.isDesktopShell = embeddedToken !== '' || !!wailsDrag
    if (embeddedToken) {
      sessionStorage.setItem(TOKEN_KEY, embeddedToken)
      this.token = embeddedToken
      if (location.hash) history.replaceState(null, '', location.pathname)
    }

    this.api = createApi({
      getToken: () => this.token,
      onUnauthorized: () => this.onUnauthorized(),
    })
    this.transcript = new TranscriptController({
      resolveApproval: (payload, { decision, always, trust }) =>
        this.api.resolveApproval(this.store.get().sessionId || '', payload.approval_id || '', {
          callId: payload.call_id,
          argsHash: payload.args_hash,
          decision,
          // The server reconstructs the call's identity from the
          // projected approval card; the hint carries only the
          // trust flavor.
          ruleHint: always ? { trust } : undefined,
        }),
      answerQuestion: (questionId, answer) =>
        this.api.answerQuestion(this.store.get().sessionId || '', questionId, answer),
      // Feedback vote: write localStorage only on success (in-block selection
      // state is maintained by the view); when tracing is off / the run has no
      // trace the backend returns an error code — rethrow so the block rolls back.
      sendFeedback: async (runId, value) => {
        const sid = this.store.get().sessionId || ''
        await this.api.submitFeedback(sid, runId, value)
        try {
          localStorage.setItem(fbKey(sid, runId), value === 1 ? 'up' : 'down')
        } catch {
          /* private mode etc.: ignore */
        }
      },
      getFeedback: (runId) => {
        try {
          return localStorage.getItem(fbKey(this.store.get().sessionId || '', runId)) || ''
        } catch {
          return ''
        }
      },
      onError: (e) => toast(e.message),
    })
    this.stream = new EventStream({
      getToken: () => this.token,
      onEvent: (evt) => this.onRuntimeEvent(evt),
      onResync: (reason) => void this.resync(reason),
      onDraining: () => {
        this.setConn('draining')
        this.store.set({ banner: { text: '服务重启中，恢复后将自动重连…', draining: true } })
        this.startDrainRecovery()
      },
      onConn: (state, detail, attempt) => {
        this.setConn(state, detail)
        this.connHealth(state, detail, attempt)
      },
      onAuthError: () => this.onUnauthorized(),
    })
  }

  // ---------- theme / sidebar collapse ----------

  initTheme() {
    // Dark by default (user preference); light only when explicitly stored.
    const saved = sessionStorage.getItem(THEME_KEY)
    const dark = saved !== 'light'
    document.documentElement.dataset.theme = dark ? 'dark' : 'light'
    this.store.set({ theme: dark ? 'dark' : 'light' })
  }

  toggleTheme() {
    const nowDark = document.documentElement.dataset.theme === 'dark'
    const next = nowDark ? 'light' : 'dark'
    document.documentElement.dataset.theme = next
    sessionStorage.setItem(THEME_KEY, next)
    this.store.set({ theme: next as 'dark' | 'light' })
  }

  initSidebarToggle() {
    // Narrow screens (drawer mode) default to collapsed; desktop reads the
    // persisted preference
    const stored = localStorage.getItem(SIDEBAR_KEY)
    const collapsed =
      stored === '1' || (stored === null && window.matchMedia('(max-width: 767px)').matches)
    this.store.set({ sidebarCollapsed: collapsed })
  }

  toggleSidebar() {
    const now = !this.store.get().sidebarCollapsed
    this.store.set({ sidebarCollapsed: now })
    localStorage.setItem(SIDEBAR_KEY, now ? '1' : '0')
  }

  // In narrow-screen drawer mode, auto-collapse the drawer after picking a
  // session (does not touch the persisted preference)
  private collapseSidebarIfNarrow() {
    if (window.matchMedia('(max-width: 767px)').matches) {
      this.store.set({ sidebarCollapsed: true })
    }
  }

  // Narrow-screen drawer: collapse on backdrop tap (does not touch the
  // persisted preference; independent of desktop collapse)
  dismissSidebarDrawer() {
    this.store.set({ sidebarCollapsed: true })
  }

  expandSidebar() {
    this.store.set({ sidebarCollapsed: false })
  }

  // ---------- right workspace panel ----------

  initRightPanel() {
    // Collapsed by default; force-collapsed on narrow screens (drawer mode is
    // too cramped)
    const open =
      localStorage.getItem(RIGHT_PANEL_KEY) === '1' &&
      !window.matchMedia('(max-width: 767px)').matches
    const tab = localStorage.getItem(RIGHT_PANEL_TAB_KEY) === 'files' ? 'files' : 'changes'
    this.store.set({ rightPanelOpen: open, rightPanelTab: tab })
  }

  toggleRightPanel() {
    const now = !this.store.get().rightPanelOpen
    this.store.set({ rightPanelOpen: now })
    localStorage.setItem(RIGHT_PANEL_KEY, now ? '1' : '0')
  }

  setRightPanelTab(tab: 'changes' | 'files') {
    this.store.set({ rightPanelTab: tab })
    localStorage.setItem(RIGHT_PANEL_TAB_KEY, tab)
  }

  // Changes-list invalidation signal: git is the source of truth; transcript
  // events only trigger refetches. Trailing-edge debounce — a long agent turn
  // completes dozens of tools, and each completion would otherwise spawn a git
  // status round (three git subprocesses server-side) plus tree refetches.
  private gitStampTimer: ReturnType<typeof setTimeout> | null = null

  private bumpGitStamp() {
    if (this.gitStampTimer) clearTimeout(this.gitStampTimer)
    this.gitStampTimer = setTimeout(() => {
      this.gitStampTimer = null
      this.store.set({ gitStamp: this.store.get().gitStamp + 1 })
    }, 500)
  }

  // Workspace bound to the right panel: the current session's workspace,
  // falling back to the default workspace when no session is open.
  currentWorkspaceId(): string {
    const s = this.store.get()
    const sess = s.sessions.find((x) => x.id === s.sessionId)
    if (sess && sess.workspace_id) return sess.workspace_id
    const def = s.workspaces.find((w) => w.is_default) || s.workspaces[0]
    return def ? def.id : ''
  }

  // ---------- approval mode quick switch ----------

  // Initial value comes from config approval.mode (the settings panel's single
  // source of truth); afterwards tracked locally by pickApprovalMode (override
  // is not persisted — restart/hot reload falls back to config).
  private async loadApprovalMode() {
    try {
      const env = await this.api.getConfig()
      const cfg = (env.config || {}) as Record<string, unknown>
      const nested = cfg.approval as Record<string, unknown> | undefined
      const mode =
        (nested && typeof nested.mode === 'string' && nested.mode) ||
        (typeof cfg['approval.mode'] === 'string' ? (cfg['approval.mode'] as string) : '') ||
        'on-request'
      this.store.set({ approvalMode: mode })
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('load approval mode:', e)
    }
  }

  // Re-read the workspace's effective approval mode (live override or config
  // default): after a session open/reload the page must not misreport an
  // override set earlier in the process.
  refreshApprovalMode = async () => {
    const wsId = this.currentWorkspaceId()
    if (!wsId) return
    try {
      const r = await this.api.getWorkspaceApprovalMode(wsId)
      if (r.mode) this.store.set({ approvalMode: r.mode })
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('approval mode:', e)
    }
  }

  async pickApprovalMode(mode: string) {
    const wsId = this.currentWorkspaceId()
    if (!wsId) {
      toast('先添加一个工作区')
      return
    }
    try {
      await this.api.setWorkspaceApprovalMode(wsId, mode as ApprovalMode)
      this.store.set({ approvalMode: mode })
      // Policy is captured at run construction: no effect on the currently
      // running turn
      toast(`审批模式：${mode}（下一轮生效，不写入配置）`, true)
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('切换审批模式失败: ' + (e as Error).message)
    }
  }

  // ---------- gate / boot ----------

  private showGate(err?: string) {
    this.stream.detach()
    this.store.set({ view: 'gate', gateError: err || '' })
  }

  private onUnauthorized() {
    sessionStorage.removeItem(TOKEN_KEY)
    this.token = ''
    if (this.isDesktopShell) {
      // The desktop token is an in-process random value; the user has no
      // credential to paste. A 401 means abnormal process state — the only
      // recourse is restarting the app (docs/DESKTOP_DESIGN.md §4.2).
      this.store.set({
        view: 'gate',
        gateError: '桌面端鉴权状态丢失，请重启应用',
        gateLocked: true,
      })
      return
    }
    this.showGate('token invalid or expired — paste the current serve token')
  }

  submitGateToken(token: string) {
    if (!token) return
    this.token = token
    sessionStorage.setItem(TOKEN_KEY, token)
    void this.enter()
  }

  async boot() {
    this.initTheme()
    this.initSidebarToggle()
    this.initRightPanel()
    // Proactively probe and reconnect when the page becomes visible again /
    // restores from BFCache / the network recovers: watchdog timers are
    // suspended with the page (App Nap / window occlusion), so without poking
    // we'd never notice the drop.
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) this.stream.ensureLive()
    })
    window.addEventListener('pageshow', () => this.stream.ensureLive())
    // Also refresh the session list when the window regains focus: other
    // sessions' approval/run state may have changed in the background; this
    // active refresh covers the gap between polls.
    window.addEventListener('focus', () => {
      this.stream.ensureLive()
      void this.refreshSessions()
    })
    window.addEventListener('online', () => this.stream.ensureLive())
    // Sidebar polling (while page is visible, 5s)
    setInterval(() => {
      if (document.visibilityState === 'visible' && this.store.get().view === 'app') {
        void this.refreshSessions()
      }
    }, 5000)

    if (!this.token) {
      this.showGate()
    } else {
      await this.enter()
    }
  }

  private async enter() {
    try {
      const meta = await this.api.metaVersion()
      this.store.set({ version: meta.version || '' })
      // Load the model catalog (picker data source; modalities ship with the
      // catalog and drive the attachment entry)
      await this.refreshModelCatalog()
      void this.loadApprovalMode()
      this.store.set({ view: 'app' })
      await this.loadWorkspaces()
      await this.refreshSessions()
      // First entry lands on the landing page: don't auto-open a session
      // (auto-restore would spin up the whole controller runtime, and with
      // multiple workspaces the "latest session" is rarely what the user wants)
      this.showLandingState()
    } catch (e) {
      if ((e as ApiError).status !== 401) this.showGate('connect failed: ' + (e as Error).message)
    }
  }

  // ---------- connection badge / banner ----------

  private renderConnBadge(state: ConnState | '', detail?: string) {
    this.connShownState = state
    this.store.set({ connState: state, connDetail: detail })
  }

  private setConn(state: ConnState, detail?: string) {
    const transient = state === 'connecting' || state === 'reconnecting'
    const showingTransient =
      this.connShownState === 'connecting' || this.connShownState === 'reconnecting'
    if (transient && !showingTransient) {
      this.connPending = { state, detail }
      if (!this.connBadgeTimer) {
        this.connBadgeTimer = setTimeout(() => {
          this.connBadgeTimer = null
          const p = this.connPending
          this.connPending = null
          if (p) this.renderConnBadge(p.state, p.detail)
        }, CONN_BADGE_DELAY_MS)
      }
      return
    }
    if (this.connBadgeTimer) {
      clearTimeout(this.connBadgeTimer)
      this.connBadgeTimer = null
      this.connPending = null
    }
    this.renderConnBadge(state, detail)
  }

  private connHealth(state: ConnState, detail?: string, attempt?: number) {
    if (state === 'live') {
      this.stopDrainRecovery()
      this.store.set({ banner: null })
      return
    }
    if (state === 'draining') return // onDraining already raised the banner
    if (state === 'dead') {
      this.showConnBanner(`连接已断开：${detail || 'disconnected'}`)
    } else if (typeof attempt === 'number' && attempt >= CONN_WARN_ATTEMPTS) {
      this.showConnBanner('连接已断开，正在自动重连…')
    }
  }

  // Drain self-healing: after a graceful shutdown the service usually returns
  // quickly as a new instance (desktop restart, rolling deploy). Slowly poll
  // the version endpoint; resync once reachable.
  private startDrainRecovery() {
    if (this.drainPoll) return
    this.drainPoll = setInterval(() => {
      void (async () => {
        try {
          await this.api.metaVersion()
          void this.resync('drain_recovered')
        } catch {
          /* still down: retry next tick */
        }
      })()
    }, 10_000)
  }

  private stopDrainRecovery() {
    if (this.drainPoll) {
      clearInterval(this.drainPoll)
      this.drainPoll = null
    }
  }

  private showConnBanner(text: string) {
    this.store.set({ banner: { text, draining: false } })
  }

  // ---------- session state ----------

  private setSessionState(state: SessionState | '') {
    const busy = state === 'running' || state === 'awaiting_approval' || state === 'cancelling'
    this.store.set({ sessionState: state, busy })
  }

  // Read-only mode has two sources: sub-agent sessions (snap.delegated) and
  // archived sessions.
  private updateComposerLock() {
    const s = this.store.get()
    const locked = s.readOnly || s.archived
    this.store.set({
      readOnlyLabel: s.readOnly
        ? '子 agent 会话 · 只读'
        : s.archived
          ? '会话已归档 · 只读（在侧栏归档视图中取消归档后可继续）'
          : '',
    })
    void locked // readOnlyLabel === '' means unlocked (the view keys off this)
  }

  private setReadOnly(snap: Snapshot) {
    this.store.set({
      readOnly: !!snap.delegated,
      readOnlyTitle: snap.parent_session_id ? `parent: ${snap.parent_session_id}` : '',
    })
    this.updateComposerLock()
  }

  // setArchived toggles the archived read-only state of the current session.
  setArchived(archived: boolean) {
    this.store.set({ archived })
    this.updateComposerLock()
  }

  // ---------- artifact / tool output ----------

  // Artifact loading: fetch then mint a blob URL (artifacts are content-
  // addressed immutable blobs; cache by id+size so the snapshot and live paths
  // don't download twice).
  fetchArtifactURL = async (id: string, size: number): Promise<ArtifactEntry> => {
    const key = `${id}:${size}`
    const cached = this.artifactURLCache.get(key)
    if (cached) {
      // LRU touch: re-insert to move to end (most-recently-used position)
      this.artifactURLCache.delete(key)
      this.artifactURLCache.set(key, cached)
      return cached
    }
    // Promise coalescing: a concurrent second caller for the same key awaits
    // the same in-flight promise instead of firing a duplicate fetch (the old
    // "check → fetch → set" window leaked one duplicate per race).
    const inflight = this.artifactInflight.get(key)
    if (inflight) return inflight
    const params = new URLSearchParams({ size: String(size) })
    const promise = (async () => {
      const res = await fetch(`/v1/artifacts/${encodeURIComponent(id)}?${params}`, {
        headers: { Authorization: 'Bearer ' + this.token },
      })
      if (!res.ok) throw new Error(`artifact fetch failed (HTTP ${res.status})`)
      const blob = await res.blob()
      const entry: ArtifactEntry = {
        url: URL.createObjectURL(blob),
        mediaType: blob.type || '',
        blob,
      }
      // LRU evict: drop oldest entries (first in Map iteration order) when over capacity
      while (this.artifactURLCache.size >= AppController.ARTIFACT_CACHE_MAX) {
        const oldest = this.artifactURLCache.keys().next().value
        if (oldest === undefined) break
        const evicted = this.artifactURLCache.get(oldest)
        if (evicted) URL.revokeObjectURL(evicted.url)
        this.artifactURLCache.delete(oldest)
      }
      this.artifactURLCache.set(key, entry)
      return entry
    })()
    this.artifactInflight.set(key, promise)
    try {
      return await promise
    } finally {
      // Clean up the in-flight slot regardless of outcome; failures leave no
      // cache entry, so a later retry re-fetches.
      this.artifactInflight.delete(key)
    }
  }

  // Release all artifact blob URLs on session switch, so the cache can't grow
  // unbounded across image-heavy sessions.
  private clearArtifactURLCache() {
    for (const entry of this.artifactURLCache.values()) URL.revokeObjectURL(entry.url)
    this.artifactURLCache.clear()
  }

  // Copy full tool output: live tool.completed events carry only a bounded
  // preview; the full content comes from the snapshot message history by call_id.
  fetchToolOutput = async (callId: string): Promise<string> => {
    const sid = this.store.get().sessionId
    if (!sid || !callId) throw new Error('no active session')
    const snap = await this.api.snapshot(sid)
    for (const m of snap.messages || []) {
      for (const part of m.parts || []) {
        const r = part.kind === 'tool_result' ? part.tool_result : null
        if (!r || r.call_id !== callId) continue
        const texts = (r.content || [])
          .filter((c) => c.kind === 'text' && c.text)
          .map((c) => c.text as string)
        const out = texts.join('\n')
        if (out) return out
        if (r.error && r.error.message) return r.error.message
        throw new Error('tool output unavailable (empty or compacted)')
      }
    }
    throw new Error('tool result not found in session history')
  }

  // ---------- model / reasoning state sync ----------

  modelLabel(ref: string): string {
    // Show only the model name (drop the provider prefix) — more compact
    return ref ? ref.split('/').pop() || 'model' : 'model'
  }

  // refreshModelCatalog refetches the model catalog and syncs all consumers.
  // The desktop shell has no F5, so after settings hot-apply we must refresh
  // in place, otherwise changes like modalities never surface in the UI.
  refreshModelCatalog = async () => {
    try {
      const cat = await this.api.metaModels()
      this.store.set({ models: cat.models || [], defaultModelRef: cat.default || '' })
      this.syncAttachCapability()
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('refresh models:', e)
    }
  }

  // syncAttachCapability gates the composer's image attachment entry on the
  // current model's declared modalities. If the current model is missing from
  // the catalog (stale config etc.), keep it enabled — the server-side submit
  // gate reports the error.
  private syncAttachCapability() {
    const s = this.store.get()
    const entry = s.models.find((m) => m.provider + '/' + m.name === s.curModelRef)
    if (!entry) {
      this.store.set({ imagesEnabled: true, imagesDisabledReason: '' })
      return
    }
    const ok = (entry.modalities || []).includes('image')
    this.store.set({
      imagesEnabled: ok,
      imagesDisabledReason: ok
        ? ''
        : `模型 ${entry.name} 未声明图片输入（modalities）；请切换多模态模型，或在设置 → 模型中勾选「图片输入」`,
    })
  }

  private applySnapshotMeta(snap: Snapshot) {
    const s = this.store.get()
    let curModelRef = s.curModelRef
    if (snap.provider_name && snap.model_name) {
      curModelRef = snap.provider_name + '/' + snap.model_name
    } else if (snap.model_name) {
      curModelRef = s.defaultModelRef || snap.model_name
    }
    this.store.set({
      curModelRef,
      curReasoning: snap.reasoning_effort || '',
      reasoningOverridden: !!snap.reasoning_overridden,
    })
    this.syncAttachCapability()
  }

  async pickModel(ref: string) {
    const sid = this.store.get().sessionId
    if (!sid) {
      toast('先创建或选择一个会话')
      return
    }
    try {
      const r = await this.api.setModel(sid, ref)
      // Adopt the picker's ref directly: it shares provenance with the
      // currentRef comparison in list items, so it always matches.
      // (SetModelResult has no JSON tags; response keys are capitalized
      // Cur/Meta, easy to misread.)
      this.store.set({ curModelRef: ref })
      this.syncAttachCapability()
      // Window thresholds change on model switch: reset the occupancy ring
      // from the server-derived new window projection; occupancy itself
      // refreshes on the next context.usage / snapshot
      this.store.set({ window: r.Window || r.window || null })
      toast('模型已切换为 ' + this.modelLabel(ref), true)
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('切换模型失败: ' + (e as Error).message)
    }
  }

  async pickReasoning(effort: string) {
    const sid = this.store.get().sessionId
    if (!sid) {
      toast('先创建或选择一个会话')
      return
    }
    try {
      const r = await this.api.setReasoning(sid, effort)
      // Same as above: SetReasoningResult response keys are capitalized
      // Effective/Overridden; effort comes from the picker's fixed option set,
      // adopt it directly.
      this.store.set({
        curReasoning: effort,
        reasoningOverridden: r.Overridden ?? r.overridden ?? effort !== 'default',
      })
      toast('reasoning: ' + (effort === 'default' ? '默认' : effort), true)
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('设置 reasoning 失败: ' + (e as Error).message)
    }
  }

  // ---------- session loading ----------

  // Session list signature: skip re-render when polling shows no change, so
  // rebuilding the view every 5s doesn't interrupt sidebar hover actions
  // (archive/delete buttons). The view (active/archived) joins the signature.
  private sessionListSig(list: SessionSummary[]): string {
    return (
      this.store.get().showArchived +
      '|' +
      list.map((s) => `${s.id}:${s.state}:${s.updated_at}:${s.title}`).join(',')
    )
  }

  // Workspace signature: registering/deleting an (empty) workspace doesn't
  // change the session list, so the workspace set itself must join
  // refreshSessions' skip check.
  private workspacesSig(list: Workspace[]): string {
    return (list || [])
      .map((w) => `${w.id}:${w.name || ''}:${w.root_path || ''}:${w.session_count}`)
      .join(',')
  }

  refreshSessions = async () => {
    try {
      const limit = Math.max(this.store.get().sessions.length, SESSION_PAGE_SIZE)
      const { sessions, next_cursor } = await this.api.listSessions(
        limit,
        '',
        this.store.get().showArchived,
        'all',
      )
      const list = sessions || []
      this.sessCursor = next_cursor || ''
      const sig = this.sessionListSig(list) + '|' + this.workspacesSig(this.store.get().workspaces)
      if (sig !== this.sessSig) {
        this.sessSig = sig
        this.store.set({ sessions: list })
      }
      if (this.store.get().sessionId) this.syncHdrWorkspace()
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('list sessions:', e)
    }
  }

  // Infinite scroll: fetch the next page when scrolling near the bottom.
  loadMoreSessions = async () => {
    if (this.sessLoading || !this.sessCursor) return
    this.sessLoading = true
    this.store.set({ sessionsLoading: true })
    try {
      const { sessions, next_cursor } = await this.api.listSessions(
        SESSION_PAGE_SIZE,
        this.sessCursor,
        this.store.get().showArchived,
        'all',
      )
      const list = this.store.get().sessions.concat(sessions || [])
      this.sessCursor = next_cursor || ''
      this.sessSig =
        this.sessionListSig(list) + '|' + this.workspacesSig(this.store.get().workspaces)
      this.store.set({ sessions: list })
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('load more sessions:', e)
    } finally {
      this.sessLoading = false
      this.store.set({ sessionsLoading: false })
    }
  }

  toggleArchivedView() {
    const showArchived = !this.store.get().showArchived
    this.sessCursor = ''
    this.sessSig = ''
    this.store.set({ showArchived, sessions: [] })
    void this.refreshSessions()
  }

  // Archive / unarchive / delete a session (sidebar entry actions)
  onSessionAction = async (id: string, action: 'archive' | 'unarchive' | 'delete') => {
    try {
      if (action === 'delete') {
        const sess = this.store.get().sessions.find((x) => x.id === id)
        const title = (sess && sess.title) || shortId(id)
        const ok = await confirmDialog({
          title: '删除会话',
          body: `「${title}」将被永久删除，包括全部消息与事件记录。该操作不可恢复。`,
          okLabel: '删除',
        })
        if (!ok) return
        await this.api.deleteSession(id)
        if (id === this.store.get().sessionId) {
          // Deleted the currently open session: detach the stream, return to
          // the empty state (read-only state resets too)
          this.stream.detach()
          this.store.set({ sessionId: null, archived: false, readOnly: false })
          this.updateComposerLock()
          this.transcript.clear()
          this.store.set({ window: null, occupancy: 0, plan: null, hdrWorkspace: '' })
          this.showLandingState()
          this.setSessionState('closed')
        }
        toast('会话已删除', true)
      } else {
        await this.api.archiveSession(id, action === 'archive')
        toast(action === 'archive' ? '已归档' : '已取消归档', true)
        // Archived/unarchived the currently open session: sync the composer
        // read-only state
        if (id === this.store.get().sessionId) this.setArchived(action === 'archive')
      }
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('操作失败: ' + (e as Error).message)
    }
    await this.refreshSessions()
  }

  // Composer drafts are per-session: stash the current input before switching
  // away, restore on switch back.
  private stashComposerDraft() {
    const sid = this.store.get().sessionId
    if (!sid) return
    const d = this.composerText
    if (d) this.composerDrafts.set(sid, d)
    else this.composerDrafts.delete(sid)
  }

  private restoreComposerDraft(id: string) {
    const saved = this.composerDrafts.get(id) || ''
    this.composerDrafts.delete(id)
    this.onComposerRestore?.(saved)
  }

  openSession = async (id: string, { archived = false }: { archived?: boolean } = {}) => {
    // Reopening the same session = resync (stream recovery / manual retry):
    // keep the scroll position, don't yank back to the bottom.
    const isResync = this.store.get().sessionId === id
    // Archived read-only state: refresh the lock immediately — if snapshot/
    // resume fails, the composer must not retain the previous session's state.
    this.store.set({ archived })
    this.updateComposerLock()
    this.stashComposerDraft()
    this.clearArtifactURLCache()
    this.stream.detach()
    // After detach, anything left in the catch-up queue predates the old
    // session/snapshot; the new snapshot covers it — safe to drop.
    this.eventQueue.length = 0
    this.store.set({ sessionId: id, landingVisible: false })
    this.restoreComposerDraft(id)
    this.collapseSidebarIfNarrow()
    this.syncHdrWorkspace()

    let snap: Snapshot
    try {
      snap = await this.api.snapshot(id)
    } catch (e) {
      if ((e as ApiError).status === 404) {
        // Not live: resume first, then fetch the snapshot
        await this.api.resumeSession(id)
        snap = await this.api.snapshot(id)
      } else {
        throw e
      }
    }
    // Snapshot race guard: if the user switched sessions while the snapshot
    // fetch was in flight, this snapshot belongs to the OLD session — drop it,
    // otherwise it would overwrite the new session's transcript, plan, model
    // metadata, readOnly/archived state, and eventFloor with stale data.
    if (this.store.get().sessionId !== id) {
      console.info(
        'openSession: dropping stale snapshot for',
        id,
        '(now on',
        this.store.get().sessionId,
        ')',
      )
      return
    }
    // Preserve scroll on resync: the view layer restores scrollTop after the
    // DOM rebuild
    if (isResync && this.scrollerRef.el) {
      const top = this.scrollerRef.el.scrollTop
      this.transcript.store.update((s) => {
        s.preserveScrollTop = top
      })
    }
    this.transcript.applySnapshot(snap, { preserveScroll: isResync })
    this.store.set({ plan: snap.plan || null })
    this.setReadOnly(snap)
    this.setSessionState(snap.state || '')
    this.applySnapshotMeta(snap)
    this.store.set({
      usage: snap.usage,
      turnCount: snap.turn_count || 0,
      // ctx occupancy ring: both the window threshold and occupancy are
      // server-side projections (same accounting as the compaction trigger);
      // the frontend does no local estimation
      window: snap.window && snap.window.effective ? (snap.window as ContextWindow) : null,
      occupancy: snap.occupancy || 0,
    })
    // Clear the catch-up queue once more before attach: leftover events the
    // old connection dispatched during the snapshot fetch are either already
    // covered by the snapshot (seq ≤ event_seq) or will be replayed by the new
    // stream from event_seq — dropping them is lossless.
    this.eventFloor = snap.event_seq || 0
    this.eventQueue.length = 0
    this.stream.attach(id, snap.event_seq || 0)
    void this.refreshApprovalMode()
  }

  onSelectSession = (id: string) => {
    if (id === this.store.get().sessionId) return
    // A session opened from the archived view = archived (read-only); the
    // default view = active sessions
    this.openSession(id, { archived: this.store.get().showArchived }).catch((e) => {
      if ((e as ApiError).status !== 401) toast('open session: ' + (e as Error).message)
    })
  }

  // Header breadcrumb: workspace name of the current session.
  private syncHdrWorkspace() {
    const s = this.store.get()
    const sess = s.sessions.find((x) => x.id === s.sessionId)
    if (!sess) {
      this.store.set({ hdrWorkspace: '', hdrWorkspaceTitle: '' })
      return
    }
    const wsId = sess.workspace_id || ''
    const ws = s.workspaces.find((w) => w.id === wsId)
    const name = ws ? ws.name || ws.root_path || '' : wsId ? '已删除的工作区' : '默认工作区'
    this.store.set({
      hdrWorkspace: name + ' /',
      hdrWorkspaceTitle: ((ws && ws.root_path) || name) + '（点击在侧栏定位）',
    })
  }

  // Breadcrumb click: expand the sidebar (if collapsed) and locate the
  // workspace group owning the current session
  revealCurrentWorkspace(): string | null {
    const s = this.store.get()
    if (!s.sessionId) return null
    const sess = s.sessions.find((x) => x.id === s.sessionId)
    if (!sess) return null
    this.expandSidebar()
    return sess.workspace_id || ''
  }

  // ---------- workspace management ----------

  loadWorkspaces = async () => {
    try {
      const { workspaces } = await this.api.listWorkspaces()
      this.store.set({ workspaces: workspaces || [] })
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('list workspaces:', e)
    }
  }

  openDirPicker() {
    this.store.set({ dirPickerOpen: true })
  }

  closeDirPicker() {
    this.store.set({ dirPickerOpen: false })
  }

  confirmDirPicker = async (rootPath: string) => {
    if (!rootPath) return
    try {
      const { workspace } = await this.api.registerWorkspace(rootPath, '')
      this.store.set({ dirPickerOpen: false })
      toast('已添加工作区 ' + (workspace.name || rootPath), true)
      await this.loadWorkspaces()
      await this.refreshSessions()
      // After adding the first workspace, leave the onboarding state for the
      // landing page (only when no session is open)
      if (this.store.get().workspaces.length === 1 && !this.store.get().sessionId) {
        this.showLandingState()
      }
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('添加工作区失败: ' + (e as Error).message)
    }
  }

  newSession = async (workspaceId?: string) => {
    const { session_id } = await this.api.createSession(workspaceId || '')
    await this.refreshSessions()
    await this.openSession(session_id, { archived: false })
  }

  onNewSession = (wsId: string) => {
    this.newSession(wsId).catch((e) => {
      if ((e as ApiError).status !== 401) toast('new session: ' + (e as Error).message)
    })
  }

  // Delete a workspace (sidebar workspace node action): cascades to all its
  // sessions; the on-disk directory is untouched.
  onDeleteWorkspace = async (wsId: string) => {
    const ws = this.store.get().workspaces.find((w) => w.id === wsId)
    const name = (ws && (ws.name || ws.root_path)) || wsId
    const count = (ws && ws.session_count) || 0
    const body =
      count > 0
        ? `「${name}」将被删除，其下 ${count} 个会话将一并永久删除（不可恢复）。磁盘目录不受影响。`
        : `「${name}」将从工作区列表移除（无会话）。磁盘目录不受影响。`
    const ok = await confirmDialog({ title: '删除工作区', body, okLabel: '删除' })
    if (!ok) return
    try {
      await this.api.deleteWorkspace(wsId)
      toast('工作区已删除', true)
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('删除工作区失败: ' + (e as Error).message)
      return
    }
    // If the currently open session belongs to the deleted workspace, detach
    // the stream and clear the transcript
    const s = this.store.get()
    if (s.sessionId) {
      const sess = s.sessions.find((x) => x.id === s.sessionId)
      if (sess && sess.workspace_id === wsId) {
        this.stream.detach()
        this.store.set({ sessionId: null, plan: null, hdrWorkspace: '' })
        this.transcript.clear()
        this.setSessionState('closed')
      }
    }
    await this.loadWorkspaces()
    await this.refreshSessions()
    // Deletion may enter the zero-workspace onboarding state; otherwise return
    // to the landing page (only when no session is open)
    if (this.syncWorkspaceGate()) return
    if (!this.store.get().sessionId) this.showLandingState()
  }

  // Zero-workspace onboarding state: hide the chat area and sidebar list when
  // no workspace exists.
  private syncWorkspaceGate(): boolean {
    const noWorkspace = this.store.get().workspaces.length === 0
    this.store.set({ noWorkspace })
    return noWorkspace
  }

  // First entry lands on the landing page: don't auto-open any session
  // (desktop startup should be cheap and predictable).
  private showLandingState() {
    if (this.syncWorkspaceGate()) return
    const hasSessions = this.store.get().sessions.length > 0
    this.store.set({
      landingHint: hasSessions
        ? 'Pick a session from the sidebar, or just start typing.'
        : 'No sessions yet — add a workspace, or just start typing.',
      landingShowAddWs: !hasSessions,
      landingVisible: true,
      hdrWorkspace: '',
    })
    this.store.set({ curModelRef: this.store.get().defaultModelRef })
    this.syncAttachCapability()
  }

  // Most recently active workspace: the session list is sorted by update time
  // (newest first); take the first entry's owner. Default target for new
  // sessions created by typing/enter directly — avoids landing in the process
  // default workspace.
  recentWorkspaceId(): string {
    const s = this.store.get().sessions.find((x) => x.workspace_id)
    return s ? s.workspace_id || '' : ''
  }

  // ---------- prompt / cancel ----------

  submitPrompt = async (
    text: string,
    images: { media_type: string; data: string }[] = [],
    followup = false,
  ) => {
    const s = this.store.get()
    if (s.readOnly) {
      toast('子 agent 会话为只读，不能追问')
      return
    }
    if (s.archived) {
      toast('会话已归档，仅可查看；取消归档后可继续对话')
      return
    }
    // followup is text-only: images go with a normal prompt (the backend also
    // rejects followup+images)
    if (followup && images.length) {
      toast('排队到下一轮的消息仅支持文本，图片已忽略')
      images = []
    }
    // Idempotency key: resending the same "text + image set + delivery mode"
    // shares one key (double-click / network retry won't create duplicate turns)
    const fp = (followup ? 'F:' : '') + text + '#' + images.map((i) => i.data.length).join('+')
    let key: string
    if (this.lastSubmit && this.lastSubmit.fp === fp) {
      key = this.lastSubmit.key
    } else {
      key = randomId()
    }
    this.lastSubmit = { fp, key }
    try {
      if (!this.store.get().sessionId) await this.newSession(this.recentWorkspaceId())
      await this.api.submitPrompt(this.store.get().sessionId || '', text, key, images, followup)
      this.onComposerRestore?.('')
      this.lastSubmit = null
      void this.refreshSessions()
    } catch (e) {
      const err = e as ApiError
      if (err.status === 401) return
      // On failure restore only the text; attachments stay in the composer for retry
      this.onComposerRestore?.(text)
      // Session was archived meanwhile (manual/auto): switch to read-only and
      // prompt to unarchive
      if (err.code === 'session_archived') {
        this.setArchived(true)
        void this.refreshSessions()
        toast('会话已归档，仅可查看；取消归档后可继续对话')
        return
      }
      toast('send failed: ' + err.message)
    }
  }

  cancelTurn = () => {
    const sid = this.store.get().sessionId
    if (!sid) return
    this.api.cancelTurn(sid).catch((e) => {
      if ((e as ApiError).status !== 401) toast('cancel: ' + (e as Error).message)
    })
  }

  // ---------- events ----------

  onRuntimeEvent(evt: RuntimeEvent) {
    // Late frames from the previous session (leftovers of the old connection
    // after switching) must not enter the new session's view.
    const sid = this.store.get().sessionId
    if (evt.session_id && sid && evt.session_id !== sid) return
    if (evt.sequence && evt.sequence <= this.eventFloor) return
    this.eventQueue.push(evt)
    if (this.eventFlushScheduled) return
    this.eventFlushScheduled = true
    requestAnimationFrame(() => this.flushEventQueue())
  }

  private flushEventQueue() {
    const batch = this.eventQueue.splice(0, EVENTS_PER_FRAME)
    for (const evt of batch) this.applyRuntimeEvent(evt)
    if (this.eventQueue.length > 0) {
      requestAnimationFrame(() => this.flushEventQueue())
    } else {
      this.eventFlushScheduled = false
    }
  }

  private applyRuntimeEvent(evt: RuntimeEvent) {
    this.transcript.handleEvent(evt)
    switch (evt.kind) {
      case 'turn.started':
        this.setSessionState('running')
        break
      case 'turn.finished':
        this.setSessionState('idle')
        this.bumpGitStamp()
        void this.refreshSessions()
        break
      case 'approval.requested':
        this.setSessionState('awaiting_approval')
        // The sidebar status light and workspace pending-approval badge are
        // fed by the session list: refresh immediately instead of waiting for
        // the 5s poll, otherwise the badge's appear/disappear visibly lags the
        // card's pop-up/collapse.
        void this.refreshSessions()
        break
      case 'approval.resolved':
        this.setSessionState('running')
        void this.refreshSessions()
        break
      case 'run.cancel_requested':
        this.setSessionState('cancelling')
        break
      case 'run.cancelled':
      case 'runtime.fatal':
        this.setSessionState('idle')
        break
      case 'budget.updated':
        // Session-cumulative accounting (usage.updated is per-call accounting
        // and does not drive the status bar)
        this.store.set({ usage: evt.payload })
        break
      case 'context.usage':
        // Live context occupancy: drives the occupancy ring next to the
        // composer (the backend re-sends a fresh value after compaction)
        this.store.set({ occupancy: evt.payload?.occupancy_tokens ?? 0 })
        break
      case 'plan.updated':
        this.store.set({ plan: evt.payload || null })
        break
      case 'tool.completed':
        // Any tool hitting disk (edit/write/run_cmd...) may have touched
        // workspace files: bump gitStamp to invalidate and refetch the right
        // panel's changes list.
        this.bumpGitStamp()
        break
      case 'reasoning.changed':
        if (evt.payload?.effective) {
          this.store.set({
            curReasoning: evt.payload.effective.effort || '',
            reasoningOverridden: !!evt.payload.overridden,
          })
        }
        break
      default:
        break
    }
  }

  async resync(reason: string) {
    console.info('resync:', reason)
    this.stream.detach()
    const sid = this.store.get().sessionId
    if (!sid) return
    try {
      await this.openSession(sid, { archived: this.store.get().archived })
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('resync failed: ' + (e as Error).message)
    }
  }

  // ---------- sharing ----------

  shareSession = async (shiftKey: boolean) => {
    const sid = this.store.get().sessionId
    if (!sid) return
    try {
      if (shiftKey) {
        const ok = await confirmDialog({
          title: '撤销分享',
          body: '撤销后，已发出的分享链接将立即失效（再次分享会生成新链接）。',
          okLabel: '撤销分享',
        })
        if (!ok) return
        await this.api.revokeShare(sid)
        toast('分享已撤销', true)
        return
      }
      // Desktop: if the share listener is off, confirm in place and enable it —
      // enabling writes through to share.enabled and hot-applies (effective
      // immediately and persisted); loom serve has no toggle (404), so fall
      // back to the current origin.
      try {
        const endpoint = await this.api.getShareEndpoint()
        if (!endpoint.enabled) {
          const ok = await confirmDialog({
            title: '开启局域网分享',
            body: '分享链接需要一个局域网可达的监听。开启后，同一网络内持有链接的人可只读查看本会话（可随时在设置 → 系统 → 局域网分享关闭）。',
            okLabel: '开启并复制链接',
          })
          if (!ok) return
          const resp = await this.api.setShareEndpoint(true)
          if (resp?.endpoint?.error) {
            toast('分享监听启动失败: ' + resp.endpoint.error)
            return
          }
        }
      } catch (err) {
        if ((err as ApiError).status !== 404) throw err
      }
      // When the share listener is online the server returns an absolute url
      // (docs/DESKTOP_DESIGN.md §5.2); otherwise fall back to joining with the
      // current origin.
      const { path, url: absoluteUrl } = await this.api.shareSession(sid)
      const url = absoluteUrl || location.origin + path
      if (await copyText(url)) {
        toast('分享链接已复制：任何持有链接的人可只读查看本会话', true)
      } else {
        // Clipboard unavailable (insecure context): open the share page and
        // copy from the address bar
        window.open(url, '_blank', 'noopener')
        toast('剪贴板不可用，已在新标签页打开分享页（可从地址栏复制链接）')
      }
    } catch (err) {
      if ((err as ApiError).status !== 401) toast('分享失败: ' + (err as Error).message)
    }
  }

  copySessionId = async () => {
    const sid = this.store.get().sessionId
    if (!sid) return
    if (await copyText(sid)) toast('session id copied', true)
    else toast('剪贴板不可用，session id: ' + sid)
  }

  // ---------- settings panel ----------

  openSettings() {
    this.store.set({ settingsOpen: true })
  }

  closeSettings() {
    this.store.set({ settingsOpen: false })
  }

  // ---------- execution-trace maze ----------

  setMainView(v: AppState['mainView']) {
    this.store.set({ mainView: v })
  }

  openCompare() {
    this.store.set({ mainView: 'compare' })
  }

  closeCompare() {
    this.store.set({ mainView: 'chat' })
  }
}
