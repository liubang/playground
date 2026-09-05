-- approval.lua — 审批卡与问答决议（UX 层；内容渲染在 blocks.approval_*）。
local api = require("loom.api")
local state = require("loom.state")
local util = require("loom.util")

local function transcript()
  return require("loom.ui.transcript")
end

local M = { active = {} } -- approval_id → { block, payload }

--- approval.requested → 追加审批卡。
function M.requested(p)
  M.active[p.approval_id] = { block = transcript().add_approval_block(p), payload = p }
  util.notify(("待审批: %s %s"):format(p.tool_name or "?", p.target or ""), vim.log.levels.WARN)
  transcript().set_status("awaiting_approval")
end

--- approval.resolved → 卡片收敛（本端或其他端决议的广播）。
--- 注：resolved payload 不带 tool_name（events.ts ApprovalResolvedPayload），从请求阶段回放。
function M.resolved(p)
  local entry = M.active[p.approval_id]
  M.active[p.approval_id] = nil
  state.pending_approvals[p.approval_id] = nil
  if entry then
    local tool = p.tool_name or (entry.payload and entry.payload.tool_name) or "?"
    transcript().replace_with_resolved(entry.block, tool, p.decision)
  end
  -- 若还有挂起的审批保持 awaiting_approval，否则回到 running。
  transcript().set_status(next(state.pending_approvals) ~= nil and "awaiting_approval" or "running")
end

--- 决议最新挂起审批（transcript buffer-local y/n 绑定到这里）。
--- @param decision "allow" | "deny"
function M.resolve_latest(decision)
  local latest = state.latest_approval()
  if not latest then
    util.notify("没有挂起的审批", vim.log.levels.WARN)
    return
  end
  local p = latest.payload
  api.resolve_approval(state.session_id, p.approval_id, {
    call_id = p.call_id,
    args_hash = p.args_hash,
    decision = decision,
    client = "nvim",
  }, function(err)
    if err then
      util.notify("审批决议失败: " .. err, vim.log.levels.ERROR)
    end
  end)
end

--- question.asked → vim.ui.select（Phase 2 再做专用卡片）。
function M.question(p)
  local qid = p.question_id or p.id
  local opts = {}
  for _, o in ipairs(p.options or {}) do
    opts[#opts + 1] = o.label
  end
  vim.ui.select(opts, { prompt = p.text or "agent 提问" }, function(choice)
    local payload = choice and { selected = { choice } } or { skipped = true }
    api.answer_question(state.session_id, qid, payload, function(err)
      if err then
        util.notify("回答问题失败: " .. err, vim.log.levels.ERROR)
      else
        state.pending_questions[qid] = nil
      end
    end)
  end)
end

return M
