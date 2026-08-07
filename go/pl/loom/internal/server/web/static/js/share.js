// share.js — 分享页启动（/share/{token}）：公开只读渲染，复用主界面的
// Transcript/blocks 渲染管线；无 SSE、无 composer、无鉴权（token 即凭证）。

import { Transcript } from "./components/transcript.js";

const THEME_KEY = "loom_theme";
const $ = (id) => document.getElementById(id);

// 主题与主应用一致：默认深色，仅显式存了 "light" 才用浅色。
const saved = sessionStorage.getItem(THEME_KEY);
document.documentElement.dataset.theme = saved !== "light" ? "dark" : "light";

// token 取自路径最后一段；格式非法（非 32 位 hex）直接算无效链接。
const token = location.pathname.split("/").filter(Boolean).pop() || "";

// 分享页 artifact 走公开端点（/v1/shared/* 免 bearer）；内容寻址 + 不可变，
// 按 id+size 缓存避免重复下载。返回 {url, mediaType, blob}（同 main.js 契约），
// mediaType 取自响应 Content-Type，供渲染层区分图片与文本 artifact；blob
// 本体用于文本预览。
const artifactURLCache = new Map();
async function fetchArtifactURL(id, size) {
  const key = `${id}:${size}`;
  const cached = artifactURLCache.get(key);
  if (cached) return cached;
  const res = await fetch(
    `/v1/shared/${encodeURIComponent(token)}/artifacts/${encodeURIComponent(id)}?size=${size}`,
  );
  if (!res.ok) throw new Error(`artifact fetch failed (HTTP ${res.status})`);
  const blob = await res.blob();
  const entry = { url: URL.createObjectURL(blob), mediaType: blob.type || "", blob };
  artifactURLCache.set(key, entry);
  return entry;
}

function showError(text) {
  $("share-error-text").textContent = text;
  $("share-error").hidden = false;
  $("share-title").textContent = "";
  $("share-meta").textContent = "";
}

function fmtTime(iso) {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "";
  return d.toLocaleString();
}

async function boot() {
  if (!/^[0-9a-f]{32}$/.test(token)) {
    showError("链接无效或已撤销。");
    return;
  }
  let view;
  try {
    const res = await fetch(`/v1/shared/${encodeURIComponent(token)}`);
    if (res.status === 404) {
      showError("链接无效或已撤销。");
      return;
    }
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    view = await res.json();
  } catch (e) {
    showError("加载失败：" + e.message);
    return;
  }

  const title = view.title || "shared session";
  document.title = `${title} · loom`;
  $("share-title").textContent = title;
  $("share-title").title = view.session_id || "";
  $("share-meta").textContent = view.updated_at ? `更新于 ${fmtTime(view.updated_at)}` : "";

  // 只读渲染：io 只提供 artifact 解析；无 sendFeedback → 不渲染投票按钮；
  // state 固定 "closed"，applySnapshot 会为最后一轮补齐操作行（复制/时间）。
  const transcript = new Transcript($("transcript"), $("blocks"), { fetchArtifactURL });
  transcript.applySnapshot({ messages: view.messages || [], state: "closed" });
}

boot();
