// sidebar.js — 会话列表（docs/WEB_DESIGN.md §4.6）。
// 条目字段来自富化后的 SessionSummary：title/model/state/parent_session_id。
// 紧凑单行布局（标题 + 相对时间）；子 agent 会话缩进挂在父会话下方，
// 父会话不在已加载页内时按顶层渲染（分页边界兜底）。

import { el } from "./blocks.js";
import { relTime, shortId } from "../format.js";

export class Sidebar {
  // onAction(id, action)：action ∈ "archive" | "unarchive" | "delete"
  constructor(listEl, { onSelect, onAction }) {
    this.listEl = listEl;
    this.onSelect = onSelect;
    this.onAction = onAction;
    this.activeId = null;
    this.archivedView = false; // 归档视图：条目显示取消归档而非归档
  }

  setActive(id) {
    this.activeId = id;
    for (const item of this.listEl.querySelectorAll(".sess-item")) {
      item.classList.toggle("is-active", item.dataset.id === id);
    }
  }

  render(sessions) {
    this.listEl.textContent = "";
    const ids = new Set(sessions.map((s) => s.id));
    const childrenOf = new Map(); // parent_id → [child…]（保持到达序，即 updated 降序）
    const tops = [];
    for (const s of sessions) {
      if (s.parent_session_id && ids.has(s.parent_session_id)) {
        let arr = childrenOf.get(s.parent_session_id);
        if (!arr) childrenOf.set(s.parent_session_id, (arr = []));
        arr.push(s);
      } else {
        tops.push(s);
      }
    }
    for (const s of tops) {
      this._appendItem(s, false);
      for (const c of childrenOf.get(s.id) || []) this._appendItem(c, true);
    }
  }

  _appendItem(s, isChild) {
    const item = el("button", "sess-item" + (s.id === this.activeId ? " is-active" : "") + (isChild ? " is-child" : ""));
    item.dataset.id = s.id;
    item.title = (s.title || shortId(s.id)) + (s.model_name ? ` · ${s.model_name}` : "");
    if (isChild) item.appendChild(el("span", "child-mark", "↳"));
    item.appendChild(el("span", "t", s.title || shortId(s.id)));
    item.appendChild(el("span", "rt", relTime(s.updated_at)));
    // 悬停操作：归档/取消归档 + 删除（不占常态宽度，hover 时替换时间戳）
    const acts = el("span", "acts");
    const archBtn = el("button", "act", this.archivedView ? "↩" : "⤓");
    archBtn.title = this.archivedView ? "取消归档" : "归档";
    archBtn.onclick = (e) => {
      e.stopPropagation();
      this.onAction(s.id, this.archivedView ? "unarchive" : "archive");
    };
    const delBtn = el("button", "act act-del", "×");
    delBtn.title = "删除会话";
    delBtn.onclick = (e) => {
      e.stopPropagation();
      this.onAction(s.id, "delete");
    };
    acts.appendChild(archBtn);
    acts.appendChild(delBtn);
    item.appendChild(acts);
    item.onclick = () => this.onSelect(s.id);
    this.listEl.appendChild(item);
  }
}
