// picker.js — 模型 / Reasoning 下拉切换器。
// 复用一个 #menu 浮层：点击按钮 → 锚定定位 → 列表项 → 选中回调。

const REASONING_OPTIONS = [
  { value: "default", label: "默认（跟随模型）" },
  { value: "off", label: "Off" },
  { value: "low", label: "Low" },
  { value: "medium", label: "Medium" },
  { value: "high", label: "High" },
];

export class Picker {
  // menuEl: 共享的 #menu 容器
  constructor(menuEl) {
    this.menuEl = menuEl;
    this.current = null; // 当前打开的 picker id
    this._onOutside = (e) => {
      if (this.menuEl.hidden) return;
      // 点击触发按钮自身时不关闭（按钮 onclick 会 toggle）
      if (this._activeBtn && (this._activeBtn === e.target || this._activeBtn.contains(e.target))) return;
      if (!this.menuEl.contains(e.target)) this.close();
    };
    document.addEventListener("click", this._onOutside);
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape") this.close();
    });
    window.addEventListener("blur", () => this.close());
  }

  // 锚定到 anchorEl 上方（composer 在视口底部，向下会被裁剪）
  _anchor(anchorEl) {
    const r = anchorEl.getBoundingClientRect();
    const m = this.menuEl;
    m.style.top = "";
    m.style.left = Math.max(8, Math.min(r.left, innerWidth - m.offsetWidth - 8)) + "px";
    m.style.bottom = (innerHeight - r.top + 6) + "px";
  }

  close() {
    this.menuEl.hidden = true;
    this.menuEl.textContent = "";
    if (this._activeBtn) {
      this._activeBtn.classList.remove("is-active");
      this._activeBtn = null;
    }
    this.current = null;
  }

  // 打开模型选择器
  // models: [{provider, name, context_window}]
  // currentRef: "provider/model"
  // onPick(ref) => Promise
  openModels(btn, { models, defaultRef, currentRef, onPick }) {
    this.close();
    this.current = "model";
    this._activeBtn = btn;
    btn.classList.add("is-active");
    const m = this.menuEl;
    m.hidden = false;

    const groups = new Map();
    for (const mo of models) {
      if (!groups.has(mo.provider)) groups.set(mo.provider, []);
      groups.get(mo.provider).push(mo);
    }
    for (const [provider, list] of groups) {
      const g = document.createElement("div");
      g.className = "menu-group";
      g.textContent = provider;
      m.appendChild(g);
      for (const mo of list) {
        const ref = provider + "/" + mo.name;
        const item = document.createElement("button");
        item.className = "menu-item" + (ref === currentRef ? " is-active" : "");
        item.appendChild(document.createTextNode(mo.name));
        if (mo.context_window) {
          const cw = document.createElement("span");
          cw.className = "ctx";
          cw.textContent = fmtCtx(mo.context_window);
          item.appendChild(cw);
        }
        // ✓ 槽位始终占位（未选中留空），保持所有行右侧对齐
        const chk = document.createElement("span");
        chk.className = "check";
        chk.textContent = ref === currentRef ? "✓" : "";
        item.appendChild(chk);
        item.onclick = () => {
          this.close();
          if (ref !== currentRef) onPick(ref);
        };
        m.appendChild(item);
      }
    }
    this._anchor(btn);
  }

  // 打开 reasoning 选择器
  // current: "off"/"low"/... 或 "" / null（= default）
  // overridden: bool（是否为会话覆盖）
  // onPick(effort) => Promise
  openReasoning(btn, { current, overridden, onPick }) {
    this.close();
    this.current = "reasoning";
    this._activeBtn = btn;
    btn.classList.add("is-active");
    const m = this.menuEl;
    m.hidden = false;
    const eff = current || "default";
    for (const opt of REASONING_OPTIONS) {
      const item = document.createElement("button");
      item.className = "menu-item" + (opt.value === eff ? " is-active" : "");
      item.appendChild(document.createTextNode(opt.label));
      const chk = document.createElement("span");
      chk.className = "check";
      chk.textContent = opt.value === eff ? "✓" : "";
      item.appendChild(chk);
      item.onclick = () => {
        this.close();
        if (opt.value !== eff) onPick(opt.value);
      };
      m.appendChild(item);
    }
    this._anchor(btn);
  }
}

function fmtCtx(n) {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(0) + "M";
  if (n >= 1000) return (n / 1000).toFixed(0) + "k";
  return String(n);
}
