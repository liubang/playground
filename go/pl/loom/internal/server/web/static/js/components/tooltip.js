// tooltip.js — 主题化悬浮提示：全局接管 title 属性，渲染与主题一致的浮层。
// 调用方零改动：mouseover 时发现带 title 的元素（closest 向上匹配，与原生
// 提示的命中语义一致），把文本暂存到 data-tip 并移除 title——原生白底提示
// 因此不再出现；短暂延迟后显示自绘浮层。之后代码重新 setAttribute("title")
// 的（如 MCP 徽标的工具清单）在下次 hover 时会再被接管。
//
// 浮层为共享单例、pointer-events:none，默认在目标上方居中，空间不足翻转
// 到下方；点击/滚动/按键/窗口失焦立即收起。

const DELAY_MS = 350;

let tipEl = null; // 共享浮层
let timer = 0;
let current = null; // 当前悬停目标

function ensureEl() {
  if (!tipEl) {
    tipEl = document.createElement("div");
    tipEl.className = "tip";
    tipEl.hidden = true;
    document.body.appendChild(tipEl);
  }
  return tipEl;
}

function hide() {
  clearTimeout(timer);
  timer = 0;
  current = null;
  if (tipEl) tipEl.hidden = true;
}

function show(target) {
  const text = target.dataset.tip;
  if (!text || !target.isConnected) return;
  const tip = ensureEl();
  tip.textContent = text;
  tip.hidden = false;
  const r = target.getBoundingClientRect();
  const w = tip.offsetWidth, h = tip.offsetHeight;
  let left = r.left + r.width / 2 - w / 2;
  left = Math.max(8, Math.min(left, innerWidth - w - 8));
  let top = r.top - h - 6;
  if (top < 8) top = r.bottom + 6; // 上方空间不足翻转到下方
  tip.style.left = left + "px";
  tip.style.top = top + "px";
}

export function initTooltips() {
  document.addEventListener("mouseover", (e) => {
    const t = e.target instanceof Element ? e.target.closest("[title], [data-tip]") : null;
    if (!t) return;
    if (t.hasAttribute("title")) {
      // 接管：暂存文本并移除原生 title（系统提示只在 hover 静止后出现，
      // mouseover 时移除即可彻底抑制）
      t.dataset.tip = t.getAttribute("title");
      t.removeAttribute("title");
    }
    if (!t.dataset.tip || current === t) return;
    clearTimeout(timer);
    current = t;
    timer = setTimeout(() => show(t), DELAY_MS);
  });
  document.addEventListener("mouseout", (e) => {
    if (!current) return;
    // 目标内部子元素间的移动不收起
    if (e.relatedTarget instanceof Node && current.contains(e.relatedTarget)) return;
    hide();
  });
  document.addEventListener("mousedown", hide, true);
  document.addEventListener("scroll", hide, true);
  document.addEventListener("keydown", hide, true);
  window.addEventListener("blur", hide);
}
