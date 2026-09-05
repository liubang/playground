-- state.lua — 客户端会话状态（纯数据；事件作用在 events.lua）。
local M = {}

M.session_id = nil
M.snapshot = nil
--- 当前 turn 渲染草稿：
--- { text, reasoning, tools = { call_id → tool }, tool_order = {call_id...}, finished, error }
M.run = nil
M.pending_approvals = {} -- approval_id → ApprovalRequestedPayload
M.pending_questions = {} -- question_id → QuestionPayload
M.event_seq = 0 -- snapshot 水位（首屏 SSE cursor）
M.workspace_id = nil -- POST /v1/sessions 响应携带（workspace API 的前哨）
M.workspace_root = nil -- snapshot.workspace_root（相对路径解析兜底）

function M.reset()
  M.session_id = nil
  M.snapshot = nil
  M.run = nil
  M.pending_approvals = {}
  M.pending_questions = {}
  M.event_seq = 0
  M.workspace_id = nil
  M.workspace_root = nil
end

--- 获取当前 run；若上一个 run 已结束则新开一个。
--- 返回 run, is_new（UI 据此创建新 run block）。
function M.ensure_run()
  if not M.run or M.run.finished then
    M.run = {
      text = "",
      reasoning = "",
      tools = {},
      tool_order = {},
      finished = false,
      error = nil,
    }
    return M.run, true
  end
  return M.run, false
end

function M.finish_run(err)
  local run = M.run
  if not run then
    return nil
  end
  run.finished = true
  run.error = err or run.error
  return run
end

--- 从 snapshot 恢复水位与挂起请求（首屏 / resync 共用）。
function M.apply_snapshot(snap)
  M.snapshot = snap
  M.event_seq = snap.event_seq or 0
  if snap.workspace_root and snap.workspace_root ~= "" then
    M.workspace_root = snap.workspace_root
  end
  M.pending_approvals = {}
  M.pending_questions = {}
  for _, req in ipairs(snap.pending_requests or {}) do
    if req.kind == "approval" and req.approval then
      M.pending_approvals[req.approval.approval_id or req.id] = req.approval
    elseif req.kind == "question" and req.question then
      local q = req.question
      M.pending_questions[q.question_id or q.id or req.id] = q
    end
  end
end

--- 最新挂起审批（buffer-local y/n 作用于它；同时挂起极罕见）。
function M.latest_approval()
  for id, p in pairs(M.pending_approvals) do
    return { id = id, payload = p }
  end
  return nil
end

return M
