// controller.ts — 应用控制器（框架无关）：启动编排、会话生命周期、SSE 事件
// 分批调度、连接健康、模型/reasoning 切换、工作区管理、分享、反馈。
// 逻辑与旧 static/js/main.js 一一对应；DOM 操作全部改为 AppState 状态投影，
// React 视图层订阅渲染。

import { createApi, ApiError, type Api } from '../protocol/api'
import { EventStream, type ConnState } from '../protocol/sse'
import type { RuntimeEvent, Plan, SessionState, TokenUsage } from '../protocol/events'
import type {
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
// 反馈投票本地态：key 带 session+run，存 "up"/"down"。仅作 UI 恢复用，
// 真源在 Langfuse（score id 幂等覆盖，重投不产生重复分数）。
const fbKey = (sessionId: string, runId: string) => `loom_fb_${sessionId}_${runId}`

const SESSION_PAGE_SIZE = 30
// SSE 事件分批渲染：重连追帧时事件成突发到达（几百上千条），逐事件同步
// 处理会把主线程连成多秒长任务。排队 + rAF 合帧、每帧最多 EVENTS_PER_FRAME
// 条：帧间让出主线程，追帧进度可见且界面保持响应。
const EVENTS_PER_FRAME = 120
// 重连徽标防抖：从 live 切入 connecting/reconnecting 时延迟 400ms 显示——
// 瞬态断流在延迟期内恢复 live 就完全不打断视觉。
const CONN_BADGE_DELAY_MS = 400
// 断连强提示：连续失败达到阈值（退避序列约 15s+）或进入 dead 终态时升 banner。
const CONN_WARN_ATTEMPTS = 5

export interface BannerState {
  text: string
  draining: boolean
}

export interface AppState {
  // boot：初始/鉴权校验中（什么都不渲染，避免有效 token 刷新时闪现 gate）
  view: 'boot' | 'gate' | 'app'
  gateError: string
  gateLocked: boolean // 桌面端 401：进程内凭证失效，只提示重启
  theme: 'dark' | 'light'
  sidebarCollapsed: boolean
  sessionId: string | null
  sessionState: SessionState | ''
  busy: boolean
  connState: ConnState | ''
  connDetail?: string
  banner: BannerState | null
  sessions: SessionSummary[]
  showArchived: boolean
  workspaces: Workspace[]
  noWorkspace: boolean // 零工作区引导态
  landingHint: string // 落地页文案
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
  hdrWorkspace: string // header 面包屑：当前会话所属工作区名
  hdrWorkspaceTitle: string
  // Main-area view: chat ↔ execution-trace maze (Header trace button) ↔
  // two-session trace compare (sidebar compare entry).
  mainView: 'chat' | 'maze' | 'compare'
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
  }
}

export class AppController {
  readonly store = new Store<AppState>(initialState())
  readonly transcript: TranscriptController
  readonly api: Api
  readonly stream: EventStream
  // 视图层滚动容器（resync 保留滚动位置用），由 TranscriptView 挂接
  readonly scrollerRef: { el: HTMLDivElement | null } = { el: null }

  token: string
  readonly isDesktopShell: boolean
  private sessCursor = ''
  private sessLoading = false
  private sessSig = ''
  private lastSubmit: { fp: string; key: string } | null = null // 幂等重发
  private eventQueue: RuntimeEvent[] = []
  private eventFlushScheduled = false
  // 事件水位：snapshot 的 event_seq 是服务端投影水印，序号 ≤ 它的事件效果
  // 已包含在快照里。重挂流后迟到的旧帧必然 ≤ 水位，直接丢弃，避免在重建
  // 后的 transcript 上重复渲染（同一命令两份 output 的 bug 来源）。
  private eventFloor = 0
  private connBadgeTimer: ReturnType<typeof setTimeout> | null = null
  private connPending: { state: ConnState; detail?: string } | null = null
  private connShownState: ConnState | '' = ''
  private drainPoll: ReturnType<typeof setInterval> | null = null
  // composer 草稿按会话隔离：切走前暂存当前输入文本，切回时还原。
  private composerDrafts = new Map<string, string>()
  // artifact 内容寻址缓存（blob URL），切会话时整体释放
  private artifactURLCache = new Map<string, ArtifactEntry>()
  // composer 草稿的当前文本（视图层受控组件回写，供草稿暂存）
  composerText = ''
  onComposerRestore: ((text: string) => void) | null = null

  constructor() {
    this.token = sessionStorage.getItem(TOKEN_KEY) || ''
    // Desktop shell bootstrap (docs/DESKTOP_DESIGN.md §4.2)：内嵌 token 经
    // <meta name="loom-token"> 或 URL fragment 传入；原生 message bridge
    // 只在桌面 webview 存在，且页面刷新后仍可据此识别桌面壳。
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
      resolveApproval: (payload, { decision, always }) =>
        this.api.resolveApproval(this.store.get().sessionId || '', payload.approval_id || '', {
          callId: payload.call_id,
          argsHash: payload.args_hash,
          decision,
          ruleHint: always
            ? { tool_name: payload.tool_name, arguments: payload.arguments }
            : undefined,
        }),
      answerQuestion: (questionId, answer) =>
        this.api.answerQuestion(this.store.get().sessionId || '', questionId, answer),
      // 反馈投票：成功才写 localStorage（块内选中态由视图维护）；tracing
      // 未开启 / run 无 trace 时后端返回错误码，抛回让块内回滚。
      sendFeedback: async (runId, value) => {
        const sid = this.store.get().sessionId || ''
        await this.api.submitFeedback(sid, runId, value)
        try {
          localStorage.setItem(fbKey(sid, runId), value === 1 ? 'up' : 'down')
        } catch {
          /* 隐私模式等：忽略 */
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

  // ---------- theme / sidebar 折叠 ----------

  initTheme() {
    // 默认深色（用户偏好）；仅当显式存了 "light" 才用浅色。
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
    // 窄屏（抽屉模式）默认折叠；桌面端读取持久化偏好
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

  // 窄屏抽屉模式下，选中会话后自动收起抽屉（不写入持久化偏好）
  private collapseSidebarIfNarrow() {
    if (window.matchMedia('(max-width: 767px)').matches) {
      this.store.set({ sidebarCollapsed: true })
    }
  }

  // 窄屏抽屉：点遮罩收起（不写入持久化偏好，与桌面端折叠互不影响）
  dismissSidebarDrawer() {
    this.store.set({ sidebarCollapsed: true })
  }

  expandSidebar() {
    this.store.set({ sidebarCollapsed: false })
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
      // 桌面端的 token 是进程内随机值，用户没有任何可粘贴的凭证；401 意味着
      // 进程状态异常，唯一出路是重启应用（docs/DESKTOP_DESIGN.md §4.2）。
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
    // 页面恢复可见 / 从 BFCache 还原 / 网络恢复时主动探活重连：看门狗定时器
    // 随页面一起被系统挂起（App Nap / 窗口遮挡），不主动戳永远不会发现断连。
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) this.stream.ensureLive()
    })
    window.addEventListener('pageshow', () => this.stream.ensureLive())
    // 窗口重获焦点时同时刷新会话列表：其他会话的审批/运行状态可能已在
    // 后台变化，轮询间隔内靠这次主动刷新补齐。
    window.addEventListener('focus', () => {
      this.stream.ensureLive()
      void this.refreshSessions()
    })
    window.addEventListener('online', () => this.stream.ensureLive())
    // 侧栏轮询（页面可见时，5s）
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
      // 加载模型目录（picker 数据源；modalities 随目录下发，刷新附件入口）
      await this.refreshModelCatalog()
      this.store.set({ view: 'app' })
      await this.loadWorkspaces()
      await this.refreshSessions()
      // 首入落地页：不自动打开会话（自动恢复要拉起整个 controller 运行时，
      // 且多工作区场景下「最新会话」大概率不是用户当下想做的）
      this.showLandingState()
    } catch (e) {
      if ((e as ApiError).status !== 401) this.showGate('connect failed: ' + (e as Error).message)
    }
  }

  // ---------- 连接徽标 / banner ----------

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
    if (state === 'draining') return // onDraining 已升 banner
    if (state === 'dead') {
      this.showConnBanner(`连接已断开：${detail || 'disconnected'}`)
    } else if (typeof attempt === 'number' && attempt >= CONN_WARN_ATTEMPTS) {
      this.showConnBanner('连接已断开，正在自动重连…')
    }
  }

  // draining 自愈：优雅停机后服务通常很快以新实例回归（桌面端重启、部署
  // 滚动）。慢速轮询版本端点，可达即 resync。
  private startDrainRecovery() {
    if (this.drainPoll) return
    this.drainPoll = setInterval(() => {
      void (async () => {
        try {
          await this.api.metaVersion()
          void this.resync('drain_recovered')
        } catch {
          /* 仍未恢复：下一轮再试 */
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

  // ---------- 会话状态 ----------

  private setSessionState(state: SessionState | '') {
    const busy = state === 'running' || state === 'awaiting_approval' || state === 'cancelling'
    this.store.set({ sessionState: state, busy })
  }

  // 只读模式有两个来源：子 agent 会话（snap.delegated）与已归档会话。
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
    void locked // readOnlyLabel 为 '' 即未锁定（视图层据此判态）
  }

  private setReadOnly(snap: Snapshot) {
    this.store.set({
      readOnly: !!snap.delegated,
      readOnlyTitle: snap.parent_session_id ? `parent: ${snap.parent_session_id}` : '',
    })
    this.updateComposerLock()
  }

  // setArchived 切换当前会话的归档只读态。
  setArchived(archived: boolean) {
    this.store.set({ archived })
    this.updateComposerLock()
  }

  // ---------- artifact / 工具输出 ----------

  // artifact 加载：fetch 拉取后生成 blob URL（artifact 是内容寻址的不可变
  // blob，按 id+size 缓存避免 snapshot/实时两条路径重复下载）。
  fetchArtifactURL = async (id: string, size: number): Promise<ArtifactEntry> => {
    const key = `${id}:${size}`
    const cached = this.artifactURLCache.get(key)
    if (cached) return cached
    const res = await fetch(`/v1/artifacts/${encodeURIComponent(id)}?size=${size}`, {
      headers: { Authorization: 'Bearer ' + this.token },
    })
    if (!res.ok) throw new Error(`artifact fetch failed (HTTP ${res.status})`)
    const blob = await res.blob()
    const entry: ArtifactEntry = {
      url: URL.createObjectURL(blob),
      mediaType: blob.type || '',
      blob,
    }
    this.artifactURLCache.set(key, entry)
    return entry
  }

  // 切会话时释放全部 artifact blob URL，避免频繁带图会话里缓存无界增长。
  private clearArtifactURLCache() {
    for (const entry of this.artifactURLCache.values()) URL.revokeObjectURL(entry.url)
    this.artifactURLCache.clear()
  }

  // 复制完整工具输出：实时 tool.completed 事件只带有界 preview，
  // 完整内容从 snapshot 消息历史里按 call_id 取。
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

  // ---------- model / reasoning 状态同步 ----------

  modelLabel(ref: string): string {
    // 只显示 model 名（去掉 provider 前缀），更紧凑
    return ref ? ref.split('/').pop() || 'model' : 'model'
  }

  // refreshModelCatalog 重新拉取模型目录并同步所有消费方。桌面壳没有 F5，
  // 设置保存热应用后必须就地刷新，否则 modalities 等改动在界面上不可见。
  refreshModelCatalog = async () => {
    try {
      const cat = await this.api.metaModels()
      this.store.set({ models: cat.models || [], defaultModelRef: cat.default || '' })
      this.syncAttachCapability()
    } catch (e) {
      if ((e as ApiError).status !== 401) console.warn('refresh models:', e)
    }
  }

  // syncAttachCapability 按当前模型声明的 modalities 门控 composer 的图片
  // 附件入口。目录里查不到当前模型（配置过期等）时保持放行，由服务端
  // 提交门禁兜底报错。
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
      // 直接采用 picker 的 ref：它与列表项的 currentRef 比较同源，必然匹配。
      // （SetModelResult 无 JSON tag，响应键是大写的 Cur/Meta，拼读易错。）
      this.store.set({ curModelRef: ref })
      this.syncAttachCapability()
      // 模型切换后窗口阈值变化：按服务端推导的新窗口投影重设占用环，
      // occupancy 等下一次 context.usage / snapshot 刷新
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
      // 同上：SetReasoningResult 响应键是大写 Effective/Overridden；effort
      // 来自 picker 固定选项集，直接采用。
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

  // 会话列表签名：轮询时数据无变化则跳过重渲染，避免每 5s 重建视图
  // 打断侧栏悬停操作（归档/删除按钮）。视图（活跃/归档）参与签名。
  private sessionListSig(list: SessionSummary[]): string {
    return (
      this.store.get().showArchived +
      '|' +
      list.map((s) => `${s.id}:${s.state}:${s.updated_at}:${s.title}`).join(',')
    )
  }

  // 工作区签名：注册/删除一个（空）工作区不会改变会话列表，必须让工作区
  // 集合本身参与 refreshSessions 的跳过判断。
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

  // 瀑布流：滚动接近底部时拉取下一页。
  loadMoreSessions = async () => {
    if (this.sessLoading || !this.sessCursor) return
    this.sessLoading = true
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
    }
  }

  toggleArchivedView() {
    const showArchived = !this.store.get().showArchived
    this.sessCursor = ''
    this.sessSig = ''
    this.store.set({ showArchived, sessions: [] })
    void this.refreshSessions()
  }

  // 归档 / 取消归档 / 删除会话（侧栏条目操作）
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
          // 删的是当前打开的会话：断开流、回空态（只读态一并复位）
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
        // 归档/取消归档的是当前打开的会话：同步输入区只读态
        if (id === this.store.get().sessionId) this.setArchived(action === 'archive')
      }
    } catch (e) {
      if ((e as ApiError).status !== 401) toast('操作失败: ' + (e as Error).message)
    }
    await this.refreshSessions()
  }

  // composer 草稿按会话隔离：切走前暂存当前输入文本，切回时还原。
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
    // 同会话重开 = resync（断流恢复/手动重试）：保留滚动位置，不拽回底部。
    const isResync = this.store.get().sessionId === id
    // 归档只读态：立即刷新锁态——snapshot/resume 失败时输入区也不能残留
    // 上一个会话的状态。
    this.store.set({ archived })
    this.updateComposerLock()
    this.stashComposerDraft()
    this.clearArtifactURLCache()
    this.stream.detach()
    // detach 后追帧队列里残留的都是旧会话/旧快照之前的事件，snapshot 已覆盖，直接丢弃。
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
        // 非 live：先 resume 再取快照
        await this.api.resumeSession(id)
        snap = await this.api.snapshot(id)
      } else {
        throw e
      }
    }
    // resync 保留滚动：视图层在 DOM 重建后恢复 scrollTop
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
      // ctx 占用环：window 阈值与 occupancy 均由服务端投影（与压缩触发器
      // 同口径），前端不做本地推算
      window: snap.window && snap.window.effective ? (snap.window as ContextWindow) : null,
      occupancy: snap.occupancy || 0,
    })
    // attach 前再清一次追帧队列：快照拉取期间旧连接 dispatched 的残余事件
    // 要么已被快照覆盖（seq ≤ event_seq），要么会被新流从 event_seq 重放，
    // 丢弃是无损的。
    this.eventFloor = snap.event_seq || 0
    this.eventQueue.length = 0
    this.stream.attach(id, snap.event_seq || 0)
  }

  onSelectSession = (id: string) => {
    if (id === this.store.get().sessionId) return
    // 从归档视图点开的会话 = 已归档（只读）；默认视图 = 活跃会话
    this.openSession(id, { archived: this.store.get().showArchived }).catch((e) => {
      if ((e as ApiError).status !== 401) toast('open session: ' + (e as Error).message)
    })
  }

  // header 面包屑：当前会话所属工作区名。
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

  // 面包屑点击：展开侧栏（若折叠）并定位当前会话所属的工作区组
  revealCurrentWorkspace(): string | null {
    const s = this.store.get()
    if (!s.sessionId) return null
    const sess = s.sessions.find((x) => x.id === s.sessionId)
    if (!sess) return null
    this.expandSidebar()
    return sess.workspace_id || ''
  }

  // ---------- workspace 管理 ----------

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
      // 添加首个工作区后从引导态恢复到落地页（仅当没有打开的会话）
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

  // 删除工作区（侧栏工作区节点操作）：级联删除其下全部会话；磁盘目录不动。
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
    // 当前打开的会话如果属于被删工作区，断开流并清空 transcript
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
    // 删除后可能进入零工作区引导态；否则回落地页（仅当没有打开的会话）
    if (this.syncWorkspaceGate()) return
    if (!this.store.get().sessionId) this.showLandingState()
  }

  // 零工作区引导态：无任何工作区时隐藏对话区和侧栏列表。
  private syncWorkspaceGate(): boolean {
    const noWorkspace = this.store.get().workspaces.length === 0
    this.store.set({ noWorkspace })
    return noWorkspace
  }

  // 首入落地页：不自动打开任何会话（桌面端启动应是廉价且可预期的）。
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

  // 最近活跃工作区：会话列表按更新时间排序（newest first），取第一项的归属。
  // 新建会话（直接打字/回车）的默认落点，避免落到进程默认工作区。
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
    // followup 仅文本：图片随普通 prompt 发送（后端同样拒绝 followup+图片）
    if (followup && images.length) {
      toast('排队到下一轮的消息仅支持文本，图片已忽略')
      images = []
    }
    // 幂等键：同一「文本 + 图片集合 + 投递方式」重发共享同键（双击/网络重试不产生重复 turn）
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
      // 失败时仅还原文本，附件留在 composer 以便重试
      this.onComposerRestore?.(text)
      // 会话在此期间被归档（手动/自动）：切换为只读并引导取消归档
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
    // 上一会话的迟到帧（切换会话后旧连接的残余）不进入新会话的视图。
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
        void this.refreshSessions()
        break
      case 'approval.requested':
        this.setSessionState('awaiting_approval')
        // 侧栏状态灯与工作区待审批徽标的数据源是会话列表：立即刷新，
        // 不等 5s 轮询，否则徽标的出现/消失明显滞后于卡片的弹出/收起。
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
        // 会话累计口径（usage.updated 是单次调用口径，不驱动状态栏）
        this.store.set({ usage: evt.payload })
        break
      case 'context.usage':
        // 实时 context 占用：驱动 composer 旁的占用环（压缩后后端会补发新值）
        this.store.set({ occupancy: evt.payload?.occupancy_tokens ?? 0 })
        break
      case 'plan.updated':
        this.store.set({ plan: evt.payload || null })
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

  // ---------- 分享 ----------

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
      // 桌面端：分享监听未开启时就地确认并开启——开启动作写穿到
      // share.enabled 并热应用（即时生效且持久）；loom serve 无开关
      // （404），直接用当前 origin 拼接。
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
      // 分享监听在线时服务端返回绝对 url（docs/DESKTOP_DESIGN.md §5.2）；
      // 缺省退回按当前 origin 拼接。
      const { path, url: absoluteUrl } = await this.api.shareSession(sid)
      const url = absoluteUrl || location.origin + path
      if (await copyText(url)) {
        toast('分享链接已复制：任何持有链接的人可只读查看本会话', true)
      } else {
        // 剪贴板不可用（非安全上下文）：打开分享页，从地址栏复制
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

  // ---------- 设置面板 ----------

  openSettings() {
    this.store.set({ settingsOpen: true })
  }

  closeSettings() {
    this.store.set({ settingsOpen: false })
  }

  // ---------- execution-trace maze ----------

  // Chat ↔ maze main-area toggle (Header trace button). From the compare
  // view the button lands on the maze (it is the trace entry point).
  toggleMainView() {
    const v = this.store.get().mainView
    this.store.set({ mainView: v === 'chat' ? 'maze' : v === 'maze' ? 'chat' : 'maze' })
  }

  openCompare() {
    this.store.set({ mainView: 'compare' })
  }

  closeCompare() {
    this.store.set({ mainView: 'chat' })
  }
}
