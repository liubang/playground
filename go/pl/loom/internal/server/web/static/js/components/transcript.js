// transcript.js — 消息流容器与事件→块调度（docs/WEB_DESIGN.md §3.3/§5）。
// 状态来源仅 snapshot（首屏）+ SSE（此后全部）；未知 kind 一律忽略。

import {
  el, userBlock, assistantBlock, streamBlock, reasoningBlock, thinkingBlock,
  toolBlock, attachDiff, approvalCard, questionCard, resolvedNotice,
  noticeBlock, fatalBlock, histTarget, histCompletion,
} from "./blocks.js";
import { diffForToolCall } from "../diff.js";

const FOLLOW_THRESHOLD_PX = 80;

export class Transcript {
  // scroller: 滚动容器；container: 块父节点
  // io: { resolveApproval(payload, {decision, always}), answerQuestion(id, answer) }
  constructor(scroller, container, io) {
    this.scroller = scroller;
    this.container = container;
    this.io = io;
    this.stream = null;            // 当前 stream 块（按 request 生命周期）
    this.reasoning = null;
    this.thinking = null;          // 等待模型输出的三点动画节点
    this.tools = new Map();        // call_id → tool block api
    this.approvals = new Map();    // approval_id → card api
    this.questions = new Map();    // question_id → card api
    this.steers = [];              // pending steer notice（{el, text}，FIFO）
    this.following = true;
    this._rafPending = false;

    scroller.addEventListener("scroll", () => {
      const gap = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight;
      this.following = gap < FOLLOW_THRESHOLD_PX;
      if (this.followBtn) this.followBtn.hidden = this.following;
    });
  }

  setFollowButton(btn) {
    this.followBtn = btn;
    btn.onclick = () => {
      this.following = true;
      btn.hidden = true;
      this._scrollToBottom();
    };
  }

  clear() {
    this.container.textContent = "";
    this.stream = null;
    this.reasoning = null;
    this.thinking = null;
    this.tools.clear();
    this.approvals.clear();
    this.questions.clear();
    this.steers = [];
    this.following = true;
    if (this.followBtn) this.followBtn.hidden = true;
  }

  _scrollToBottom() {
    this.scroller.scrollTop = this.scroller.scrollHeight;
  }

  _maybeFollow() {
    if (!this.following || this._rafPending) return;
    this._rafPending = true;
    requestAnimationFrame(() => {
      this._rafPending = false;
      if (this.following) this._scrollToBottom();
    });
  }

  _append(node) {
    // 新块到达视为收到新对话：强制回到底部。流式 delta 仍遵循 following
    // 标志（用户上翻阅读时不被增量打扰），但下一个新块会重新带到底部。
    this.following = true;
    if (this.followBtn) this.followBtn.hidden = true;
    this.container.appendChild(node);
    this._maybeFollow();
  }

  // --- thinking 动画（等待模型输出） ---

  _showThinking() {
    if (this.thinking) return;
    this.thinking = thinkingBlock();
    this._append(this.thinking);
  }

  _hideThinking() {
    if (!this.thinking) return;
    this.thinking.remove();
    this.thinking = null;
  }

  // --- 首屏：snapshot → 块（§3.3） ---

  applySnapshot(snap) {
    this.clear();
    const histTools = new Map(); // call_id → tool block api（跨消息配对 tool_result）
    for (const m of snap.messages || []) {
      let textRun = [];
      const flushText = () => {
        if (textRun.length === 0) return;
        const text = textRun.join("\n");
        textRun = [];
        if (m.role === "user") this._append(userBlock(text));
        else this._append(assistantBlock(text));
      };
      for (const p of m.parts || []) {
        switch (p.kind) {
          case "text":
            if (p.text) textRun.push(p.text);
            break;
          case "reasoning":
            flushText();
            if (p.reasoning?.text) {
              const rb = reasoningBlock();
              rb.append(p.reasoning.text);
              this._append(rb.el);
            }
            break;
          case "tool_call":
            flushText();
            if (p.tool_call) {
              const tb = toolBlock({ tool_name: p.tool_call.name, target: histTarget(p.tool_call) });
              if (p.tool_call.id) histTools.set(p.tool_call.id, tb);
              // diff 不落盘（只在实时 tool.prepared 载荷里）：历史重建时从
              // edit/write 参数本地重算（diff.js diffForToolCall）
              const diffText = diffForToolCall(p.tool_call.name, p.tool_call.arguments);
              if (diffText) attachDiff(tb.el, diffText);
              this._append(tb.el);
            }
            break;
          case "tool_result":
            flushText();
            if (p.tool_result) {
              const tb = histTools.get(p.tool_result.call_id);
              if (tb) tb.complete(histCompletion(p.tool_result));
            }
            break;
          case "image":
            flushText();
            this._append(noticeBlock("[image]"));
            break;
          default:
            break; // artifact_ref 等：忽略
        }
      }
      flushText();
    }
    for (const pr of snap.pending_requests || []) {
      if (pr.kind === "approval" && pr.approval) this._addApprovalCard(pr.approval);
      else if (pr.kind === "question" && pr.question) this._addQuestionCard(pr.question);
    }
    // pending steer 队列重建（STEER_DESIGN §4.5：snapshot 兜底）
    for (const text of snap.pending_steers || []) this._addSteerNotice(text);
    this._scrollToBottom();
  }

  // --- SSE 事件调度（§5；未知 kind 忽略） ---

  handleEvent(evt) {
    const p = evt.payload || {};
    switch (evt.kind) {
      case "turn.started":
        this._hideThinking();
        this._drainSteerNotices(p.prompt || "");
        this._append(userBlock(p.prompt || ""));
        this._showThinking();
        break;
      case "turn.finished":
        this._hideThinking();
        this._finalizeStream();
        this._finalizeReasoning();
        break;
      case "model.text_delta":
        this._hideThinking();
        this._ensureStream().append(p.delta || "");
        this._maybeFollow();
        break;
      case "model.reasoning_delta":
        this._hideThinking();
        this._ensureReasoning().append(p.delta || "");
        this._maybeFollow();
        break;
      case "model.response_completed":
        this._hideThinking();
        // canonical 校正：以 completed.text 整段替换草稿（§3.2 铁律 3）
        if (p.text) {
          this._discardStream();
          this._append(assistantBlock(p.text));
        } else {
          this._finalizeStream();
        }
        this._finalizeReasoning();
        break;
      case "model.request_failed":
        this._hideThinking();
        this._append(noticeBlock(`model request failed (${p.stage || "unknown"}): ${p.code || ""}`, true));
        break;
      case "tool.prepared": {
        this._hideThinking();
        // 实时事件只带有界 preview；完整输出经 io.fetchToolOutput 按需取
        const tb = toolBlock(p, {
          onCopy: this.io.fetchToolOutput
            ? () => this.io.fetchToolOutput(p.call_id)
            : null,
        });
        if (p.call_id) this.tools.set(p.call_id, tb);
        if (p.diff) attachDiff(tb.el, p.diff);
        this._append(tb.el);
        break;
      }
      case "tool.started":
        break; // 块在 prepared 已建，running 为默认态
      case "tool.completed": {
        const tb = this.tools.get(p.call_id);
        if (tb) tb.complete(p);
        // 工具完成后模型会继续思考下一步，重新亮起等待动画
        this._showThinking();
        break;
      }
      case "approval.requested":
        this._hideThinking();
        this._addApprovalCard(p);
        break;
      case "approval.resolved":
        this._collapseApproval(p.approval_id, p.decision === "allow", p.actor || "another client");
        this._showThinking();
        break;
      case "question.asked":
        this._hideThinking();
        this._addQuestionCard(p);
        break;
      case "question.answered":
        this._collapseQuestion(p.question_id, p.skipped);
        this._showThinking();
        break;
      case "steer.queued":
        this._addSteerNotice(p.text || p.prompt || "");
        break;
      case "steer.injected": {
        // cell 严格 FIFO：移除头部第一条 queued notice，转为正式 user block
        const head = this.steers.shift();
        if (head) head.el.remove();
        if (p.text) this._append(userBlock(p.text));
        this._showThinking();
        break;
      }
      case "run.cancel_requested":
        this._append(noticeBlock("cancelling…"));
        break;
      case "run.cancelled":
        this._hideThinking();
        this._append(noticeBlock("turn cancelled", true));
        this._finalizeStream();
        break;
      case "context.compacted":
        this._append(noticeBlock("context compacted"));
        break;
      case "budget.notice":
        this._append(noticeBlock("budget notice", true));
        break;
      case "runtime.warning":
        this._append(noticeBlock(p.message || "runtime warning", true));
        break;
      case "runtime.fatal":
        this._hideThinking();
        this._append(fatalBlock(p.message || "runtime fatal"));
        break;
      case "subagent.started":
        this._append(noticeBlock(`subagent started: ${p.role || p.session_id || ""}`));
        break;
      case "subagent.finished":
        this._append(noticeBlock(`subagent finished: ${p.role || p.session_id || ""}`));
        break;
      default:
        break; // 未知 kind：忽略（契约第 2 条）
    }
  }

  // --- pending steer 通知生命周期（queued → injected / turn.started 接力） ---

  _addSteerNotice(text) {
    const n = noticeBlock(`steer queued: “${text}”`);
    this.steers.push({ el: n, text });
    this._append(n);
  }

  // turn.started 的 prompt 可能是 steer 接力产物（leftover 以 "\n\n" 合并）：
  // 文本命中 queued notice 即移除；未命中的保留（turn 内仍会 steer.injected）。
  _drainSteerNotices(prompt) {
    if (this.steers.length === 0) return;
    const kept = [];
    for (const s of this.steers) {
      if (s.text && prompt.includes(s.text)) s.el.remove();
      else kept.push(s);
    }
    this.steers = kept;
  }

  _ensureStream() {
    if (!this.stream) {
      this.stream = streamBlock();
      this._append(this.stream.el);
    }
    return this.stream;
  }

  _ensureReasoning() {
    if (!this.reasoning) {
      this.reasoning = reasoningBlock();
      this._append(this.reasoning.el);
    }
    return this.reasoning;
  }

  _finalizeStream() {
    if (!this.stream) return;
    const s = this.stream;
    this.stream = null;
    // 草稿已是 markdown 实时渲染：收笔做最终渲染即可，不再整体替换节点
    if (s.text().trim()) s.finalize();
    else s.el.remove();
    this._maybeFollow();
  }

  _discardStream() {
    if (!this.stream) return;
    this.stream.discard();
    this.stream.el.remove();
    this.stream = null;
  }

  _finalizeReasoning() {
    this.reasoning = null;
  }

  // --- 审批 / 问答卡片生命周期 ---

  _addApprovalCard(p) {
    if (!p.approval_id || this.approvals.has(p.approval_id)) return;
    const card = approvalCard(p, {
      onResolve: async ({ decision, always }) => {
        card.setResolving();
        try {
          await this.io.resolveApproval(p, { decision, always });
          this._collapseApproval(p.approval_id, decision === "allow", "you");
        } catch (e) {
          if (e.code === "binding_mismatch") {
            this._collapseApproval(p.approval_id, true, "another client");
          } else {
            this.io.onError(e);
          }
        }
      },
    });
    this.approvals.set(p.approval_id, card);
    this._append(card.el);
  }

  _collapseApproval(approvalId, allowed, actor) {
    const card = this.approvals.get(approvalId);
    if (!card) return;
    this.approvals.delete(approvalId);
    card.el.replaceWith(resolvedNotice(allowed, { actor, what: "approval" }));
    this._maybeFollow();
  }

  _addQuestionCard(p) {
    const id = p.question_id || p.id;
    if (!id || this.questions.has(id)) return;
    const card = questionCard(p, {
      onAnswer: async (answer) => {
        card.setResolving();
        try {
          await this.io.answerQuestion(id, answer);
          this._collapseQuestion(id, answer.skipped);
        } catch (e) {
          if (e.code === "binding_mismatch") {
            this._collapseQuestion(id, false);
          } else {
            this.io.onError(e);
          }
        }
      },
    });
    this.questions.set(id, card);
    this._append(card.el);
  }

  _collapseQuestion(questionId, skipped) {
    const card = this.questions.get(questionId);
    if (!card) return;
    this.questions.delete(questionId);
    card.el.replaceWith(noticeBlock(skipped ? "question skipped" : "question answered"));
    this._maybeFollow();
  }
}
