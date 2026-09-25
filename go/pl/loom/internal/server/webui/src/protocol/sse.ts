// sse.ts — event-stream connection management (docs/WEB_DESIGN.md §2.2/§3.4).
//
// Dual transport: WebSocket first, fetch-SSE fallback. Every fetch-SSE
// stream pins one HTTP/1.1 connection for its lifetime and browser stacks
// cap at ~6 connections per host — a handful of live session streams
// starve the pool, after which plain API calls (prompt submit included)
// queue client-side until they time out without ever reaching the server
// (the 2026-09 desktop "request timed out" incident). WebSocket
// connections do not count against that pool, so the stream upgrades in
// place: same URL, same frame format (one SSE frame per WS text message,
// parsed by the exact same line/dispatch logic). The fetch path stays as
// the fallback for WS-unfriendly environments (CSP, proxies, old WebKit)
// and keeps its precise status handling (401/404/429) for failures the
// WS handshake cannot surface to a browser. EventSource itself is
// unusable in both roles because it cannot carry the Authorization header;
// the WS handshake rides the token as a query parameter instead
// (docs/SERVE_DESIGN.md §5.2).

import type { RuntimeEvent } from './events'

const WATCHDOG_MS = 45_000 // server heartbeat is 15s; 45s without a frame is judged dead
const WATCHDOG_TICK_MS = 5_000
const BACKOFF_MIN_MS = 1_000
const BACKOFF_MAX_MS = 15_000
const RATE_LIMIT_RETRY_MS = 30_000 // 429 stream limit exceeded: slow retry (self-heals once the peer tab closes)

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
  private ws: WebSocket | null = null
  // Stays true until a WebSocket fails before opening once (CSP block,
  // WS-blind proxy, ancient WebKit); from then on this stream goes straight
  // to the SSE transport instead of paying a doomed handshake per reconnect.
  private wsUsable = true
  private retries = 0
  private stopped = true
  private drained = false
  private lastFrameAt = 0
  private watchdog: ReturnType<typeof setInterval> | null = null
  private retryTimer: ReturnType<typeof setTimeout> | null = null
  private gen = 0 // connection generation: invalidates old connections' async continuations (prevents double connections / killing the new watchdog)
  // Frame parser state, reset per connection; shared by both transports.
  private buf = ''
  private frameId = ''
  private frameEvent = ''
  private frameData = ''

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
    this._teardownConnection()
    if (this.retryTimer) {
      clearTimeout(this.retryTimer)
      this.retryTimer = null
    }
    this._stopWatchdog()
  }

  // ensureLive is called on page lifecycle events (visibilitychange/pageshow/
  // online/focus): the watchdog's setInterval is suspended with the page by the
  // system (App Nap / window occlusion / BFCache); while frozen TCP goes half-open
  // and the connection dies silently, and after resume no timer ever notices — a
  // page event must actively poke it. No-op when connected and frames are fresh.
  // Note: only triggers reconnect early, does NOT reset retries — frequent page wakes on
  // weak networks must not zero the backoff progress into a reconnect storm against the server (a successful connection frame self-resets).
  ensureLive() {
    if (this.stopped || this.drained || !this.sessionId) return
    // Waiting in backoff (incl. 429 slow retry): skip the wait, reconnect now.
    if (this.retryTimer) {
      clearTimeout(this.retryTimer)
      this.retryTimer = null
      void this._connect()
      return
    }
    // Connection looks in-flight but exceeded the heartbeat deadline: judge dead, kill, reconnect now.
    const stale = this.lastFrameAt > 0 && Date.now() - this.lastFrameAt > WATCHDOG_MS
    if (stale || (!this.abort && !this.ws)) {
      this._teardownConnection()
      void this._connect()
    }
  }

  private async _connect(): Promise<void> {
    if (this.stopped) return
    const gen = ++this.gen
    this.onConn(this.retries === 0 ? 'connecting' : 'reconnecting')
    this._resetParser()
    if (this.wsUsable) {
      const streamed = await this._connectWS(gen)
      if (this.stopped || this.gen !== gen) return
      if (streamed) {
        // The WebSocket ran and ended (server close, drop, kill): same
        // epilogue as a finished SSE stream.
        this._stopWatchdog()
        if (this.drained) return
        return this._scheduleRetry()
      }
      // The WebSocket never opened: the failure carries no status a
      // browser can read, so fall through to the SSE transport — it both
      // works in WS-hostile environments and surfaces the precise status
      // (401/404/429) when the server is the one refusing.
      this.wsUsable = false
      this._teardownConnection()
    }
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
      // Aborts caused by detach (stopped) or by a newer connection started by
      // ensureLive/attach (gen changed) are dropped; the rest (network failure) go into backoff retry.
      if (this.stopped || this.gen !== gen) return
      return this._scheduleRetry()
    }
    if (this.stopped || this.gen !== gen) return
    if (res.status === 401) {
      this.onAuthError()
      return
    }
    if (res.status === 404) {
      // Session not live in this process (after a server restart sessions must be
      // resumed again): the events endpoint has no frames to send (not even server.resync),
      // so plain-failure backoff is wrong — go straight to resync: snapshot 404 → resume
      // → re-attach the stream. A deleted session ends with a resync error; no infinite loop.
      this.onResync('session_not_live')
      return
    }
    if (res.status === 429) {
      // Stream limit exceeded: raise dead for the UI to prompt (with manual retry),
      // and also arm a slow auto-retry — self-heals with no user intervention once tabs elsewhere close.
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
      // The watchdog's kill AbortError also lands here: must not return silently,
      // otherwise a dropped stream never reconnects (historical bug: badge stuck at
      // live, UI frozen). Deliberate aborts by detach/ensureLive are already intercepted by the gen/stopped guards.
      if (this.stopped || this.gen !== gen) return
    }
    this._stopWatchdog()
    if (this.stopped || this.drained || this.gen !== gen) return
    this._scheduleRetry()
  }

  // _connectWS runs one WebSocket connection to completion. Resolves true
  // when the socket had opened (the stream ran and then ended — caller
  // retries); false when it never opened (caller falls back to SSE).
  private _connectWS(gen: number): Promise<boolean> {
    return new Promise((resolve) => {
      const sid = this.sessionId!
      const params = new URLSearchParams({ after: String(this.lastSeq), token: this.getToken() })
      const scheme = location.protocol === 'https:' ? 'wss' : 'ws'
      let ws: WebSocket
      try {
        ws = new WebSocket(
          `${scheme}://${location.host}/v1/sessions/${encodeURIComponent(sid)}/events?${params}`,
        )
      } catch {
        resolve(false)
        return
      }
      this.ws = ws
      let opened = false
      let receivedAny = false
      ws.onopen = () => {
        opened = true
        this.lastFrameAt = Date.now()
        this._startWatchdog()
      }
      ws.onmessage = (m) => {
        if (this.stopped || this.gen !== gen) return
        if (typeof m.data !== 'string') return
        receivedAny = true
        this.lastFrameAt = Date.now()
        this._feed(m.data)
      }
      ws.onerror = () => {
        // onclose always follows; every teardown decision lives there.
      }
      ws.onclose = () => {
        if (this.ws === ws) this.ws = null
        if (this.stopped || this.gen !== gen) {
          // Deliberate teardown (detach/ensureLive/newer connection): the
          // value is irrelevant, the caller's guards return before reading it.
          resolve(true)
          return
        }
        this._flushPending()
        // A socket that closed without delivering a single frame — whether
        // the handshake itself failed or it 101'd and died silently — falls
        // back to SSE, which surfaces the server's precise status (the
        // Swift client's receivedAnyFrame mirrors this).
        resolve(opened && receivedAny)
      }
    })
  }

  // _teardownConnection kills whichever transport is live (fetch abort or
  // WS close); the killed connection's own completion path drives the
  // reconnect, guarded by gen/stopped.
  private _teardownConnection() {
    if (this.abort) {
      this.abort.abort()
      this.abort = null
    }
    if (this.ws) {
      const ws = this.ws
      this.ws = null
      ws.close()
    }
  }

  private _scheduleRetry() {
    if (this.stopped || this.drained) return
    const base = Math.min(BACKOFF_MAX_MS, BACKOFF_MIN_MS * 2 ** this.retries)
    const jitter = base * (0.75 + Math.random() * 0.5)
    this.retries++
    // attempt lets the UI judge "consecutive failures": past the threshold, raise a banner alert.
    this.onConn('reconnecting', `retry in ${Math.round(jitter / 1000)}s`, this.retries)
    this.retryTimer = setTimeout(() => void this._connect(), jitter)
  }

  private _startWatchdog() {
    this._stopWatchdog()
    this.watchdog = setInterval(() => {
      if (Date.now() - this.lastFrameAt > WATCHDOG_MS) {
        this._teardownConnection() // judged dead: triggers the reconnect path
      }
    }, WATCHDOG_TICK_MS)
  }

  private _stopWatchdog() {
    if (this.watchdog) {
      clearInterval(this.watchdog)
      this.watchdog = null
    }
  }

  private _resetParser() {
    this.buf = ''
    this.frameId = ''
    this.frameEvent = ''
    this.frameData = ''
  }

  private _dispatchFrame() {
    const d = this.frameData
    const ev = this.frameEvent
    const i = this.frameId
    this.frameId = ''
    this.frameEvent = ''
    this.frameData = ''
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

  // _flushPending dispatches a trailing frame that never saw its blank-line
  // terminator (stream cut mid-frame); identical to the SSE reader's
  // end-of-body behavior.
  private _flushPending() {
    if (this.frameData) this._dispatchFrame()
    this.buf = ''
  }

  // _feed consumes one text chunk (a fetch body chunk or one WS message)
  // and dispatches every complete frame in it.
  private _feed(chunk: string) {
    this.buf += chunk
    let nl: number
    while ((nl = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, nl).replace(/\r$/, '')
      this.buf = this.buf.slice(nl + 1)
      if (line === '') {
        this._dispatchFrame()
        continue
      }
      if (line.startsWith(':')) {
        // connection banner / heartbeat comment frame
        const m = line.match(/^: connected, instance=(\w+)/)
        if (m) {
          if (this.instance && this.instance !== m[1]) {
            // An instance switch means the server changed processes (rolling deploy/restart):
            // the old connection's resources must be cleaned up by hand before resync —
            // tear the connection down, stop the watchdog, bump gen so the old
            // connection's tail won't schedule another retry (otherwise double
            // connection with resync's new attach).
            this._teardownConnection()
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
      if (line.startsWith('id: ')) this.frameId = line.slice(4)
      else if (line.startsWith('event: ')) this.frameEvent = line.slice(7)
      else if (line.startsWith('data: '))
        this.frameData = this.frameData ? this.frameData + '\n' + line.slice(6) : line.slice(6)
    }
  }

  private async _parse(body: ReadableStream<Uint8Array>) {
    const reader = body.getReader()
    const decoder = new TextDecoder()
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      this.lastFrameAt = Date.now()
      this._feed(decoder.decode(value, { stream: true }))
    }
    this._flushPending()
  }
}
