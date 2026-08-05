// main.js — 启动编排（docs/WEB_DESIGN.md §3.1）：
// token gate → meta 验活 → 模型目录 → 会话列表 → snapshot 首屏 → SSE 直播。
// composer 内嵌模型/reasoning 切换器。

import { createApi } from "./api.js";
import { EventStream } from "./sse.js";
import { Transcript } from "./components/transcript.js";
import { renderPlanInto } from "./components/blocks.js";
import { Sidebar } from "./components/sidebar.js";
import { Composer } from "./components/composer.js";
import { Statusbar } from "./components/statusbar.js";
import { Picker } from "./components/picker.js";
import { shortId } from "./format.js";

const TOKEN_KEY = "loom_token";
const THEME_KEY = "loom_theme";
const SIDEBAR_KEY = "loom_sidebar_collapsed";

const $ = (id) => document.getElementById(id);

// ---------- theme ----------
function initTheme() {
  // 默认深色（用户偏好）；仅当显式存了 "light" 才用浅色。
  const saved = sessionStorage.getItem(THEME_KEY);
  const dark = saved !== "light";
  document.documentElement.dataset.theme = dark ? "dark" : "light";
  $("hdr-theme").textContent = dark ? "☾" : "☀";
  $("hdr-theme").onclick = () => {
    const nowDark = document.documentElement.dataset.theme === "dark";
    document.documentElement.dataset.theme = nowDark ? "light" : "dark";
    sessionStorage.setItem(THEME_KEY, nowDark ? "light" : "dark");
    $("hdr-theme").textContent = nowDark ? "☀" : "☾";
  };
}

// ---------- sidebar 折叠 ----------
const NARROW_MQ = "(max-width: 767px)";

function initSidebarToggle() {
  const shell = $("app");
  // 窄屏（抽屉模式）默认折叠；桌面端读取持久化偏好
  const stored = localStorage.getItem(SIDEBAR_KEY);
  const collapsed = stored === "1" || (stored === null && window.matchMedia(NARROW_MQ).matches);
  shell.classList.toggle("sidebar-collapsed", collapsed);
  $("hdr-sidebar").onclick = () => {
    const now = shell.classList.toggle("sidebar-collapsed");
    localStorage.setItem(SIDEBAR_KEY, now ? "1" : "0");
  };
}

// 窄屏抽屉模式下，选中会话后自动收起抽屉（不写入持久化偏好）
function collapseSidebarIfNarrow() {
  if (window.matchMedia(NARROW_MQ).matches) $("app").classList.add("sidebar-collapsed");
}

// ---------- toast ----------
function toast(msg, info) {
  const t = document.createElement("div");
  t.className = "toast" + (info ? " is-info" : "");
  t.textContent = msg;
  $("toasts").appendChild(t);
  setTimeout(() => t.remove(), 5000);
}

// ---------- 确认弹窗（替代原生 confirm） ----------
// confirmDialog({title, body, okLabel}) → Promise<boolean>
// Esc / 点遮罩 = 取消；Enter = 确认。
function confirmDialog({ title, body, okLabel }) {
  return new Promise((resolve) => {
    const wrap = $("confirm-modal");
    const ok = $("confirm-ok");
    const cancel = $("confirm-cancel");
    $("confirm-title").textContent = title;
    $("confirm-body").textContent = body;
    ok.textContent = okLabel || "确认";
    wrap.hidden = false;
    ok.focus();
    const done = (v) => {
      wrap.hidden = true;
      ok.onclick = cancel.onclick = wrap.onclick = null;
      document.removeEventListener("keydown", onKey, true);
      resolve(v);
    };
    const onKey = (e) => {
      if (e.key === "Escape") { e.stopPropagation(); done(false); }
      else if (e.key === "Enter") { e.stopPropagation(); done(true); }
    };
    document.addEventListener("keydown", onKey, true);
    ok.onclick = () => done(true);
    cancel.onclick = () => done(false);
    wrap.onclick = (e) => { if (e.target === wrap) done(false); };
  });
}

// ---------- app ----------
const app = {
  token: sessionStorage.getItem(TOKEN_KEY) || "",
  sessionId: null,
  busy: false,
  api: null,
  stream: null,
  transcript: null,
  sidebar: null,
  composer: null,
  statusbar: null,
  picker: null,
  models: [],          // [{provider, name, context_window}]
  defaultModelRef: "", // "provider/model"
  curModelRef: "",     // 当前会话选中
  curReasoning: "",    // 当前会话 reasoning effort
  reasoningOverridden: false,
  lastSubmit: null, // {text, key} —— 幂等重发
  sessionList: [],   // 已加载的会话列表（分页累加）
  sessCursor: "",    // 下一页游标（"" = 没有更多）
  sessLoading: false,
  showArchived: false, // 侧栏归档视图开关
  readOnly: false,   // 当前会话为只读子 agent 会话
};

function setBadge(el, cls, text) {
  el.className = "badge" + (cls ? " " + cls : "");
  el.querySelector(".txt").textContent = text;
}

function setConn(state, detail) {
  const map = {
    connecting: ["is-reconnecting", "connecting…"],
    live: ["is-live", "live"],
    reconnecting: ["is-reconnecting", detail ? `reconnecting (${detail})` : "reconnecting…"],
    draining: ["is-draining", "draining"],
    dead: ["is-dead", detail || "disconnected"],
  };
  const [cls, text] = map[state] || ["", state];
  setBadge($("hdr-conn"), cls, text);
}

function setSessionState(state) {
  const map = {
    idle: ["", "idle"],
    running: ["is-running", "running"],
    awaiting_approval: ["is-awaiting", "awaiting approval"],
    cancelling: ["is-awaiting", "cancelling"],
    booting: ["", "booting"],
    fatal: ["is-dead", "fatal"],
    closed: ["", "closed"],
  };
  const [cls, text] = map[state] || ["", state || ""];
  setBadge($("hdr-state"), cls, text);
  app.busy = state === "running" || state === "awaiting_approval" || state === "cancelling";
  app.composer.setRunning(app.busy);
  $("cancel-btn").hidden = !app.busy;
}

function showGate(err) {
  app.stream?.detach();
  $("app").hidden = true;
  $("gate").hidden = false;
  const e = $("gate-error");
  if (err) { e.textContent = err; e.hidden = false; } else { e.hidden = true; }
}

function showApp() {
  $("gate").hidden = true;
  $("app").hidden = false;
}

function onUnauthorized() {
  sessionStorage.removeItem(TOKEN_KEY);
  app.token = "";
  showGate("token invalid or expired — paste the current serve token");
}

// 复制完整工具输出：实时 tool.completed 事件只带有界 preview，
// 完整内容从 snapshot 消息历史里按 call_id 取。
async function fetchToolOutput(callId) {
  if (!app.sessionId || !callId) throw new Error("no active session");
  const snap = await app.api.snapshot(app.sessionId);
  for (const m of snap.messages || []) {
    for (const part of m.parts || []) {
      const r = part.kind === "tool_result" ? part.tool_result : null;
      if (!r || r.call_id !== callId) continue;
      const texts = (r.content || []).filter((c) => c.kind === "text" && c.text).map((c) => c.text);
      const out = texts.join("\n");
      if (out) return out;
      if (r.error && r.error.message) return r.error.message;
      throw new Error("tool output unavailable (empty or compacted)");
    }
  }
  throw new Error("tool result not found in session history");
}

// ---------- model / reasoning 状态同步 ----------

function modelLabel(ref) {
  // 只显示 model 名（去掉 provider 前缀），更紧凑
  return ref ? ref.split("/").pop() : "model";
}

function reasoningLabel(effort, overridden) {
  const e = effort || "default";
  const map = { default: "reasoning", off: "reasoning: off", low: "reasoning: low", medium: "reasoning: medium", high: "reasoning: high" };
  return map[e] + (overridden && e !== "default" ? " ★" : "");
}

function syncPickerLabels() {
  $("model-btn").querySelector(".picker-label").textContent = modelLabel(app.curModelRef);
  $("reasoning-btn").querySelector(".picker-label").textContent = reasoningLabel(app.curReasoning, app.reasoningOverridden);
}

function applySnapshotMeta(snap) {
  if (snap.provider_name && snap.model_name) {
    app.curModelRef = snap.provider_name + "/" + snap.model_name;
  } else if (snap.model_name) {
    app.curModelRef = app.defaultModelRef || snap.model_name;
  }
  app.curReasoning = snap.reasoning_effort || "";
  app.reasoningOverridden = !!snap.reasoning_overridden;
  syncPickerLabels();
}

// ---------- session loading ----------

const SESSION_PAGE_SIZE = 30;

// 刷新 = 重拉「已加载数量」大小的一页并整列替换：新会话/状态变化都能
// 体现，且不丢已展开的浏览深度（瀑布流页数）。
async function refreshSessions() {
  try {
    const limit = Math.max(app.sessionList.length, SESSION_PAGE_SIZE);
    const { sessions, next_cursor } = await app.api.listSessions(limit, "", app.showArchived);
    app.sessionList = sessions || [];
    app.sessCursor = next_cursor || "";
    app.sidebar.render(app.sessionList);
    if (app.sessionId) app.sidebar.setActive(app.sessionId);
  } catch (e) {
    if (e.status !== 401) console.warn("list sessions:", e);
  }
}

// 瀑布流：滚动接近底部时拉取下一页。
async function loadMoreSessions() {
  if (app.sessLoading || !app.sessCursor) return;
  app.sessLoading = true;
  try {
    const { sessions, next_cursor } = await app.api.listSessions(SESSION_PAGE_SIZE, app.sessCursor, app.showArchived);
    app.sessionList = app.sessionList.concat(sessions || []);
    app.sessCursor = next_cursor || "";
    app.sidebar.render(app.sessionList);
    if (app.sessionId) app.sidebar.setActive(app.sessionId);
  } catch (e) {
    if (e.status !== 401) console.warn("load more sessions:", e);
  } finally {
    app.sessLoading = false;
  }
}

// 归档 / 取消归档 / 删除会话（侧栏条目操作）
async function onSessionAction(id, action) {
  try {
    if (action === "delete") {
      const sess = app.sessionList.find((x) => x.id === id);
      const title = (sess && sess.title) || shortId(id);
      const ok = await confirmDialog({
        title: "删除会话",
        body: `「${title}」将被永久删除，包括全部消息与事件记录。该操作不可恢复。`,
        okLabel: "删除",
      });
      if (!ok) return;
      await app.api.deleteSession(id);
      if (id === app.sessionId) {
        // 删的是当前打开的会话：断开流、回空态
        app.stream.detach();
        app.sessionId = null;
        app.transcript.clear();
        renderPlanInto($("plan-panel"), null);
        $("hdr-session").hidden = true;
        $("empty-state").hidden = false;
        setSessionState("closed");
      }
      toast("会话已删除", true);
    } else {
      await app.api.archiveSession(id, action === "archive");
      toast(action === "archive" ? "已归档" : "已取消归档", true);
    }
  } catch (e) {
    if (e.status !== 401) toast("操作失败: " + e.message);
  }
  await refreshSessions();
}

// 只读模式（子 agent 会话）：composer/模型切换禁用；审批与提问卡片不受影响。
function setReadOnly(snap) {
  app.readOnly = !!snap.delegated;
  app.composer.setReadOnly(app.readOnly);
  $("send-btn").disabled = app.readOnly;
  $("model-btn").disabled = app.readOnly;
  $("reasoning-btn").disabled = app.readOnly;
  const badge = $("hdr-readonly");
  badge.hidden = !app.readOnly;
  badge.title = snap.parent_session_id ? `parent: ${snap.parent_session_id}` : "";
}

async function openSession(id) {
  app.stream.detach();
  app.sessionId = id;
  app.sidebar.setActive(id);
  $("empty-state").hidden = true;
  $("hdr-session").hidden = false;
  $("hdr-session").textContent = shortId(id);

  let snap;
  try {
    snap = await app.api.snapshot(id);
  } catch (e) {
    if (e.status === 404) {
      // 非 live：先 resume 再取快照
      await app.api.resumeSession(id);
      snap = await app.api.snapshot(id);
    } else {
      throw e;
    }
  }
  app.transcript.applySnapshot(snap);
  renderPlanInto($("plan-panel"), snap.plan);
  setReadOnly(snap);
  setSessionState(snap.state);
  applySnapshotMeta(snap);
  app.statusbar.setUsage(snap.usage, snap.context_window);
  app.statusbar.setTurns(snap.turn_count);
  app.stream.attach(id, snap.event_seq || 0);
}

async function newSession() {
  const { session_id } = await app.api.createSession();
  await refreshSessions();
  await openSession(session_id);
}

// ---------- model / reasoning 切换 ----------

async function pickModel(ref) {
  if (!app.sessionId) { toast("先创建或选择一个会话"); return; }
  try {
    const r = await app.api.setModel(app.sessionId, ref);
    // 直接采用 picker 的 ref：它与列表项的 currentRef 比较同源，必然匹配。
    // （SetModelResult 无 JSON tag，响应键是大写的 Cur/Meta，拼读易错。）
    app.curModelRef = ref;
    syncPickerLabels();
    const meta = r.Meta || r.meta || {};
    if (meta.context_window || meta.ContextWindow) {
      app.statusbar.setUsage(null, meta.context_window || meta.ContextWindow);
    }
    toast("模型已切换为 " + modelLabel(app.curModelRef), true);
  } catch (e) {
    if (e.status !== 401) toast("切换模型失败: " + e.message);
  }
}

async function pickReasoning(effort) {
  if (!app.sessionId) { toast("先创建或选择一个会话"); return; }
  try {
    const r = await app.api.setReasoning(app.sessionId, effort);
    // 同上：SetReasoningResult 响应键是大写 Effective/Overridden；effort
    // 来自 picker 固定选项集，直接采用。
    app.curReasoning = effort;
    app.reasoningOverridden = (r.Overridden ?? r.overridden) ?? (effort !== "default");
    syncPickerLabels();
    toast("reasoning: " + (effort === "default" ? "默认" : effort), true);
  } catch (e) {
    if (e.status !== 401) toast("设置 reasoning 失败: " + e.message);
  }
}

// ---------- events ----------

function onRuntimeEvent(evt) {
  app.transcript.handleEvent(evt);
  switch (evt.kind) {
    case "turn.started":
      setSessionState("running");
      break;
    case "turn.finished":
      setSessionState("idle");
      if (evt.payload?.usage) app.statusbar.setUsage(evt.payload.usage, null);
      refreshSessions();
      break;
    case "approval.requested":
      setSessionState("awaiting_approval");
      break;
    case "approval.resolved":
      setSessionState("running");
      break;
    case "run.cancel_requested":
      setSessionState("cancelling");
      break;
    case "run.cancelled":
    case "runtime.fatal":
      setSessionState("idle");
      break;
    case "usage.updated":
      app.statusbar.setUsage(evt.payload, null);
      break;
    case "plan.updated":
      renderPlanInto($("plan-panel"), evt.payload);
      break;
    case "model.changed":
      // 会话内 /model 切换（其他客户端触发）同步到 UI
      if (evt.payload?.cur) {
        app.curModelRef = (evt.payload.cur.provider || "") + "/" + (evt.payload.cur.model || "");
        syncPickerLabels();
      }
      break;
    case "reasoning.changed":
      if (evt.payload?.effective) {
        app.curReasoning = evt.payload.effective.effort || "";
        app.reasoningOverridden = !!evt.payload.overridden;
        syncPickerLabels();
      }
      break;
    default:
      break;
  }
}

async function resync(reason) {
  console.info("resync:", reason);
  app.stream.detach();
  if (!app.sessionId) return;
  try {
    await openSession(app.sessionId);
  } catch (e) {
    if (e.status !== 401) toast("resync failed: " + e.message);
  }
}

// ---------- boot ----------

async function boot() {
  initTheme();
  initSidebarToggle();

  app.api = createApi({ getToken: () => app.token, onUnauthorized });

  app.transcript = new Transcript($("transcript"), $("blocks"), {
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
    onError: (e) => toast(e.message),
  });
  app.transcript.setFollowButton($("follow-btn"));

  // 会话列表瀑布流：滚动接近底部时加载下一页
  $("session-list").addEventListener("scroll", () => {
    const list = $("session-list");
    if (list.scrollHeight - list.scrollTop - list.clientHeight < 120) loadMoreSessions();
  });

  app.sidebar = new Sidebar($("session-list"), {
    onSelect: (id) => {
      if (id === app.sessionId) return;
      collapseSidebarIfNarrow();
      openSession(id).catch((e) => { if (e.status !== 401) toast("open session: " + e.message); });
    },
    onAction: (id, action) => { onSessionAction(id, action); },
  });
  // 归档视图切换：重置分页状态后整列重拉
  $("toggle-archived").onclick = () => {
    app.showArchived = !app.showArchived;
    app.sidebar.archivedView = app.showArchived;
    app.sessionList = [];
    app.sessCursor = "";
    const btn = $("toggle-archived");
    btn.textContent = app.showArchived ? "← 返回" : "归档";
    btn.title = app.showArchived ? "返回会话列表" : "查看归档会话";
    btn.classList.toggle("is-active", app.showArchived);
    $("new-session").hidden = app.showArchived;
    refreshSessions();
  };
  $("new-session").onclick = () => {
    newSession().catch((e) => { if (e.status !== 401) toast("new session: " + e.message); });
  };

  app.composer = new Composer({
    textarea: $("composer-input"),
    sendBtn: $("send-btn"),
    cancelBtn: $("cancel-btn"),
    hint: $("composer-hint"),
    onSubmit: submitPrompt,
    onCancel: () => {
      if (!app.sessionId) return;
      app.api.cancelTurn(app.sessionId).catch((e) => {
        if (e.status !== 401) toast("cancel: " + e.message);
      });
    },
  });

  app.statusbar = new Statusbar({
    ctxEl: $("sb-ctx"), usageEl: $("sb-usage"), turnEl: $("sb-turn"), versionEl: $("sb-version"),
  });

  // 模型 / reasoning 切换器
  app.picker = new Picker($("menu"));
  $("model-btn").onclick = () => {
    if (app.picker.current === "model") { app.picker.close(); return; }
    app.picker.openModels($("model-btn"), {
      models: app.models,
      defaultRef: app.defaultModelRef,
      currentRef: app.curModelRef,
      onPick: pickModel,
    });
  };
  $("reasoning-btn").onclick = () => {
    if (app.picker.current === "reasoning") { app.picker.close(); return; }
    app.picker.openReasoning($("reasoning-btn"), {
      current: app.curReasoning,
      overridden: app.reasoningOverridden,
      onPick: pickReasoning,
    });
  };

  app.stream = new EventStream({
    getToken: () => app.token,
    onEvent: onRuntimeEvent,
    onResync: resync,
    onDraining: () => {
      setConn("draining");
      const b = $("banner");
      b.textContent = "⚠ server is draining (restart in progress) — reconnect paused; refresh later";
      b.hidden = false;
    },
    onConn: (state, detail) => {
      setConn(state, detail);
      if (state === "live") $("banner").hidden = true;
    },
    onAuthError: onUnauthorized,
  });

  $("hdr-session").onclick = () => {
    if (app.sessionId) navigator.clipboard?.writeText(app.sessionId).then(() => toast("session id copied", true));
  };

  // 侧栏轮询（页面可见时，5s）
  setInterval(() => {
    if (document.visibilityState === "visible" && !$("app").hidden) refreshSessions();
  }, 5000);

  if (!app.token) {
    showGate();
  } else {
    await enter();
  }
}

async function enter() {
  try {
    const meta = await app.api.metaVersion();
    app.statusbar.setVersion(meta.version);
    // 加载模型目录（picker 数据源）
    try {
      const cat = await app.api.metaModels();
      app.models = cat.models || [];
      app.defaultModelRef = cat.default || "";
    } catch (e) {
      if (e.status !== 401) console.warn("load models:", e);
    }
    showApp();
    await refreshSessions();
    // 首入落地态：最近更新的会话；无会话则空态
    const { sessions } = await app.api.listSessions(1);
    if (sessions && sessions.length > 0) {
      await openSession(sessions[0].id);
    } else {
      $("empty-state").hidden = false;
      $("hdr-session").hidden = true;
      // 无会话时 picker 显示默认模型
      app.curModelRef = app.defaultModelRef;
      syncPickerLabels();
    }
  } catch (e) {
    if (e.status !== 401) showGate("connect failed: " + e.message);
  }
}

async function submitPrompt(text) {
  if (app.readOnly) {
    toast("子 agent 会话为只读，不能追问");
    return;
  }
  // 幂等键：同一文本重发共享同键（双击/网络重试不产生重复 turn）
  let key;
  if (app.lastSubmit && app.lastSubmit.text === text) {
    key = app.lastSubmit.key;
  } else {
    key = crypto.randomUUID();
  }
  app.lastSubmit = { text, key };
  try {
    if (!app.sessionId) await newSession();
    await app.api.submitPrompt(app.sessionId, text, key);
    app.composer.clearDraft();
    app.lastSubmit = null;
    refreshSessions();
  } catch (e) {
    if (e.status === 401) return;
    app.composer.restoreDraft(text);
    toast("send failed: " + e.message);
  }
}

$("gate-form").addEventListener("submit", (e) => {
  e.preventDefault();
  const token = $("gate-token").value.trim();
  if (!token) return;
  app.token = token;
  sessionStorage.setItem(TOKEN_KEY, token);
  enter();
});

boot();
