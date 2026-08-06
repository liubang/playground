// blocks.js — transcript 块渲染器（docs/WEB_DESIGN.md §3.3/§6.3）。
// 铁律：一切模型/工具文本只走 textContent；innerHTML 入口统一位于
// markdown.js（marked → DOMPurify；hljs 高亮片段经 sanitizeHtml 复用同一白名单）。

import { renderMarkdownInto } from "../markdown.js";
import { renderDiff } from "../diff.js";
import { fmtTokens, fmtBytes } from "../format.js";

export function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}

// --- user ---

// 右侧气泡，无标签（参考截图样式）。
export function userBlock(text) {
  const b = el("div", "block block-user");
  b.appendChild(el("div", "user-text", text));
  return b;
}

// --- assistant（完成态，markdown 渲染） ---
// 无标签，左侧纯文本（参考截图样式）。
export function assistantBlock(text) {
  const b = el("div", "block block-assistant");
  const md = el("div", "md");
  renderMarkdownInto(md, text);
  b.appendChild(md);
  return b;
}

// --- stream（进行中草稿，markdown 实时渲染） ---
// 无标签，左侧纯文本。每个 delta 都会重渲全量 buffer（marked 对不完整
// 语法容错良好，未闭合的代码围栏也会按代码块渲染）；渲染走 rAF 合帧 +
// 最小间隔节流，避免长文本下每帧全量解析造成卡顿。
const STREAM_RENDER_MIN_INTERVAL_MS = 60;

export function streamBlock() {
  const b = el("div", "block block-assistant");
  const md = el("div", "md");
  const cursor = el("span", "stream-cursor", "▍");
  b.appendChild(md);
  let buf = "";
  let scheduled = false;
  let destroyed = false;
  let lastRender = 0;
  const render = () => {
    scheduled = false;
    if (destroyed) return;
    lastRender = performance.now();
    renderMarkdownInto(md, buf);
    // 光标跟随渲染内容末尾：末节点是段落/列表项时嵌入其中，避免独占一行
    const last = md.lastElementChild;
    if (last && (last.tagName === "P" || last.tagName === "LI")) last.appendChild(cursor);
    else md.appendChild(cursor);
  };
  const schedule = () => {
    if (scheduled || destroyed) return;
    scheduled = true;
    const wait = Math.max(0, STREAM_RENDER_MIN_INTERVAL_MS - (performance.now() - lastRender));
    setTimeout(() => requestAnimationFrame(render), wait);
  };
  return {
    el: b,
    append(delta) { buf += delta; schedule(); },
    text: () => buf,
    // 收笔：取消未执行的合帧，同步做最终渲染并移除光标。
    finalize() {
      destroyed = true;
      renderMarkdownInto(md, buf);
      cursor.remove();
    },
    // 丢弃（canonical 文本整段替换时）：停止后续合帧渲染。
    discard() { destroyed = true; },
  };
}

// --- thinking（等待模型首个 token / 工具间等待的三点动画） ---
export function thinkingBlock() {
  const b = el("div", "block block-thinking");
  b.appendChild(el("span", "t-dot"));
  b.appendChild(el("span", "t-dot"));
  b.appendChild(el("span", "t-dot"));
  return b;
}

// --- reasoning（折叠块） ---

export function reasoningBlock() {
  const d = document.createElement("details");
  d.className = "block block-reasoning disclosure";
  const summary = el("summary", "", "reasoning");
  const body = el("div", "body");
  d.appendChild(summary);
  d.appendChild(body);
  let buf = "";
  return {
    el: d,
    append(delta) {
      buf += delta;
      body.textContent = buf;
      summary.textContent = `reasoning · ${buf.length} chars`;
    },
  };
}

// --- tool ---

const TOOL_STATUS_TEXT = { ok: "✓ ok", error: "✗ error", canceled: "⊘ canceled" };

// hooks.onCopy: async () => string —— 复制完整输出用。实时事件的 preview
// 是有界摘要，完整内容需向 server 取；snapshot 重建路径有 full_text，用不到。
export function toolBlock(payload, hooks = {}) {
  const b = el("div", "block block-tool");
  const head = el("div", "tool-head");
  head.appendChild(el("span", "tool-name mono", payload.tool_name || "tool"));
  if (payload.target) head.appendChild(el("span", "tool-target mono", payload.target));
  const status = el("span", "tool-status running", "running");
  head.appendChild(status);
  const dur = el("span", "tool-dur mono");
  head.appendChild(dur);
  b.appendChild(head);
  let errEl = null;
  return {
    el: b,
    complete(p) {
      const st = p.status === "success" ? "ok" : (p.status === "error" ? "err" : "canceled");
      status.className = "tool-status " + st;
      status.textContent = TOOL_STATUS_TEXT[st] || p.status || "done";
      if (p.duration_ms != null) dur.textContent = fmtDuration(p.duration_ms);
      if (p.error_message || p.error) {
        errEl = el("div", "tool-error", p.error_message || p.error);
        b.appendChild(errEl);
      }
      if (p.preview) {
        b.appendChild(toolOutput(p.preview, async () => {
          if (p.full_text) return p.full_text;
          if (hooks.onCopy) return hooks.onCopy();
          return p.preview; // server 取不到时兜底复制摘要
        }));
      }
    },
  };
}

// 工具输出区：默认折叠，展开显示有界 preview（截断以 "\n…" 结尾标记）；
// copy 按钮始终复制完整输出。
function toolOutput(preview, getFullText) {
  const d = document.createElement("details");
  d.className = "tool-output disclosure";
  const summary = el("summary");
  const truncated = preview.endsWith("\n…");
  summary.appendChild(el("span", "tool-output-label",
    `output · ${preview.length} chars${truncated ? " · truncated" : ""}`));
  const copyBtn = el("button", "tool-copy", "copy");
  copyBtn.title = "复制完整输出";
  copyBtn.onclick = async (e) => {
    e.preventDefault(); // 不触发 details 展开/收起
    e.stopPropagation();
    try {
      const text = await getFullText();
      await navigator.clipboard.writeText(text);
      copyBtn.textContent = "✓ copied";
    } catch {
      copyBtn.textContent = "copy failed";
    }
    setTimeout(() => { copyBtn.textContent = "copy"; }, 1500);
  };
  summary.appendChild(copyBtn);
  d.appendChild(summary);
  d.appendChild(el("div", "tool-preview mono", preview));
  return d;
}

function fmtDuration(ms) {
  if (ms < 1000) return ms + "ms";
  return (ms / 1000).toFixed(1) + "s";
}

// --- 历史（snapshot 重建）工具块辅助 ---

// histTarget 从 tool_call.arguments（wire 上已是 object）提取展示用目标。
export function histTarget(call) {
  const a = call?.arguments;
  if (!a || typeof a !== "object") return "";
  const v = a.path || a.file_path || a.command || a.cmd || a.query || a.pattern || a.url || "";
  const s = String(v);
  return s.length > 120 ? s.slice(0, 120) + "…" : s;
}

// histCompletion 把 ToolResult 映射为 toolBlock.complete(p) 需要的载荷。
// preview 有界（600 chars）用于展示；full_text 完整保留用于复制。
export function histCompletion(r) {
  const status = r.status === "success" ? "success" : (r.status === "cancelled" ? "canceled" : "error");
  const texts = (r.content || []).filter((c) => c.kind === "text" && c.text).map((c) => c.text);
  const fullText = texts.join("\n");
  let preview = fullText;
  if (preview.length > 600) preview = preview.slice(0, 600) + "\n…";
  let durationMs;
  if (r.started_at && r.finished_at) {
    const ms = new Date(r.finished_at) - new Date(r.started_at);
    if (Number.isFinite(ms) && ms >= 0) durationMs = ms;
  }
  return {
    status,
    duration_ms: durationMs,
    preview,
    full_text: fullText,
    error_message: r.error?.message || "",
  };
}

// tool 块展开 diff（tool.prepared 挂载）
export function attachDiff(blockEl, diffText) {
  blockEl.appendChild(renderDiff(diffText));
}

// --- approval 卡片 ---

// callbacks: onResolve({decision, always})
export function approvalCard(payload, { onResolve }) {
  const card = el("div", "block card-approval");
  const title = el("div", "card-title");
  title.appendChild(el("span", "", "⚠ Approval required"));
  title.appendChild(el("span", "risk", "R" + (payload.risk ?? "?")));
  title.appendChild(el("span", "mono", payload.tool_name || ""));
  card.appendChild(title);
  if (payload.description) card.appendChild(el("div", "desc", payload.description));
  // cmd 仅在 target 与 description 不同时展示，避免同一段话渲染两遍
  if (payload.target && payload.target !== payload.description) {
    const cmd = el("div", "cmd");
    cmd.appendChild(el("code", "", payload.target));
    card.appendChild(cmd);
  }
  if (payload.diff) card.appendChild(renderDiff(payload.diff));

  const actions = el("div", "actions");
  const allow = el("button", "btn btn-primary", "Allow");
  const always = el("button", "btn btn-secondary", "Allow always");
  const deny = el("button", "btn btn-danger", "Deny");
  const memo = el("span", "memo", "allow always remembers this command for the workspace");
  allow.onclick = () => onResolve({ decision: "allow", always: false });
  always.onclick = () => onResolve({ decision: "allow", always: true });
  deny.onclick = () => onResolve({ decision: "deny", always: false });
  actions.appendChild(allow);
  actions.appendChild(always);
  actions.appendChild(deny);
  actions.appendChild(memo);
  card.appendChild(actions);
  return {
    el: card,
    setResolving() {
      allow.disabled = always.disabled = deny.disabled = true;
    },
  };
}

// resolved 收编 notice
export function resolvedNotice(ok, text) {
  const n = el("div", "resolved");
  n.appendChild(el("span", ok ? "ok" : "no", ok ? "✓" : "✗"));
  const t = el("span");
  t.appendChild(el("b", "", (ok ? "Allowed" : "Denied") + " "));
  t.appendChild(document.createTextNode("(" + text.actor + ") · " + text.what));
  n.appendChild(t);
  return n;
}

// --- question 卡片 ---

// callbacks: onAnswer({selected, customText, skipped})
export function questionCard(payload, { onAnswer }) {
  const q = payload.question || payload; // snapshot 重建时为 PendingRequest.Question
  const card = el("div", "block card-question");
  const title = el("div", "card-title");
  title.appendChild(el("span", "", "? Loom asks"));
  if (q.allow_multiple) title.appendChild(el("span", "multi", "（可多选）"));
  card.appendChild(title);
  card.appendChild(el("div", "q-text", q.text || ""));

  const inputType = q.allow_multiple ? "checkbox" : "radio";
  const name = "q_" + Math.random().toString(36).slice(2, 8);
  const optionInputs = [];
  for (const opt of q.options || []) {
    const label = el("label", "opt");
    const input = document.createElement("input");
    input.type = inputType;
    input.name = name;
    input.value = opt.label;
    const span = el("span");
    span.appendChild(document.createTextNode(opt.label));
    if (opt.description) span.appendChild(el("span", "desc", " — " + opt.description));
    label.appendChild(input);
    label.appendChild(span);
    card.appendChild(label);
    optionInputs.push(input);
  }
  const custom = document.createElement("input");
  custom.type = "text";
  custom.placeholder = "custom answer… (optional)";
  card.appendChild(custom);

  const actions = el("div", "actions");
  const submit = el("button", "btn btn-primary", "Submit");
  const skip = el("button", "btn btn-secondary", "Skip");
  submit.onclick = () => {
    const selected = optionInputs.filter((i) => i.checked).map((i) => i.value);
    onAnswer({ selected, custom_text: custom.value.trim(), skipped: false });
  };
  skip.onclick = () => onAnswer({ selected: [], custom_text: "", skipped: true });
  actions.appendChild(submit);
  actions.appendChild(skip);
  card.appendChild(actions);
  return {
    el: card,
    setResolving() { submit.disabled = skip.disabled = true; },
  };
}

// --- plan 面板（plan.updated 驱动；钉在 composer 上方，Claude Code 风格清单） ---

const PLAN_STATUS_ICON = { todo: "○", in_progress: "▶", done: "✓" };

// renderPlanInto 就地重绘面板内容；不触碰 details.open，保留用户的折叠状态。
// plan 为空（无 items）时隐藏面板。
export function renderPlanInto(panel, plan) {
  panel.textContent = "";
  const items = plan?.items || [];
  if (items.length === 0) {
    panel.hidden = true;
    return;
  }
  panel.hidden = false;
  const done = items.filter((i) => i.status === "done").length;
  const summary = el("summary");
  summary.appendChild(el("span", "plan-title", plan.title || "plan"));
  summary.appendChild(el("span", "plan-progress", `${done}/${items.length} done`));
  panel.appendChild(summary);
  const list = el("ul", "plan-items");
  for (const item of items) {
    const li = el("li", "plan-item is-" + (item.status || "todo"));
    li.appendChild(el("span", "plan-icon", PLAN_STATUS_ICON[item.status] || "○"));
    li.appendChild(el("span", "plan-goal", item.goal || ""));
    list.appendChild(li);
  }
  panel.appendChild(list);
}

// --- notice / fatal ---

export function noticeBlock(text, warn) {
  return el("div", "notice" + (warn ? " is-warn" : ""), text);
}

// context.compacted 明细卡片：压缩前后估值 + 触发原因 + 各级动作明细。
export function compactBlock(p) {
  const wrap = el("div", "notice compact");
  const before = fmtTokens(p.est_tokens_before) || "?";
  const after = fmtTokens(p.est_tokens_after) || "?";
  wrap.appendChild(el("div", "compact-head", `⚡ context compacted · ${before} → ${after}`));
  const details = [];
  if (p.trigger) details.push("trigger: " + p.trigger);
  if (p.masked_outputs) {
    const bytes = p.masked_bytes ? ` (${fmtBytes(p.masked_bytes)})` : "";
    details.push(`mask ${p.masked_outputs} outputs${bytes}`);
  }
  if (p.archived_messages) details.push(`archive ${p.archived_messages} msgs`);
  if (p.summarized) details.push("summary handoff");
  if (details.length) wrap.appendChild(el("div", "compact-detail", details.join(" · ")));
  return wrap;
}

export function fatalBlock(text) {
  return el("div", "block block-fatal", text);
}
