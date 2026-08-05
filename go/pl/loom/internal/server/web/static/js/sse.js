// sse.js — fetch-based SSE 连接管理（docs/WEB_DESIGN.md §2.2/§3.4）。
// EventSource 无法设置 Authorization header，故用 fetch + ReadableStream
// 手写解析；与 Go 侧 client/http.go 的 pumpSSE 同构。

const WATCHDOG_MS = 45_000;       // 服务端心跳 15s；45s 无帧判死
const WATCHDOG_TICK_MS = 5_000;
const BACKOFF_MIN_MS = 1_000;
const BACKOFF_MAX_MS = 15_000;

export class EventStream {
  // callbacks: onEvent(evt), onResync(reason), onDraining(),
  //            onConn(state, detail), onAuthError()
  constructor({ getToken, onEvent, onResync, onDraining, onConn, onAuthError }) {
    this.getToken = getToken;
    this.onEvent = onEvent;
    this.onResync = onResync;
    this.onDraining = onDraining;
    this.onConn = onConn;
    this.onAuthError = onAuthError;

    this.sessionId = null;
    this.lastSeq = 0;
    this.instance = "";
    this.abort = null;
    this.retries = 0;
    this.stopped = true;
    this.drained = false;
    this.lastFrameAt = 0;
    this.watchdog = null;
    this.retryTimer = null;
  }

  // attach opens (or re-points) the stream for a session.
  attach(sessionId, afterSeq) {
    this.detach();
    this.sessionId = sessionId;
    this.lastSeq = afterSeq || 0;
    this.stopped = false;
    this.drained = false;
    this.instance = "";
    this._connect();
  }

  detach() {
    this.stopped = true;
    if (this.abort) { this.abort.abort(); this.abort = null; }
    if (this.retryTimer) { clearTimeout(this.retryTimer); this.retryTimer = null; }
    this._stopWatchdog();
  }

  async _connect() {
    if (this.stopped) return;
    this.onConn(this.retries === 0 ? "connecting" : "reconnecting");
    const url = `/v1/sessions/${this.sessionId}/events?after=${this.lastSeq}`;
    this.abort = new AbortController();
    let res;
    try {
      res = await fetch(url, {
        headers: { Authorization: "Bearer " + this.getToken() },
        signal: this.abort.signal,
      });
    } catch (e) {
      if (this.stopped || e.name === "AbortError") return;
      return this._scheduleRetry();
    }
    if (res.status === 401) { this.onAuthError(); return; }
    if (res.status === 429) {
      this.onConn("dead", "too many tabs streaming this session");
      return; // 不重连：用户在别处开了太多流
    }
    if (!res.ok) {
      return this._scheduleRetry();
    }
    this.lastFrameAt = Date.now();
    this._startWatchdog();
    try {
      await this._parse(res.body);
    } catch (e) {
      if (this.stopped || e.name === "AbortError") return;
    }
    this._stopWatchdog();
    if (this.stopped || this.drained) return;
    this._scheduleRetry();
  }

  _scheduleRetry() {
    if (this.stopped || this.drained) return;
    const base = Math.min(BACKOFF_MAX_MS, BACKOFF_MIN_MS * 2 ** this.retries);
    const jitter = base * (0.75 + Math.random() * 0.5);
    this.retries++;
    this.onConn("reconnecting", `retry in ${Math.round(jitter / 1000)}s`);
    this.retryTimer = setTimeout(() => this._connect(), jitter);
  }

  _startWatchdog() {
    this._stopWatchdog();
    this.watchdog = setInterval(() => {
      if (Date.now() - this.lastFrameAt > WATCHDOG_MS && this.abort) {
        this.abort.abort(); // 判死：触发重连路径
      }
    }, WATCHDOG_TICK_MS);
  }

  _stopWatchdog() {
    if (this.watchdog) { clearInterval(this.watchdog); this.watchdog = null; }
  }

  async _parse(body) {
    const reader = body.getReader();
    const decoder = new TextDecoder();
    let buf = "";
    let id = "", event = "", data = "";
    const dispatch = () => {
      const d = data; const ev = event; const i = id;
      id = ""; event = ""; data = "";
      if (!d) return;
      if (ev === "server.resync") { this.onResync("server.resync"); return; }
      if (ev === "server.draining") {
        this.drained = true;
        this.onDraining();
        return;
      }
      let evt;
      try { evt = JSON.parse(d); } catch { return; }
      if (i) {
        const seq = parseInt(i, 10);
        if (!isNaN(seq) && seq > this.lastSeq) this.lastSeq = seq;
      }
      if (evt.sequence && evt.sequence > this.lastSeq) this.lastSeq = evt.sequence;
      this.onEvent(evt);
    };
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      this.lastFrameAt = Date.now();
      buf += decoder.decode(value, { stream: true });
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).replace(/\r$/, "");
        buf = buf.slice(nl + 1);
        if (line === "") { dispatch(); continue; }
        if (line.startsWith(":")) {
          // 连接横幅 / 心跳注释帧
          const m = line.match(/^: connected, instance=(\w+)/);
          if (m) {
            if (this.instance && this.instance !== m[1]) {
              this.onResync("instance_changed");
              return;
            }
            this.instance = m[1];
            this.retries = 0;
            this.onConn("live");
          }
          continue;
        }
        if (line.startsWith("id: ")) id = line.slice(4);
        else if (line.startsWith("event: ")) event = line.slice(7);
        else if (line.startsWith("data: ")) data = data ? data + "\n" + line.slice(6) : line.slice(6);
      }
    }
    if (data) dispatch();
  }
}
