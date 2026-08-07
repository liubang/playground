// transcript.js — 消息流容器与事件→块调度（docs/WEB_DESIGN.md §3.3/§5）。
// 状态来源仅 snapshot（首屏）+ SSE（此后全部）；未知 kind 一律忽略。

import {
  el, userBlock, assistantBlock, attachAssistantActions, streamBlock, reasoningBlock, thinkingBlock,
  toolBlock, attachDiff, approvalCard, questionCard, resolvedNotice,
  noticeBlock, fatalBlock, histTarget, histCompletion, compactBlock,
  imageBlock, artifactBlock,
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
    this._pendingStreamTs = "";   // 首个 text_delta 事件时间，收笔注入草稿操作行
    this._turnAssistant = null;    // 本轮最新 assistant 块（轮结束时挂操作行）
    this._turnAssistantTs = "";   // 该块内容的事件时间
    this._turnRunID = "";         // 本轮 run id（跟随最新可信事件），反馈投票目标

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
    this._pendingStreamTs = "";
    this._turnAssistant = null;
    this._turnAssistantTs = "";
    this._turnRunID = "";
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

  // 轮结束时把操作行（复制/反馈 + 时间）挂到本轮末段 assistant 块上。
  // 只在轮终止事件（turn.finished / run.cancelled / runtime.fatal）调用，
  // 因此中间段永不出现「挂行→摘除」的闪烁；attachAssistantActions 幂等。
  _attachTurnActions() {
    const blk = this._turnAssistant;
    if (!blk || !blk.isConnected) return;
    attachAssistantActions(blk, {
      createdAt: this._turnAssistantTs,
      fb: this._fbOpts(this._turnRunID),
    });
  }

  // 反馈上下文：runId 为空（无 trace 的旧消息）时不渲染赞/踩。
  // onFeedback 失败在块内回滚选中态，这里同时弹出错误提示。
  _fbOpts(runId) {
    if (!runId || !this.io.sendFeedback) return null;
    return {
      runId,
      feedback: this.io.getFeedback ? this.io.getFeedback(runId) : "",
      onFeedback: async (id, value) => {
        try {
          await this.io.sendFeedback(id, value);
        } catch (e) {
          if (this.io.onError) this.io.onError(e);
          throw e;
        }
      },
    };
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
    let lastAssistant = null;    // 本轮最新 assistant 块（轮结束时挂操作行）
    let lastTs = "";            // 该块的 created_at
    let lastRunId = "";         // 该块消息的 run_id（反馈投票目标）
    // 轮结束 = 遇到下一条 user 消息（历史里轮次已完结，直接挂行）
    const closeTurn = () => {
      if (!lastAssistant) return;
      attachAssistantActions(lastAssistant, { createdAt: lastTs, fb: this._fbOpts(lastRunId) });
      lastAssistant = null;
    };
    for (const m of snap.messages || []) {
      let textRun = [];
      const createdAt = m.created_at || "";
      const flushText = () => {
        if (textRun.length === 0) return;
        const text = textRun.join("\n");
        textRun = [];
        if (m.role === "user") {
          closeTurn();
          this._append(userBlock(text, createdAt));
        } else {
          // 反馈目标：消息落盘时 agent loop 已打上 run_id metadata
          const runId = (m.metadata && m.metadata.run_id) || "";
          if (runId) lastRunId = runId;
          lastAssistant = assistantBlock(text);
          lastTs = createdAt;
          this._append(lastAssistant);
        }
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
              const tb = toolBlock(
                { tool_name: p.tool_call.name, target: histTarget(p.tool_call) },
                { resolveArtifactURL: this.io.fetchArtifactURL },
              );
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
            if (p.image) {
              this._append(imageBlock(p.image.media_type, p.image.data));
            }
            break;
          case "artifact_ref":
            flushText();
            if (p.artifact) {
              this._append(artifactBlock(p.artifact, this.io.fetchArtifactURL));
            }
            break;
          default:
            break;
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
    // 快照可能截在轮次中途：进行中的轮不挂行（留给轮终止事件挂，
    // 状态接力给实时路径）；已完结的轮在此收尾挂行。
    const running = snap.state === "running" || snap.state === "awaiting_approval" || snap.state === "cancelling";
    if (running) {
      this._turnAssistant = lastAssistant;
      this._turnAssistantTs = lastTs;
      this._turnRunID = lastRunId;
    } else {
      closeTurn();
    }
    this._scrollToBottom();
  }

  // --- SSE 事件调度（§5；未知 kind 忽略） ---

  handleEvent(evt) {
    const p = evt.payload || {};
    // run_id 跟随策略：turn.started 的信封 run_id 不可信（发布时新 run
    // 尚未创建——首轮为零值、后续轮带的是上一轮 id），其余事件由 loop 内
    // publishingStore 发出、携带真实 run id，单调跟随最新非空值即可。
    if (evt.kind === "turn.started") {
      this._turnRunID = "";
    } else if (evt.run_id) {
      this._turnRunID = evt.run_id;
    }
    switch (evt.kind) {
      case "turn.started":
        this._hideThinking();
        this._drainSteerNotices(p.prompt || "");
        this._append(userBlock(p.prompt || "", evt.time || ""));
        // 新一轮开始：上一轮末尾的操作行已在上轮终止时挂载（作为该轮
        // 结束标志），本轮状态从新起算（run_id 由后续事件带回）
        this._turnAssistant = null;
        this._turnAssistantTs = "";
        this._showThinking();
        break;
      case "turn.finished":
        this._hideThinking();
        this._finalizeStream();
        this._finalizeReasoning();
        this._attachTurnActions();
        break;
      case "model.text_delta":
        this._hideThinking();
        // 记录首个 delta 的事件时间，供草稿收笔时注入消息时间提示
        if (!this._pendingStreamTs && evt.time) this._pendingStreamTs = evt.time;
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
          this._turnAssistant = assistantBlock(p.text);
          this._turnAssistantTs = evt.time || "";
          this._append(this._turnAssistant);
        } else {
          this._finalizeStream();
        }
        this._finalizeReasoning();
        break;
      case "model.request_failed": {
        this._hideThinking();
        const detail = (p.message || "").slice(0, 300);
        this._append(noticeBlock(`model request failed (${p.stage || "unknown"}): ${p.code || ""}${detail ? " — " + detail : ""}`, true));
        break;
      }
      case "tool.prepared": {
        this._hideThinking();
        // 实时事件只带有界 preview；完整输出经 io.fetchToolOutput 按需取
        const tb = toolBlock(p, {
          onCopy: this.io.fetchToolOutput
            ? () => this.io.fetchToolOutput(p.call_id)
            : null,
          resolveArtifactURL: this.io.fetchArtifactURL,
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
        if (p.text) this._append(userBlock(p.text, evt.time || ""));
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
        this._attachTurnActions();
        break;
      case "context.compacted":
        this._append(compactBlock(p));
        break;
      case "budget.notice":
        // 后端已生成具体文案（梯度提醒 / 软着陆），直接展示
        this._append(noticeBlock(p.text || "budget notice", true));
        break;
      case "runtime.warning":
        this._append(noticeBlock(p.message || "runtime warning", true));
        break;
      case "runtime.fatal":
        this._hideThinking();
        this._append(fatalBlock(p.message || "runtime fatal"));
        this._attachTurnActions();
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
      // 新草稿即本轮最新 assistant 块；操作行留待轮终止时统一挂载
      this.stream = streamBlock();
      this._turnAssistant = this.stream.el;
      this._turnAssistantTs = this._pendingStreamTs;
      this._pendingStreamTs = "";
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
    this._pendingStreamTs = "";
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
    this._pendingStreamTs = "";
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
          // binding_mismatch / not_idle 都意味着该审批已被处理或已过期
          // （例如同域名的重复申请已被记住的规则自动放行），静默收起即可
          if (e.code === "binding_mismatch" || e.code === "not_idle") {
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
