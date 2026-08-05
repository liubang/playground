// composer.js — 输入框（IME 安全、自适应高度、steer 态、取消）。

export class Composer {
  // callbacks: onSubmit(text), onCancel()
  constructor({ textarea, sendBtn, cancelBtn, hint, onSubmit, onCancel }) {
    this.ta = textarea;
    this.hint = hint;
    this.running = false;
    this.readOnly = false;
    this.composing = false;

    const submit = () => {
      if (this.readOnly) return; // 只读会话（子 agent）：不允许追问
      const text = this.ta.value.trim();
      if (!text) return;
      onSubmit(text);
    };

    sendBtn.onclick = submit;
    cancelBtn.onclick = onCancel;

    this.ta.addEventListener("compositionstart", () => { this.composing = true; });
    this.ta.addEventListener("compositionend", () => { this.composing = false; });
    this.ta.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && !e.shiftKey && !this.composing && !e.isComposing) {
        e.preventDefault();
        submit();
      }
    });
    this.ta.addEventListener("input", () => this._autosize());
  }

  _autosize() {
    this.ta.style.height = "auto";
    this.ta.style.height = Math.min(200, this.ta.scrollHeight) + "px";
  }

  clearDraft() {
    this.ta.value = "";
    this._autosize();
  }

  restoreDraft(text) {
    this.ta.value = text;
    this._autosize();
  }

  draft() { return this.ta.value; }

  setRunning(running) {
    this.running = running;
    this._applyState();
  }

  // setReadOnly 切换只读模式（子 agent 会话）：输入禁用，提示固定；
  // 审批/提问走 transcript 卡片，不受影响。
  setReadOnly(readOnly) {
    this.readOnly = readOnly;
    this.ta.disabled = readOnly;
    this._applyState();
  }

  _applyState() {
    if (this.readOnly) {
      this.ta.placeholder = "子 agent 会话 · 只读";
      this.hint.textContent = "只读子 agent 会话：不能追问；审批与提问仍可处理";
      return;
    }
    this.ta.placeholder = this.running ? "Steer this turn…" : "Message loom…";
    this.hint.textContent = this.running
      ? "Enter to steer the running turn · Shift+Enter for newline"
      : "Enter to send · Shift+Enter for newline";
  }

  setCancellable(cancellable, btn) {
    btn.hidden = !cancellable;
  }
}
