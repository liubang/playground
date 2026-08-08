// select.js — 通用自绘下拉（替代原生 <select>）。
// 原生 select 的选项浮层由操作系统渲染，与暗色主题割裂且不可定制；
// 这里复用 picker 浮层的视觉语言（暗色底、check 槽位、锚定定位）。
//
// 与原生 select 保持最小兼容面，设置面板的 spec 驱动填充/收集与脏检查
// 因此无需特殊处理：
//   - ctl.value 可读写当前值；
//   - 用户选择后从控件上派发冒泡的 change 事件（onchange 属性与
//     addEventListener 都能收到）；
//   - data-cfg-key / data-cfg-type 等 dataset 由调用方照常设置。

import { el } from "./blocks.js";
import { iconEl } from "../icons.js";

let openPop = null; // 全局同时只开一个浮层

function closeOpenPop() {
  if (!openPop) return;
  openPop.remove();
  openPop = null;
}

document.addEventListener("click", (e) => {
  if (openPop && !openPop.contains(e.target) && openPop._anchor !== e.target && !openPop._anchor.contains(e.target)) {
    closeOpenPop();
  }
}, true);
document.addEventListener("keydown", (e) => {
  // 有浮层打开时 Esc 只关浮层。设置面板的 Esc 关闭逻辑同样注册在
  // document 捕获阶段且更晚，stopPropagation 挡不住同节点的后续监听器，
  // 必须用 stopImmediatePropagation。
  if (e.key === "Escape" && openPop) {
    e.stopImmediatePropagation();
    closeOpenPop();
  }
}, true);
window.addEventListener("blur", closeOpenPop);
window.addEventListener("resize", closeOpenPop);
// 外部滚动会让 fixed 定位漂移，关闭浮层；浮层自身的滚动除外
document.addEventListener("scroll", (e) => {
  if (openPop && !openPop.contains(e.target)) closeOpenPop();
}, true);

// createSelect({ className, options: [[value, label], ...] }) → <button>
// 初始值为 options 第一项（与原生 select 一致）。
export function createSelect({ className = "", options = [] } = {}) {
  const btn = el("button", (className ? className + " " : "") + "sel");
  btn.type = "button";
  const label = el("span", "sel-label");
  btn.appendChild(label);
  const caret = iconEl("caret-down");
  caret.classList.add("sel-caret");
  btn.appendChild(caret);

  let cur = options.length ? options[0][0] : "";

  const render = () => {
    const hit = options.find(([v]) => v === cur);
    label.textContent = hit ? hit[1] : cur;
  };

  Object.defineProperty(btn, "value", {
    get: () => cur,
    set: (v) => {
      cur = v == null ? "" : String(v);
      render();
    },
  });

  btn.onclick = () => {
    if (openPop && openPop._anchor === btn) {
      closeOpenPop();
      return;
    }
    closeOpenPop();
    const pop = el("div", "sel-pop");
    pop._anchor = btn;
    let activeItem = null;
    for (const [v, text] of options) {
      const item = el("button", "sel-item" + (v === cur ? " is-active" : ""));
      item.type = "button";
      item.appendChild(el("span", "sel-item-label", text));
      const chk = el("span", "check");
      if (v === cur) {
        chk.appendChild(iconEl("check"));
        activeItem = item;
      }
      item.appendChild(chk);
      item.onclick = () => {
        closeOpenPop();
        if (v !== cur) {
          btn.value = v;
          btn.dispatchEvent(new Event("change", { bubbles: true }));
        }
      };
      pop.appendChild(item);
    }
    document.body.appendChild(pop);
    openPop = pop;

    // 锚定到按钮下方，空间不足则翻转到上方
    const r = btn.getBoundingClientRect();
    pop.style.minWidth = r.width + "px";
    pop.style.left = Math.max(8, Math.min(r.left, innerWidth - pop.offsetWidth - 8)) + "px";
    if (r.bottom + pop.offsetHeight + 8 > innerHeight && r.top - pop.offsetHeight - 6 > 0) {
      pop.style.top = Math.max(8, r.top - pop.offsetHeight - 6) + "px";
    } else {
      pop.style.top = r.bottom + 6 + "px";
    }
    activeItem?.scrollIntoView({ block: "nearest" });
  };

  render();
  return btn;
}

// closeSelects 关闭当前打开的下拉浮层（面板关闭等场景调用）。
export function closeSelects() {
  closeOpenPop();
}
