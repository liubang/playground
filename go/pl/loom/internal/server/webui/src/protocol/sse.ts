// sse.ts — fetch-based SSE 连接管理（docs/WEB_DESIGN.md §2.2/§3.4）。
// EventSource 无法设置 Authorization header，故用 fetch + ReadableStream
// 手写解析；与 Go 侧 client/http.go 的 pumpSSE 同构。
// 逻辑与旧 static/js/sse.js 一一对应，仅补类型。

import type { RuntimeEvent } from './events'

const WATCHDOG_MS = 45_000 // 服务端心跳 15s；45s 无帧判死
const WATCHDOG_TICK_MS = 5_000
const BACKOFF_MIN_MS = 1_000
const BACKOFF_MAX_MS = 15_000
const RATE_LIMIT_RETRY_MS = 30_000 // 429 流数超限：慢速重试（对端标签页关闭即自愈）

export type ConnState = 'connecting' | 'live' | 'reconnecting' | 'draining' | 'dead'

export interface EventStreamCallbacks {
  getToken: () => string
  onEvent: (evt: RuntimeEvent) => void
  onResync: (reason: string) => void
  onDraining: () => void
  onConn: (state: ConnState, detail?: string, attempt?: number) => void
  onAuthError: () => void
}

export class EventStream {
  private getToken: () => string
  private onEvent: (evt: RuntimeEvent) => void
  private onResync: (reason: string) => void
  private onDraining: () => void
  private onConn: (state: ConnState, detail?: string, attempt?: number) => void
  private onAuthError: () => void

  private sessionId: string | null = null
  private lastSeq = 0
  private instance = ''
  private abort: AbortController | null = null
  private retries = 0
  private stopped = true
  private drained = false
  private lastFrameAt = 0
  private watchdog: ReturnType<typeof setInterval> | null = null
  private retryTimer: ReturnType<typeof setTimeout> | null = null
  private gen = 0 // 连接代次：作废旧连接的异步后续（防双连接/误杀新看门狗）

  constructor(cb: EventStreamCallbacks) {
    this.getToken = cb.getToken
    this.onEvent = cb.onEvent
    this.onResync = cb.onResync
    this.onDraining = cb.onDraining
    this.onConn = cb.onConn
    this.onAuthError = cb.onAuthError
  }

  // attach opens (or re-points) the stream for a session.
  attach(sessionId: string, afterSeq?: number) {
    this.detach()
    this.sessionId = sessionId
    this.lastSeq = afterSeq || 0
    this.stopped = false
    this.drained = false
    this.instance = ''
    void this._connect()
  }

  detach() {
    this.stopped = true
    if (this.abort) {
      this.abort.abort()
      this.abort = null
    }
    if (this.retryTimer) {
      clearTimeout(this.retryTimer)
      this.retryTimer = null
    }
    this._stopWatchdog()
  }

  // ensureLive 在页面生命周期事件（visibilitychange/pageshow/online/focus）
  // 时调用：看门狗的 setInterval 会随页面一起被系统挂起（App Nap / 窗口
  // 遮挡 / BFCache），冻结期间 TCP 半开、连接悄死，恢复后没有任何定时器
  // 会发现它——必须由页面事件主动戳一下。连接在且帧新鲜时是 no-op。
  // 注意：这里只提前触发重连，不重置 retries——弱网下频繁唤起页面不应
  // 把退避进度清零变成对服务端的重连风暴（成功后连接帧会自行归零）。
  ensureLive() {
    if (this.stopped || this.drained || !this.sessionId) return
    // 退避等待中（含 429 慢速重试）：不等了，立即重连。
    if (this.retryTimer) {
      clearTimeout(this.retryTimer)
      this.retryTimer = null
      void this._connect()
      return
    }
    // 连接看似在途但已超心跳期限：判死杀掉，立即重连。
    const stale = this.lastFrameAt > 0 && Date.now() - this.lastFrameAt > WATCHDOG_MS
    if (stale || !this.abort) {
      if (this.abort) {
        this.abort.abort()
        this.abort = null
      }
      void this._connect()
    }
  }

  private async _connect(): Promise<void> {
    if (this.stopped) return
    const gen = ++this.gen
    this.onConn(this.retries === 0 ? 'connecting' : 'reconnecting')
    const sid = this.sessionId!
    const params = new URLSearchParams({ after: String(this.lastSeq) })
    const url = `/v1/sessions/${encodeURIComponent(sid)}/events?${params}`
    this.abort = new AbortController()
    let res: Response
    try {
      res = await fetch(url, {
        headers: { Authorization: 'Bearer ' + this.getToken() },
        signal: this.abort.signal,
      })
    } catch {
      // detach（stopped）或 ensureLive/attach 起的新连接（gen 变化）引发的
      // 中止直接丢弃；其余（网络失败）进入退避重连。
      if (this.stopped || this.gen !== gen) return
      return this._scheduleRetry()
    }
    if (this.stopped || this.gen !== gen) return
    if (res.status === 401) {
      this.onAuthError()
      return
    }
    if (res.status === 404) {
      // 会话在本进程不 live（服务重启后会话需重新 resume）：events 端点
      // 无帧可发（连 server.resync 都收不到），不能按普通失败退避——直接
      // 走 resync：snapshot 404 → resume → 重挂流。会话已删除时 resync
      // 内部报错收场，不会形成死循环。
      this.onResync('session_not_live')
      return
    }
    if (res.status === 429) {
      // 流数超限：升 dead 让 UI 提示（附手动重试），同时挂一个慢速自动
      // 重试——别处的标签页关闭后无需用户干预即可自愈。
      this.onConn('dead', 'too many tabs streaming this session')
      this.retryTimer = setTimeout(() => void this._connect(), RATE_LIMIT_RETRY_MS)
      return
    }
    if (!res.ok) {
      return this._scheduleRetry()
    }
    this.lastFrameAt = Date.now()
    this._startWatchdog()
    try {
      await this._parse(res.body!)
    } catch {
      // 看门狗判死的 AbortError 也走到这里：不能静默 return，否则断流后
      // 永无重连（历史 bug：徽标停在 live，界面僵死）。detach/ensureLive
      // 的主动中止已被 gen/stopped 守卫拦截，不会误入。
      if (this.stopped || this.gen !== gen) return
    }
    this._stopWatchdog()
    if (this.stopped || this.drained || this.gen !== gen) return
    this._scheduleRetry()
  }

  private _scheduleRetry() {
    if (this.stopped || this.drained) return
    const base = Math.min(BACKOFF_MAX_MS, BACKOFF_MIN_MS * 2 ** this.retries)
    const jitter = base * (0.75 + Math.random() * 0.5)
    this.retries++
    // attempt 给 UI 判断「连续失败」用：达到阈值后升起横幅强提示。
    this.onConn('reconnecting', `retry in ${Math.round(jitter / 1000)}s`, this.retries)
    this.retryTimer = setTimeout(() => void this._connect(), jitter)
  }

  private _startWatchdog() {
    this._stopWatchdog()
    this.watchdog = setInterval(() => {
      if (Date.now() - this.lastFrameAt > WATCHDOG_MS && this.abort) {
        this.abort.abort() // 判死：触发重连路径
      }
    }, WATCHDOG_TICK_MS)
  }

  private _stopWatchdog() {
    if (this.watchdog) {
      clearInterval(this.watchdog)
      this.watchdog = null
    }
  }

  private async _parse(body: ReadableStream<Uint8Array>) {
    const reader = body.getReader()
    const decoder = new TextDecoder()
    let buf = ''
    let id = ''
    let event = ''
    let data = ''
    const dispatch = (): void => {
      const d = data
      const ev = event
      const i = id
      id = ''
      event = ''
      data = ''
      if (!d) return
      if (ev === 'server.resync') {
        this.onResync('server.resync')
        return
      }
      if (ev === 'server.draining') {
        this.drained = true
        this.onDraining()
        return
      }
      let evt: RuntimeEvent
      try {
        evt = JSON.parse(d) as RuntimeEvent
      } catch {
        return
      }
      if (i) {
        const seq = parseInt(i, 10)
        if (!isNaN(seq) && seq > this.lastSeq) this.lastSeq = seq
      }
      if (evt.sequence && evt.sequence > this.lastSeq) this.lastSeq = evt.sequence
      this.onEvent(evt)
    }
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      this.lastFrameAt = Date.now()
      buf += decoder.decode(value, { stream: true })
      let nl: number
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl).replace(/\r$/, '')
        buf = buf.slice(nl + 1)
        if (line === '') {
          dispatch()
          continue
        }
        if (line.startsWith(':')) {
          // 连接横幅 / 心跳注释帧
          const m = line.match(/^: connected, instance=(\w+)/)
          if (m) {
            if (this.instance && this.instance !== m[1]) {
              // 实例切换意味着服务端换了进程（滚动部署/重启）：旧连接的
              // 资源必须在走 resync 前亲手清理——cancel reader 收掉底层流、
              // 停看门狗、bump gen 让 _connect 尾部不会给这条旧连接再排一次
              // 重连定时器（否则与 resync 牵出的新 attach 构成双连接）。
              void reader.cancel()
              this._stopWatchdog()
              this.gen++
              this.onResync('instance_changed')
              return
            }
            this.instance = m[1]
            this.retries = 0
            this.onConn('live')
          }
          continue
        }
        if (line.startsWith('id: ')) id = line.slice(4)
        else if (line.startsWith('event: ')) event = line.slice(7)
        else if (line.startsWith('data: '))
          data = data ? data + '\n' + line.slice(6) : line.slice(6)
      }
    }
    if (data) dispatch()
  }
}
