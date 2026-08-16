// main.js — 启动编排（docs/WEB_DESIGN.md §3.1）：
// token gate → meta 验活 → 模型目录 → 会话列表 → snapshot 首屏 → SSE 直播。
// composer 内嵌模型/reasoning 切换器。

import { createApi } from './api.js'
import { EventStream } from './sse.js'
import { Transcript } from './components/transcript.js'
import { renderPlanInto } from './components/blocks.js'
import { Sidebar } from './components/sidebar.js'
import { Composer } from './components/composer.js'
import { Statusbar } from './components/statusbar.js'
import { CtxGauge } from './components/ctxgauge.js'
import { Picker } from './components/picker.js'
import { SettingsPanel } from './components/settings.js'
import { initTooltips } from './components/tooltip.js'
import { shortId, estTranscriptTokens, randomId, copyText } from './format.js'
import { icon, hydrateIcons } from './icons.js'

const TOKEN_KEY = 'loom_token'
const THEME_KEY = 'loom_theme'
const SIDEBAR_KEY = 'loom_sidebar_collapsed'
// 反馈投票本地态：key 带 session+run，存 "up"/"down"。仅作 UI 恢复用，
// 真源在 Langfuse（score id 幂等覆盖，重投不产生重复分数）。
const fbKey = (sessionId, runId) => `loom_fb_${sessionId}_${runId}`

const $ = (id) => document.getElementById(id)

// Desktop shell bootstrap (docs/DESKTOP_DESIGN.md §4.2): the embedding
// desktop app passes its in-process token either via a <meta name="loom-token">
// tag rendered into index.html, or via URL fragment (#token=...) — fragments
// never reach the wire. Persist to sessionStorage before the gate reads it.
const embeddedToken =
  document.querySelector('meta[name="loom-token"]')?.content ||
  '' ||
  new URLSearchParams(location.hash.slice(1)).get('token') ||
  ''
// In the desktop shell the token is the process's random in-process one —
// no user-pasteable credential exists, so the token gate is pointless there.
// The #token fragment is stripped from the URL right after boot (see the
// history.replaceState below), so a webview reload arrives with no token
// and must still be recognized as the desktop shell — otherwise the
// desktop-only chrome (hidden 工作区 title, drag regions) silently
// reverts to browser styling. The native message bridge exists only
// inside the desktop webview and survives reloads.
const isDesktopShell = embeddedToken !== '' || !!window.webkit?.messageHandlers?.external
// 桌面端隐藏了原生标题栏（mac.TitleBarHidden），红绿灯悬浮在内容之上；
// body.is-desktop 用于开启让位/拖动区样式（见 app.css 末尾）。
if (isDesktopShell) {
  document.body.classList.add('is-desktop')
  // 窗口拖动：Wails 的 --wails-draggable 在其前端 runtime（/wails/runtime.js）
  // 里实现，但桌面端 SPA 跑在 loopback origin（SSE 需要真实 HTTP，见
  // DESKTOP_DESIGN.md §2.3），加载不到该文件，这里自行实现等价逻辑。
  // 原生链路：external message handler 注册在 WKUserContentController 上
  // （与 origin 无关），收到 "drag" 即 performWindowDragWithEvent:，
  // mouseEvent 由 NSEvent 本地监听器跟踪（WailsContext.m）。
  const wailsDrag = window.webkit?.messageHandlers?.external
  if (wailsDrag) {
    let dragArmed = false
    window.addEventListener('mousedown', (e) => {
      // 仅左键单击命中 drag 区域时武装；双击保留给系统（窗口缩放）
      dragArmed =
        e.buttons === 1 &&
        e.detail === 1 &&
        getComputedStyle(e.target).getPropertyValue('--wails-draggable').trim() === 'drag'
    })
    window.addEventListener('mousemove', (e) => {
      if (dragArmed && e.buttons === 1) {
        dragArmed = false
        wailsDrag.postMessage('drag')
      }
    })
    window.addEventListener('mouseup', () => {
      dragArmed = false
    })
  }
  // 失焦时 macOS 会把红绿灯绘成非激活态（透明标题栏下呈黑色，由 AppKit
  // 绘制，web 层改不了按钮本身）；同步给顶行加淡化样式，呈现统一的原生
  // 非激活观感。
  window.addEventListener('blur', () => document.body.classList.add('is-inactive'))
  window.addEventListener('focus', () => document.body.classList.remove('is-inactive'))
}
if (embeddedToken) {
  sessionStorage.setItem(TOKEN_KEY, embeddedToken)
  if (location.hash) history.replaceState(null, '', location.pathname)
}

// ---------- theme ----------
function initTheme() {
  // 默认深色（用户偏好）；仅当显式存了 "light" 才用浅色。
  const saved = sessionStorage.getItem(THEME_KEY)
  const dark = saved !== 'light'
  document.documentElement.dataset.theme = dark ? 'dark' : 'light'
  // 图标固定为对比度圆（sun 在浅色模式下与设置齿轮撞脸）；当前模式的
  // 指示由动态 title 承担（tooltip.js 会接管为自绘提示）
  const syncTitle = () => {
    $('hdr-theme').title =
      document.documentElement.dataset.theme === 'dark' ? '切换到浅色模式' : '切换到深色模式'
  }
  syncTitle()
  $('hdr-theme').onclick = () => {
    const nowDark = document.documentElement.dataset.theme === 'dark'
    document.documentElement.dataset.theme = nowDark ? 'light' : 'dark'
    sessionStorage.setItem(THEME_KEY, nowDark ? 'light' : 'dark')
    syncTitle()
  }
}

// ---------- sidebar 折叠 ----------
const NARROW_MQ = '(max-width: 767px)'

function initSidebarToggle() {
  const shell = $('app')
  // 窄屏（抽屉模式）默认折叠；桌面端读取持久化偏好
  const stored = localStorage.getItem(SIDEBAR_KEY)
  const collapsed = stored === '1' || (stored === null && window.matchMedia(NARROW_MQ).matches)
  shell.classList.toggle('sidebar-collapsed', collapsed)
  $('hdr-sidebar').onclick = () => {
    const now = shell.classList.toggle('sidebar-collapsed')
    localStorage.setItem(SIDEBAR_KEY, now ? '1' : '0')
  }
}

// 窄屏抽屉模式下，选中会话后自动收起抽屉（不写入持久化偏好）
function collapseSidebarIfNarrow() {
  if (window.matchMedia(NARROW_MQ).matches) $('app').classList.add('sidebar-collapsed')
}

// ---------- toast ----------
function toast(msg, info) {
  const t = document.createElement('div')
  t.className = 'toast' + (info ? ' is-info' : '')
  t.textContent = msg
  $('toasts').appendChild(t)
  setTimeout(() => t.remove(), 5000)
}

// ---------- 确认弹窗（替代原生 confirm） ----------
// confirmDialog({title, body, okLabel}) → Promise<boolean>
// Esc / 点遮罩 = 取消；Enter = 确认。
function confirmDialog({ title, body, okLabel }) {
  return new Promise((resolve) => {
    const wrap = $('confirm-modal')
    const ok = $('confirm-ok')
    const cancel = $('confirm-cancel')
    $('confirm-title').textContent = title
    $('confirm-body').textContent = body
    ok.textContent = okLabel || '确认'
    wrap.hidden = false
    ok.focus()
    const done = (v) => {
      wrap.hidden = true
      ok.onclick = cancel.onclick = wrap.onclick = null
      document.removeEventListener('keydown', onKey, true)
      resolve(v)
    }
    const onKey = (e) => {
      if (e.key === 'Escape') {
        e.stopPropagation()
        done(false)
      } else if (e.key === 'Enter') {
        e.stopPropagation()
        done(true)
      }
    }
    document.addEventListener('keydown', onKey, true)
    ok.onclick = () => done(true)
    cancel.onclick = () => done(false)
    wrap.onclick = (e) => {
      if (e.target === wrap) done(false)
    }
  })
}

// ---------- app ----------
const app = {
  token: sessionStorage.getItem(TOKEN_KEY) || '',
  sessionId: null,
  busy: false,
  api: null,
  stream: null,
  transcript: null,
  sidebar: null,
  composer: null,
  statusbar: null,
  ctxgauge: null,
  picker: null,
  models: [], // [{provider, name, context_window}]
  defaultModelRef: '', // "provider/model"
  curModelRef: '', // 当前会话选中
  curReasoning: '', // 当前会话 reasoning effort
  reasoningOverridden: false,
  lastSubmit: null, // {fp, key} —— 幂等重发（fp 含图片指纹）
  sessionList: [], // 已加载的会话列表（分页累加）
  sessCursor: '', // 下一页游标（"" = 没有更多）
  sessLoading: false,
  showArchived: false, // 侧栏归档视图开关
  readOnly: false, // 当前会话为只读子 agent 会话
  workspaces: [], // 已注册工作区 [{id, name, root_path, session_count}]
}

function setBadge(el, cls, text) {
  el.className = 'badge' + (cls ? ' ' + cls : '')
  el.querySelector('.txt').textContent = text
}

function renderConnBadge(state, detail) {
  connShownState = state
  const map = {
    connecting: ['is-reconnecting', 'connecting…'],
    live: ['is-live', 'live'],
    reconnecting: ['is-reconnecting', detail ? `reconnecting (${detail})` : 'reconnecting…'],
    draining: ['is-draining', 'draining'],
    dead: ['is-dead', detail || 'disconnected'],
  }
  const [cls, text] = map[state] || ['', state]
  setBadge($('hdr-conn'), cls, text)
}

// 重连徽标防抖：从 live 切入 connecting/reconnecting 时延迟 400ms 显示——
// 瞬态断流（遮挡恢复、ensureLive 主动重连）在延迟期内恢复 live 就完全
// 不打断视觉；只有真正持续的重连才亮徽标。live/dead/draining 立即生效。
const CONN_BADGE_DELAY_MS = 400
let connBadgeTimer = null
let connPending = null // 延迟期内最新一次重连态 {state, detail}
let connShownState = '' // 徽标当前实际展示的 state

function setConn(state, detail) {
  const transient = state === 'connecting' || state === 'reconnecting'
  const showingTransient = connShownState === 'connecting' || connShownState === 'reconnecting'
  if (transient && !showingTransient) {
    connPending = { state, detail }
    if (!connBadgeTimer) {
      connBadgeTimer = setTimeout(() => {
        connBadgeTimer = null
        const p = connPending
        connPending = null
        if (p) renderConnBadge(p.state, p.detail)
      }, CONN_BADGE_DELAY_MS)
    }
    return
  }
  if (connBadgeTimer) {
    clearTimeout(connBadgeTimer)
    connBadgeTimer = null
    connPending = null
  }
  renderConnBadge(state, detail)
}

// 断连强提示：前几次自动重连只更新徽标（瞬态抖动不打扰）；连续失败达到
// 阈值（退避序列约 15s+）或进入 dead 终态（429 流数超限）时升起 banner，
// 附手动重试——重试走 resync（快照重建 + 重挂流），是最稳的恢复路径。
const CONN_WARN_ATTEMPTS = 5

function connHealth(state, detail, attempt) {
  if (state === 'live') {
    stopDrainRecovery()
    $('banner').hidden = true
    return
  }
  if (state === 'draining') return // onDraining 已升 banner
  if (state === 'dead') {
    showConnBanner(`连接已断开：${detail || 'disconnected'}`)
  } else if (typeof attempt === 'number' && attempt >= CONN_WARN_ATTEMPTS) {
    showConnBanner('连接已断开，正在自动重连…')
  }
}

// draining 自愈：优雅停机后服务通常很快以新实例回归（桌面端重启、部署
// 滚动）。慢速轮询版本端点，可达即 resync（快照重建 + 以新实例重挂流），
// 不再要求用户手动刷新。
let drainPoll = null

function startDrainRecovery() {
  if (drainPoll) return
  drainPoll = setInterval(async () => {
    try {
      await app.api.metaVersion()
      resync('drain_recovered')
    } catch {
      /* 仍未恢复：下一轮再试 */
    }
  }, 10_000)
}

function stopDrainRecovery() {
  if (drainPoll) {
    clearInterval(drainPoll)
    drainPoll = null
  }
}

function showConnBanner(text) {
  const b = $('banner')
  b.textContent = ''
  const msg = document.createElement('span')
  msg.innerHTML = icon('triangle-exclamation')
  msg.appendChild(document.createTextNode(' ' + text))
  const retry = document.createElement('button')
  retry.className = 'banner-retry'
  retry.type = 'button'
  retry.textContent = '立即重试'
  retry.onclick = () => resync('manual_retry')
  b.appendChild(msg)
  b.appendChild(retry)
  b.hidden = false
}

function setSessionState(state) {
  const map = {
    idle: ['', 'idle'],
    running: ['is-running', 'running'],
    awaiting_approval: ['is-awaiting', 'awaiting approval'],
    cancelling: ['is-awaiting', 'cancelling'],
    booting: ['', 'booting'],
    fatal: ['is-dead', 'fatal'],
    closed: ['', 'closed'],
  }
  const [cls, text] = map[state] || ['', state || '']
  setBadge($('hdr-state'), cls, text)
  app.busy = state === 'running' || state === 'awaiting_approval' || state === 'cancelling'
  app.composer.setRunning(app.busy)
  $('cancel-btn').hidden = !app.busy
}

function showGate(err) {
  app.stream?.detach()
  $('app').hidden = true
  $('gate').hidden = false
  const e = $('gate-error')
  if (err) {
    e.textContent = err
    e.hidden = false
  } else {
    e.hidden = true
  }
}

function showApp() {
  $('gate').hidden = true
  $('app').hidden = false
}

function onUnauthorized() {
  sessionStorage.removeItem(TOKEN_KEY)
  app.token = ''
  if (isDesktopShell) {
    // 桌面端的 token 是进程内随机值，用户没有任何可粘贴的凭证；401 意味着
    // 进程状态异常，唯一出路是重启应用（docs/DESKTOP_DESIGN.md §4.2）。
    showGate('桌面端鉴权状态丢失，请重启应用')
    $('gate-token').hidden = true
    $('gate-form').querySelector('button').hidden = true
    return
  }
  showGate('token invalid or expired — paste the current serve token')
}

// artifact 加载：<img>/fetch 无法携带 Authorization 头，而 /v1/* 需要
// Bearer 鉴权，因此用 fetch 拉取后生成 blob URL。返回 {url, mediaType, blob}
// ——mediaType 取自响应 Content-Type（服务端 DetectContentType 嗅探），供
// 渲染层区分图片与文本 artifact（如 run_cmd 的 stdout）；blob 本体用于文
// 本预览（blob.text()，规避 CSP connect-src 对 blob: fetch 的限制）。
// artifact 是内容寻址的不可变 blob，按 id+size 缓存可避免同一 blob 在
// snapshot/实时两条路径重复下载；缓存随页面生命周期存续。
const artifactURLCache = new Map()
async function fetchArtifactURL(id, size) {
  const key = `${id}:${size}`
  const cached = artifactURLCache.get(key)
  if (cached) return cached
  const res = await fetch(`/v1/artifacts/${encodeURIComponent(id)}?size=${size}`, {
    headers: { Authorization: 'Bearer ' + app.token },
  })
  if (!res.ok) throw new Error(`artifact fetch failed (HTTP ${res.status})`)
  const blob = await res.blob()
  const entry = { url: URL.createObjectURL(blob), mediaType: blob.type || '', blob }
  artifactURLCache.set(key, entry)
  return entry
}

// 切会话时释放全部 artifact blob URL，避免频繁带图会话里缓存无界增长。
// artifact 是内容寻址的不可变 blob，新会话再次引用时重新 fetch 即可。
function clearArtifactURLCache() {
  for (const entry of artifactURLCache.values()) URL.revokeObjectURL(entry.url)
  artifactURLCache.clear()
}

// 复制完整工具输出：实时 tool.completed 事件只带有界 preview，
// 完整内容从 snapshot 消息历史里按 call_id 取。
async function fetchToolOutput(callId) {
  if (!app.sessionId || !callId) throw new Error('no active session')
  const snap = await app.api.snapshot(app.sessionId)
  for (const m of snap.messages || []) {
    for (const part of m.parts || []) {
      const r = part.kind === 'tool_result' ? part.tool_result : null
      if (!r || r.call_id !== callId) continue
      const texts = (r.content || []).filter((c) => c.kind === 'text' && c.text).map((c) => c.text)
      const out = texts.join('\n')
      if (out) return out
      if (r.error && r.error.message) return r.error.message
      throw new Error('tool output unavailable (empty or compacted)')
    }
  }
  throw new Error('tool result not found in session history')
}

// ---------- model / reasoning 状态同步 ----------

function modelLabel(ref) {
  // 只显示 model 名（去掉 provider 前缀），更紧凑
  return ref ? ref.split('/').pop() : 'model'
}

function reasoningLabel(effort, overridden) {
  const e = effort || 'default'
  const map = {
    default: 'reasoning',
    off: 'reasoning: off',
    low: 'reasoning: low',
    medium: 'reasoning: medium',
    high: 'reasoning: high',
  }
  // ★ 覆盖标记为静态图标（icons.js），拼接处无用户输入
  return map[e] + (overridden && e !== 'default' ? icon('star') : '')
}

function syncPickerLabels() {
  $('model-btn').querySelector('.picker-label').textContent = modelLabel(app.curModelRef)
  $('reasoning-btn').querySelector('.picker-label').innerHTML = reasoningLabel(
    app.curReasoning,
    app.reasoningOverridden,
  )
  syncAttachCapability()
}

// refreshModelCatalog 重新拉取模型目录并同步所有消费方：picker 角标
//（modalities 图片徽标 / context_window）、模型按钮标签、composer 附件
// 门控。桌面壳没有 F5，设置保存热应用后必须就地刷新，否则 modalities
// 等模型元数据改动在界面上不可见。
async function refreshModelCatalog() {
  try {
    const cat = await app.api.metaModels()
    app.models = cat.models || []
    app.defaultModelRef = cat.default || ''
    syncPickerLabels()
  } catch (e) {
    if (e.status !== 401) console.warn('refresh models:', e)
  }
}

// syncAttachCapability 按当前模型声明的 modalities 门控 composer 的图片
// 附件入口。目录里查不到当前模型（配置过期等）时保持放行，由服务端
// 提交门禁兜底报错。
function syncAttachCapability() {
  if (!app.composer) return
  const entry = app.models.find((m) => m.provider + '/' + m.name === app.curModelRef)
  if (!entry) {
    app.composer.setImagesEnabled(true)
    return
  }
  const ok = (entry.modalities || []).includes('image')
  app.composer.setImagesEnabled(
    ok,
    ok
      ? ''
      : `模型 ${entry.name} 未声明图片输入（modalities）；请切换多模态模型，或在设置 → 模型中勾选「图片输入」`,
  )
}

function applySnapshotMeta(snap) {
  if (snap.provider_name && snap.model_name) {
    app.curModelRef = snap.provider_name + '/' + snap.model_name
  } else if (snap.model_name) {
    app.curModelRef = app.defaultModelRef || snap.model_name
  }
  app.curReasoning = snap.reasoning_effort || ''
  app.reasoningOverridden = !!snap.reasoning_overridden
  syncPickerLabels()
}

// ---------- session loading ----------

const SESSION_PAGE_SIZE = 30

// 刷新 = 重拉「已加载数量」大小的一页并整列替换：新会话/状态变化都能
// 体现，且不丢已展开的浏览深度（瀑布流页数）。
// 会话列表签名：轮询时数据无变化则跳过重渲染，避免每 5s 重建 DOM
// 打断侧栏悬停操作（归档/删除按钮）。视图（活跃/归档）参与签名。
function sessionListSig(list) {
  return (
    app.showArchived +
    '|' +
    list.map((s) => `${s.id}:${s.state}:${s.updated_at}:${s.title}`).join(',')
  )
}

// 工作区签名：侧栏按工作区分组渲染，注册/删除一个（空）工作区不会改变
// 会话列表，必须让工作区集合本身参与 refreshSessions 的跳过判断，否则
// 新工作区要等手动刷新页面才出现。
function workspacesSig(list) {
  return (list || [])
    .map((w) => `${w.id}:${w.name || ''}:${w.root_path || ''}:${w.session_count}`)
    .join(',')
}

async function refreshSessions() {
  try {
    const limit = Math.max(app.sessionList.length, SESSION_PAGE_SIZE)
    const { sessions, next_cursor } = await app.api.listSessions(limit, '', app.showArchived, 'all')
    app.sessionList = sessions || []
    app.sessCursor = next_cursor || ''
    const sig = sessionListSig(app.sessionList) + '|' + workspacesSig(app.workspaces)
    if (sig !== app.sessSig) {
      app.sessSig = sig
      app.sidebar.render(app.sessionList, app.workspaces)
      if (app.sessionId) app.sidebar.setActive(app.sessionId)
    }
    if (app.sessionId) syncHdrWorkspace(app.sessionId)
  } catch (e) {
    if (e.status !== 401) console.warn('list sessions:', e)
  }
}

// 瀑布流：滚动接近底部时拉取下一页。
async function loadMoreSessions() {
  if (app.sessLoading || !app.sessCursor) return
  app.sessLoading = true
  try {
    const { sessions, next_cursor } = await app.api.listSessions(
      SESSION_PAGE_SIZE,
      app.sessCursor,
      app.showArchived,
      'all',
    )
    app.sessionList = app.sessionList.concat(sessions || [])
    app.sessCursor = next_cursor || ''
    app.sessSig = sessionListSig(app.sessionList) + '|' + workspacesSig(app.workspaces)
    app.sidebar.render(app.sessionList, app.workspaces)
    if (app.sessionId) app.sidebar.setActive(app.sessionId)
  } catch (e) {
    if (e.status !== 401) console.warn('load more sessions:', e)
  } finally {
    app.sessLoading = false
  }
}

// 归档 / 取消归档 / 删除会话（侧栏条目操作）
async function onSessionAction(id, action) {
  try {
    if (action === 'delete') {
      const sess = app.sessionList.find((x) => x.id === id)
      const title = (sess && sess.title) || shortId(id)
      const ok = await confirmDialog({
        title: '删除会话',
        body: `「${title}」将被永久删除，包括全部消息与事件记录。该操作不可恢复。`,
        okLabel: '删除',
      })
      if (!ok) return
      await app.api.deleteSession(id)
      if (id === app.sessionId) {
        // 删的是当前打开的会话：断开流、回空态
        app.stream.detach()
        app.sessionId = null
        app.transcript.clear()
        app.ctxgauge.reset()
        renderPlanInto($('plan-panel'), null)
        $('hdr-session').hidden = true
        $('hdr-share').hidden = true
        $('hdr-ws').hidden = true
        $('empty-state').hidden = false
        setSessionState('closed')
      }
      toast('会话已删除', true)
    } else {
      await app.api.archiveSession(id, action === 'archive')
      toast(action === 'archive' ? '已归档' : '已取消归档', true)
    }
  } catch (e) {
    if (e.status !== 401) toast('操作失败: ' + e.message)
  }
  await refreshSessions()
}

// 只读模式（子 agent 会话）：composer/模型切换禁用；审批与提问卡片不受影响。
function setReadOnly(snap) {
  app.readOnly = !!snap.delegated
  app.composer.setReadOnly(app.readOnly)
  $('send-btn').disabled = app.readOnly
  $('model-btn').disabled = app.readOnly
  $('reasoning-btn').disabled = app.readOnly
  const badge = $('hdr-readonly')
  badge.hidden = !app.readOnly
  badge.title = snap.parent_session_id ? `parent: ${snap.parent_session_id}` : ''
}

// composer 草稿按会话隔离：切走前暂存当前输入文本，切回时还原。
// 附件（blob URL 预览）不跨会话保留——它们是针对当时会话语境挑选的。
const composerDrafts = new Map()

function stashComposerDraft() {
  if (!app.sessionId || !app.composer) return
  const d = app.composer.draft()
  if (d) composerDrafts.set(app.sessionId, d)
  else composerDrafts.delete(app.sessionId)
}

function restoreComposerDraft(id) {
  if (!app.composer) return
  app.composer.clearDraft()
  const saved = composerDrafts.get(id)
  if (saved) app.composer.restoreDraft(saved)
  composerDrafts.delete(id)
}

async function openSession(id) {
  // 同会话重开 = resync（断流恢复/手动重试）：保留滚动位置，不拽回底部。
  const isResync = app.sessionId === id
  stashComposerDraft()
  clearArtifactURLCache()
  app.stream.detach()
  // detach 后追帧队列里残留的都是旧会话/旧快照之前的事件，snapshot 已覆盖，直接丢弃。
  eventQueue.length = 0
  app.sessionId = id
  restoreComposerDraft(id)
  app.sidebar.setActive(id, { scroll: true })
  $('empty-state').hidden = true
  $('hdr-session').hidden = false
  $('hdr-share').hidden = false
  $('hdr-session').textContent = shortId(id)
  syncHdrWorkspace(id)

  let snap
  try {
    snap = await app.api.snapshot(id)
  } catch (e) {
    if (e.status === 404) {
      // 非 live：先 resume 再取快照
      await app.api.resumeSession(id)
      snap = await app.api.snapshot(id)
    } else {
      throw e
    }
  }
  app.transcript.applySnapshot(snap, { preserveScroll: isResync })
  renderPlanInto($('plan-panel'), snap.plan)
  setReadOnly(snap)
  setSessionState(snap.state)
  applySnapshotMeta(snap)
  app.statusbar.setUsage(snap.usage)
  app.statusbar.setTurns(snap.turn_count)
  // ctx 占用环：snapshot.window 优先（旧服务端回退名义窗口推导）；
  // occupancy 缺省时按 snapshot 消息本地估算（与后端 estTokens 同算法）
  app.ctxgauge.setWindow(snap.window, snap.context_window)
  app.ctxgauge.setOccupancy(snap.occupancy || estTranscriptTokens(snap.messages))
  app.stream.attach(id, snap.event_seq || 0)
}

// header 面包屑：当前会话所属工作区名。即使侧栏所有工作区组都被折叠，
// 也能一眼定位当前会话的归属；名称与侧栏分组命名规则保持一致。
function syncHdrWorkspace(id) {
  const elWs = $('hdr-ws')
  const sess = app.sessionList.find((x) => x.id === id)
  if (!sess) {
    elWs.hidden = true
    return
  }
  const wsId = sess.workspace_id || ''
  const ws = app.workspaces.find((w) => w.id === wsId)
  const name = ws ? ws.name || ws.root_path || '' : wsId ? '已删除的工作区' : '默认工作区'
  elWs.textContent = name + ' /'
  elWs.title = ((ws && ws.root_path) || name) + '（点击在侧栏定位）'
  elWs.hidden = false
}

// ---------- workspace 管理 ----------

async function loadWorkspaces() {
  try {
    const { workspaces } = await app.api.listWorkspaces()
    app.workspaces = workspaces || []
  } catch (e) {
    if (e.status !== 401) console.warn('list workspaces:', e)
  }
}

// 目录浏览弹窗（添加工作区）：从 $HOME 起逐级下钻，选择目录即注册。
const dirPicker = { path: '', parent: '' }

function openDirPicker() {
  $('dir-modal').hidden = false
  browseDir('')
}

// 面包屑：把当前路径拆成可点段，根段在 $HOME 内显示为 ~，否则为 /。
// 点击任意段直接跳转到该上级目录。
function renderDirCrumbs(path, home) {
  const nav = $('dir-path')
  nav.textContent = ''
  const inHome = !!home && (path === home || path.startsWith(home + '/'))
  const rootLabel = inHome ? '~' : '/'
  const rootPath = inHome ? home : '/'
  const rel = inHome ? path.slice(home.length) : path.slice(1)
  const parts = rel.split('/').filter(Boolean)

  addCrumb(nav, rootLabel, rootPath, parts.length === 0)
  let acc = rootPath
  parts.forEach((p, i) => {
    acc = (acc === '/' ? '' : acc) + '/' + p
    addSep(nav)
    addCrumb(nav, p, acc, i === parts.length - 1)
  })
  // 让最深一级（当前目录）滚入可视区。
  nav.scrollLeft = nav.scrollWidth
}

function addCrumb(nav, label, path, isCurrent) {
  const b = document.createElement('button')
  b.type = 'button'
  b.className = 'dir-crumb' + (isCurrent ? ' is-current' : '')
  b.textContent = label
  if (!isCurrent) b.onclick = () => browseDir(path)
  nav.appendChild(b)
}

function addSep(nav) {
  const s = document.createElement('span')
  s.className = 'dir-sep'
  s.textContent = '/'
  nav.appendChild(s)
}

async function browseDir(path) {
  try {
    const r = await app.api.browseDirectories(path)
    dirPicker.path = r.path
    dirPicker.parent = r.parent || ''
    renderDirCrumbs(r.path, r.home)
    $('dir-up').disabled = !r.parent
    const list = $('dir-list')
    list.textContent = ''
    if (!r.entries || r.entries.length === 0) {
      const empty = document.createElement('div')
      empty.className = 'dir-empty'
      empty.textContent = '（无子目录）'
      list.appendChild(empty)
      return
    }
    for (const e of r.entries) {
      const item = document.createElement('button')
      item.className = 'dir-item'
      item.type = 'button'
      item.textContent = e.name
      item.onclick = () => browseDir(e.path)
      list.appendChild(item)
    }
  } catch (e) {
    if (e.status !== 401) toast('浏览目录失败: ' + e.message)
  }
}

async function confirmDirPicker() {
  const rootPath = dirPicker.path
  if (!rootPath) return
  try {
    const { workspace } = await app.api.registerWorkspace(rootPath, '')
    $('dir-modal').hidden = true
    toast('已添加工作区 ' + (workspace.name || rootPath), true)
    await loadWorkspaces()
    await refreshSessions()
    // 添加首个工作区后从引导态恢复到落地页（仅当没有打开的会话）
    if (app.workspaces.length === 1 && !app.sessionId) showLandingState()
  } catch (e) {
    if (e.status !== 401) toast('添加工作区失败: ' + e.message)
  }
}

async function newSession(workspaceId) {
  const { session_id } = await app.api.createSession(workspaceId || '')
  await refreshSessions()
  await openSession(session_id)
}

// 删除工作区（侧栏工作区节点操作）：级联删除其下全部会话（存活会话会被
// 关闭，历史会话一并删除，不可恢复）；磁盘目录不动。
async function deleteWorkspace(wsId) {
  const ws = app.workspaces.find((w) => w.id === wsId)
  const name = (ws && (ws.name || ws.root_path)) || wsId
  const count = (ws && ws.session_count) || 0
  const body =
    count > 0
      ? `「${name}」将被删除，其下 ${count} 个会话将一并永久删除（不可恢复）。磁盘目录不受影响。`
      : `「${name}」将从工作区列表移除（无会话）。磁盘目录不受影响。`
  const ok = await confirmDialog({
    title: '删除工作区',
    body,
    okLabel: '删除',
  })
  if (!ok) return
  try {
    await app.api.deleteWorkspace(wsId)
    toast('工作区已删除', true)
  } catch (e) {
    if (e.status !== 401) toast('删除工作区失败: ' + e.message)
    return
  }
  // 当前打开的会话如果属于被删工作区，断开流并清空 transcript
  if (app.sessionId) {
    const sess = app.sessionList.find((s) => s.id === app.sessionId)
    if (sess && sess.workspace_id === wsId) {
      app.stream.detach()
      app.sessionId = null
      app.transcript.clear()
      app.ctxgauge.reset()
      renderPlanInto($('plan-panel'), null)
      $('hdr-session').hidden = true
      $('hdr-share').hidden = true
      $('hdr-ws').hidden = true
      setSessionState('closed')
    }
  }
  await loadWorkspaces()
  await refreshSessions()
  // 删除后可能进入零工作区引导态；否则回落地页（仅当没有打开的会话）
  if (syncWorkspaceGate()) return
  if (!app.sessionId) showLandingState()
}

// ---------- model / reasoning 切换 ----------

async function pickModel(ref) {
  if (!app.sessionId) {
    toast('先创建或选择一个会话')
    return
  }
  try {
    const r = await app.api.setModel(app.sessionId, ref)
    // 直接采用 picker 的 ref：它与列表项的 currentRef 比较同源，必然匹配。
    // （SetModelResult 无 JSON tag，响应键是大写的 Cur/Meta，拼读易错。）
    app.curModelRef = ref
    syncPickerLabels()
    const meta = r.Meta || r.meta || {}
    if (meta.context_window || meta.ContextWindow) {
      // 模型切换后窗口阈值变化：按新名义窗口重推导，等下一次
      // context.usage / snapshot 刷新精确值
      app.ctxgauge.setWindow(null, meta.context_window || meta.ContextWindow)
    }
    toast('模型已切换为 ' + modelLabel(app.curModelRef), true)
  } catch (e) {
    if (e.status !== 401) toast('切换模型失败: ' + e.message)
  }
}

async function pickReasoning(effort) {
  if (!app.sessionId) {
    toast('先创建或选择一个会话')
    return
  }
  try {
    const r = await app.api.setReasoning(app.sessionId, effort)
    // 同上：SetReasoningResult 响应键是大写 Effective/Overridden；effort
    // 来自 picker 固定选项集，直接采用。
    app.curReasoning = effort
    app.reasoningOverridden = r.Overridden ?? r.overridden ?? effort !== 'default'
    syncPickerLabels()
    toast('reasoning: ' + (effort === 'default' ? '默认' : effort), true)
  } catch (e) {
    if (e.status !== 401) toast('设置 reasoning 失败: ' + e.message)
  }
}

// ---------- events ----------

// SSE 事件分批渲染：重连追帧时事件成突发到达（几百上千条），逐事件同步
// 处理会把主线程连成多秒长任务（布局/绘制全被打断，肉眼就是界面闪烁+
// 卡死）。排队 + rAF 合帧、每帧最多 EVENTS_PER_FRAME 条：帧间让出主线程，
// 追帧进度可见且界面保持响应。实时流的额外延迟 ≤ 一帧，无感知。
const EVENTS_PER_FRAME = 120
const eventQueue = []
let eventFlushScheduled = false

function onRuntimeEvent(evt) {
  eventQueue.push(evt)
  if (eventFlushScheduled) return
  eventFlushScheduled = true
  requestAnimationFrame(flushEventQueue)
}

function flushEventQueue() {
  const batch = eventQueue.splice(0, EVENTS_PER_FRAME)
  for (const evt of batch) applyRuntimeEvent(evt)
  if (eventQueue.length > 0) {
    requestAnimationFrame(flushEventQueue)
  } else {
    eventFlushScheduled = false
  }
}

function applyRuntimeEvent(evt) {
  app.transcript.handleEvent(evt)
  switch (evt.kind) {
    case 'turn.started':
      setSessionState('running')
      break
    case 'turn.finished':
      setSessionState('idle')
      if (evt.payload?.usage) app.statusbar.setUsage(evt.payload.usage)
      refreshSessions()
      break
    case 'approval.requested':
      setSessionState('awaiting_approval')
      // 侧栏状态灯与工作区待审批徽标的数据源是会话列表：立即刷新，
      // 不等 5s 轮询，否则徽标的出现/消失明显滞后于卡片的弹出/收起。
      refreshSessions()
      break
    case 'approval.resolved':
      setSessionState('running')
      refreshSessions()
      break
    case 'run.cancel_requested':
      setSessionState('cancelling')
      break
    case 'run.cancelled':
    case 'runtime.fatal':
      setSessionState('idle')
      break
    case 'usage.updated':
      app.statusbar.setUsage(evt.payload)
      break
    case 'context.usage':
      // 实时 context 占用：驱动 composer 旁的占用环
      app.ctxgauge.onContextUsage(evt.payload)
      break
    case 'context.compacted':
      // 压缩后占用立即回落（transcript 明细卡片由 transcript.handleEvent 渲染）
      app.ctxgauge.onCompacted(evt.payload)
      break
    case 'plan.updated':
      renderPlanInto($('plan-panel'), evt.payload)
      break
    case 'model.changed':
      // 会话内 /model 切换（其他客户端触发）同步到 UI
      if (evt.payload?.cur) {
        app.curModelRef = (evt.payload.cur.provider || '') + '/' + (evt.payload.cur.model || '')
        syncPickerLabels()
      }
      break
    case 'reasoning.changed':
      if (evt.payload?.effective) {
        app.curReasoning = evt.payload.effective.effort || ''
        app.reasoningOverridden = !!evt.payload.overridden
        syncPickerLabels()
      }
      break
    default:
      break
  }
}

async function resync(reason) {
  console.info('resync:', reason)
  app.stream.detach()
  if (!app.sessionId) return
  try {
    await openSession(app.sessionId)
  } catch (e) {
    if (e.status !== 401) toast('resync failed: ' + e.message)
  }
}

// ---------- boot ----------

async function boot() {
  hydrateIcons() // index.html 中 data-icon 占位的静态按钮
  initTheme()
  initSidebarToggle()
  initTooltips() // 全局接管 title 属性 → 主题化悬浮提示

  app.api = createApi({ getToken: () => app.token, onUnauthorized })

  app.transcript = new Transcript($('transcript'), $('blocks'), {
    resolveApproval: (payload, { decision, always }) =>
      app.api.resolveApproval(app.sessionId, payload.approval_id, {
        callId: payload.call_id,
        argsHash: payload.args_hash,
        decision,
        ruleHint: always
          ? { tool_name: payload.tool_name, arguments: payload.arguments }
          : undefined,
      }),
    answerQuestion: (questionId, answer) =>
      app.api.answerQuestion(app.sessionId, questionId, answer),
    fetchToolOutput,
    fetchArtifactURL,
    // 反馈投票：成功才写 localStorage（块内选中态由 transcript 维护）；
    // tracing 未开启 / run 无 trace 时后端返回错误码，抛回让块内回滚。
    sendFeedback: async (runId, value) => {
      await app.api.submitFeedback(app.sessionId, runId, value)
      try {
        localStorage.setItem(fbKey(app.sessionId, runId), value === 1 ? 'up' : 'down')
      } catch {
        /* 隐私模式等：忽略 */
      }
    },
    getFeedback: (runId) => {
      try {
        return localStorage.getItem(fbKey(app.sessionId, runId)) || ''
      } catch {
        return ''
      }
    },
    onError: (e) => toast(e.message),
  })
  app.transcript.setFollowButton($('follow-btn'))

  // 会话列表瀑布流：滚动接近底部时加载下一页
  $('session-list').addEventListener('scroll', () => {
    const list = $('session-list')
    if (list.scrollHeight - list.scrollTop - list.clientHeight < 120) loadMoreSessions()
  })

  app.sidebar = new Sidebar($('session-list'), {
    onSelect: (id) => {
      if (id === app.sessionId) return
      collapseSidebarIfNarrow()
      openSession(id).catch((e) => {
        if (e.status !== 401) toast('open session: ' + e.message)
      })
    },
    onAction: (id, action) => {
      onSessionAction(id, action)
    },
    onNewSession: (wsId) => {
      newSession(wsId).catch((e) => {
        if (e.status !== 401) toast('new session: ' + e.message)
      })
    },
    onDeleteWorkspace: (wsId) => {
      deleteWorkspace(wsId)
    },
  })
  // 归档视图切换：重置分页状态后整列重拉
  $('toggle-archived').onclick = () => {
    app.showArchived = !app.showArchived
    app.sidebar.archivedView = app.showArchived
    app.sessionList = []
    app.sessCursor = ''
    const btn = $('toggle-archived')
    btn.innerHTML = app.showArchived ? icon('arrow-left') + ' 返回' : '归档'
    btn.title = app.showArchived ? '返回会话列表' : '查看归档会话'
    btn.classList.toggle('is-active', app.showArchived)
    refreshSessions()
  }
  // 添加工作区（目录浏览弹窗）
  $('ws-add').onclick = () => {
    openDirPicker()
  }
  $('empty-add-ws').onclick = () => {
    openDirPicker()
  }
  // 零工作区引导态的添加按钮
  $('no-ws-add').onclick = () => {
    openDirPicker()
  }
  $('dir-up').onclick = () => {
    if (dirPicker.parent) browseDir(dirPicker.parent)
  }
  $('dir-cancel').onclick = () => {
    $('dir-modal').hidden = true
  }
  $('dir-select').onclick = () => {
    confirmDirPicker()
  }
  $('dir-modal').onclick = (e) => {
    if (e.target === $('dir-modal')) $('dir-modal').hidden = true
  }

  app.composer = new Composer({
    textarea: $('composer-input'),
    sendBtn: $('send-btn'),
    cancelBtn: $('cancel-btn'),
    attachBtn: $('attach-btn'),
    fileInput: $('attach-input'),
    stripEl: $('attach-strip'),
    boxEl: $('composer-box'),
    onSubmit: submitPrompt,
    onCancel: () => {
      if (!app.sessionId) return
      app.api.cancelTurn(app.sessionId).catch((e) => {
        if (e.status !== 401) toast('cancel: ' + e.message)
      })
    },
    onError: (m) => toast(m),
  })

  app.statusbar = new Statusbar({
    usageEl: $('sb-usage'),
    turnEl: $('sb-turn'),
    versionEl: $('sb-version'),
  })
  app.ctxgauge = new CtxGauge($('ctx-gauge'))

  // 设置面板（config.yaml 图形化编辑，保存即热更新，响应带分级生效报告）
  // onSaved：保存成功后刷新模型目录（picker 角标 / 附件门控依赖目录）
  app.settings = new SettingsPanel({
    api: app.api,
    toast,
    confirm: confirmDialog,
    onSaved: refreshModelCatalog,
  })
  $('hdr-settings').onclick = () => app.settings.open()

  // 模型 / reasoning 切换器
  app.picker = new Picker($('menu'))
  $('model-btn').onclick = () => {
    if (app.picker.current === 'model') {
      app.picker.close()
      return
    }
    app.picker.openModels($('model-btn'), {
      models: app.models,
      defaultRef: app.defaultModelRef,
      currentRef: app.curModelRef,
      onPick: pickModel,
    })
  }
  $('reasoning-btn').onclick = () => {
    if (app.picker.current === 'reasoning') {
      app.picker.close()
      return
    }
    app.picker.openReasoning($('reasoning-btn'), {
      current: app.curReasoning,
      overridden: app.reasoningOverridden,
      onPick: pickReasoning,
    })
  }

  app.stream = new EventStream({
    getToken: () => app.token,
    onEvent: onRuntimeEvent,
    onResync: resync,
    onDraining: () => {
      setConn('draining')
      const b = $('banner')
      b.textContent = ''
      const msg = document.createElement('span')
      msg.innerHTML = icon('triangle-exclamation')
      msg.appendChild(document.createTextNode(' 服务重启中，恢复后将自动重连…'))
      const retry = document.createElement('button')
      retry.className = 'banner-retry'
      retry.type = 'button'
      retry.textContent = '立即重试'
      retry.onclick = () => resync('manual_retry')
      b.appendChild(msg)
      b.appendChild(retry)
      b.hidden = false
      startDrainRecovery()
    },
    onConn: (state, detail, attempt) => {
      setConn(state, detail)
      connHealth(state, detail, attempt)
    },
    onAuthError: onUnauthorized,
  })

  // 页面恢复可见 / 从 BFCache 还原 / 网络恢复时主动探活重连：看门狗定时器
  // 随页面一起被系统挂起（App Nap / 窗口遮挡），不主动戳永远不会发现断连。
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) app.stream.ensureLive()
  })
  window.addEventListener('pageshow', () => app.stream.ensureLive())
  // 窗口重获焦点时同时刷新会话列表：其他会话的审批/运行状态可能已在
  // 后台变化，轮询间隔内靠这次主动刷新补齐。
  window.addEventListener('focus', () => {
    app.stream.ensureLive()
    refreshSessions()
  })
  window.addEventListener('online', () => app.stream.ensureLive())

  // 面包屑点击：展开侧栏（若折叠）并定位当前会话所属的工作区组
  $('hdr-ws').onclick = () => {
    if (!app.sessionId) return
    const sess = app.sessionList.find((x) => x.id === app.sessionId)
    if (!sess) return
    $('app').classList.remove('sidebar-collapsed')
    app.sidebar.revealWorkspace(sess.workspace_id || '')
  }

  $('hdr-session').onclick = async () => {
    if (!app.sessionId) return
    if (await copyText(app.sessionId)) toast('session id copied', true)
    else toast('剪贴板不可用，session id: ' + app.sessionId)
  }

  // 分享会话：点击复制公开只读链接（创建幂等，重复分享返回同一链接）；
  // Shift+点击撤销分享，原链接立即失效。
  $('hdr-share').onclick = async (e) => {
    if (!app.sessionId) return
    try {
      if (e.shiftKey) {
        const ok = await confirmDialog({
          title: '撤销分享',
          body: '撤销后，已发出的分享链接将立即失效（再次分享会生成新链接）。',
          okLabel: '撤销分享',
        })
        if (!ok) return
        await app.api.revokeShare(app.sessionId)
        toast('分享已撤销', true)
        return
      }
      // 桌面端：分享监听未开启时就地确认并开启——开启动作写穿到
      // share.enabled 并热应用（即时生效且持久）；loom serve 无开关
      // （404），直接用当前 origin 拼接。
      try {
        const endpoint = await app.api.getShareEndpoint()
        if (!endpoint.enabled) {
          const ok = await confirmDialog({
            title: '开启局域网分享',
            body: '分享链接需要一个局域网可达的监听。开启后，同一网络内持有链接的人可只读查看本会话（可随时在设置 → 系统 → 局域网分享关闭）。',
            okLabel: '开启并复制链接',
          })
          if (!ok) return
          const resp = await app.api.setShareEndpoint(true)
          if (resp?.endpoint?.error) {
            toast('分享监听启动失败: ' + resp.endpoint.error)
            return
          }
        }
      } catch (err) {
        if (err.status !== 404) throw err
      }
      // 分享监听在线时服务端返回绝对 url（docs/DESKTOP_DESIGN.md §5.2）；
      // 缺省退回按当前 origin 拼接。
      const { path, url: absoluteUrl } = await app.api.shareSession(app.sessionId)
      const url = absoluteUrl || location.origin + path
      if (await copyText(url)) {
        toast('分享链接已复制：任何持有链接的人可只读查看本会话', true)
      } else {
        // 剪贴板不可用（非安全上下文）：打开分享页，从地址栏复制
        window.open(url, '_blank', 'noopener')
        toast('剪贴板不可用，已在新标签页打开分享页（可从地址栏复制链接）')
      }
    } catch (err) {
      if (err.status !== 401) toast('分享失败: ' + err.message)
    }
  }

  // 侧栏轮询（页面可见时，5s）
  setInterval(() => {
    if (document.visibilityState === 'visible' && !$('app').hidden) refreshSessions()
  }, 5000)

  if (!app.token) {
    showGate()
  } else {
    await enter()
  }
}

// 零工作区引导态：无任何工作区时隐藏对话区和侧栏列表，只展示一个
// 居中的“添加工作区”卡片。用户删除全部工作区后也会进入此态。
function showNoWorkspace() {
  $('no-workspace').hidden = false
  $('transcript').style.display = 'none'
  document.querySelector('.composer').style.display = 'none'
  $('empty-state').hidden = true
  $('hdr-session').hidden = true
  $('hdr-share').hidden = true
  $('hdr-ws').hidden = true
  // 侧栏会话列表区在无工作区时无内容可渲染，只留工作区添加入口
  $('session-list').style.display = 'none'
}

function hideNoWorkspace() {
  $('no-workspace').hidden = true
  $('transcript').style.display = ''
  document.querySelector('.composer').style.display = ''
  $('session-list').style.display = ''
}

// 工作区状态守卫：在加载/删除工作区后检查，零工作区时切换到引导态，
// 有工作区时恢复并展示落地页。
function syncWorkspaceGate() {
  if (app.workspaces.length === 0) {
    showNoWorkspace()
    return true
  }
  hideNoWorkspace()
  return false
}

// 首入落地页：不自动打开任何会话（桌面端启动应是廉价且可预期的）。
// 零会话时给出首次引导（添加工作区 CTA）；有会话时提示从侧栏选择。
function showLandingState() {
  if (syncWorkspaceGate()) return
  const hasSessions = app.sessionList.length > 0
  $('empty-hint').textContent = hasSessions
    ? 'Pick a session from the sidebar, or just start typing.'
    : 'No sessions yet — add a workspace, or just start typing.'
  $('empty-add-ws').hidden = hasSessions
  $('empty-state').hidden = false
  $('hdr-session').hidden = true
  $('hdr-share').hidden = true
  $('hdr-ws').hidden = true
  app.curModelRef = app.defaultModelRef
  syncPickerLabels()
}

// 最近活跃工作区：会话列表按更新时间排序（newest first），取第一项的归属。
// 新建会话（直接打字/回车）的默认落点，避免落到进程默认工作区（桌面端
// 通常是 $HOME）。
function recentWorkspaceId() {
  const s = app.sessionList.find((s) => s.workspace_id)
  return s ? s.workspace_id : ''
}

async function enter() {
  try {
    const meta = await app.api.metaVersion()
    app.statusbar.setVersion(meta.version)
    // 加载模型目录（picker 数据源；modalities 随目录下发，刷新附件入口）
    await refreshModelCatalog()
    showApp()
    await loadWorkspaces()
    await refreshSessions()
    // 首入落地页：不自动打开会话（自动恢复要拉起整个 controller 运行时，
    // 且多工作区场景下「最新会话」大概率不是用户当下想做的）；侧栏定位到
    // 最近活跃工作区，由用户点选继续。
    showLandingState()
    app.sidebar.focusWorkspace(recentWorkspaceId())
  } catch (e) {
    if (e.status !== 401) showGate('connect failed: ' + e.message)
  }
}

async function submitPrompt(text, images = [], followup = false) {
  if (app.readOnly) {
    toast('子 agent 会话为只读，不能追问')
    return
  }
  // followup 仅文本：图片随普通 prompt 发送（后端同样拒绝 followup+图片）
  if (followup && images.length) {
    toast('排队到下一轮的消息仅支持文本，图片已忽略')
    images = []
  }
  // 幂等键：同一「文本 + 图片集合 + 投递方式」重发共享同键（双击/网络重试不产生重复 turn）
  const fp = (followup ? 'F:' : '') + text + '#' + images.map((i) => i.data.length).join('+')
  let key
  if (app.lastSubmit && app.lastSubmit.fp === fp) {
    key = app.lastSubmit.key
  } else {
    key = randomId()
  }
  app.lastSubmit = { fp, key }
  try {
    if (!app.sessionId) await newSession(recentWorkspaceId())
    await app.api.submitPrompt(app.sessionId, text, key, images, followup)
    app.composer.clearDraft()
    app.lastSubmit = null
    refreshSessions()
  } catch (e) {
    if (e.status === 401) return
    // 失败时仅还原文本，附件留在 composer 以便重试
    app.composer.restoreDraft(text)
    toast('send failed: ' + e.message)
  }
}

$('gate-form').addEventListener('submit', (e) => {
  e.preventDefault()
  const token = $('gate-token').value.trim()
  if (!token) return
  app.token = token
  sessionStorage.setItem(TOKEN_KEY, token)
  enter()
})

boot()
