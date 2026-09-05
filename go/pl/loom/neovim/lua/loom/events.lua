-- events.lua — RuntimeEvent kind → state/ui 动作（分发层）。
--
-- 纪律：
--   - 本层只做"事件 → 状态机变更 → 渲染触发"的编排；不出现任何渲染细节
--     （样式在 blocks，block 管理在 transcript，动画在 spinner）。
--   - 未知 kind 一律忽略（wire 协议契约：服务器可平滑新增事件）。
--   - 生命周期事件（turn.started/finished、run.cancelled、runtime.fatal）
--     是 spinner 的唯一起停点 —— 它们在哪个分支停，都收敛进 finish()。
local spinner = require("loom.ui.spinner")
local state = require("loom.state")
local util = require("loom.util")

-- 延迟 require，避免加载环（transcript→blocks/spinner，approval→transcript）。
local function transcript()
  return require("loom.ui.transcript")
end
local function approval_mod()
  return require("loom.ui.approval")
end

local M = {}

-- ---------------------------------------------------------------------------
-- run 生命周期助手
-- ---------------------------------------------------------------------------

local function ensure_run_block()
  local run, is_new = state.ensure_run()
  if is_new then
    transcript().new_run_block(run)
  end
  return run
end

--- turn/range 结束的统一收敛：停动画 → 终态渲染 → 状态收尾。
local function finish(err, status)
  spinner.stop()
  local run = state.finish_run(err)
  if run then
    transcript().render_run(run)
  end
  if status then
    transcript().set_status(status)
  end
end

-- ---------------------------------------------------------------------------
-- kind 分发表
-- ---------------------------------------------------------------------------

local handlers = {}

handlers["turn.started"] = function(evt)
  state.finish_run()
  local p = evt.payload or {}
  transcript().clear_kind("welcome")
  transcript().set_status("running")
  transcript().add_user_block(p.prompt or "")
  local run = state.ensure_run()
  transcript().new_run_block(run)
  -- spinner tick 即重绘信号；渲染时动画状态经 spinner.current() 采样。
  spinner.start(function()
    if state.run then
      transcript().render_run(state.run)
    end
  end)
end

handlers["model.text_delta"] = function(evt)
  local run = ensure_run_block()
  run.text = run.text .. ((evt.payload or {}).delta or "")
  transcript().render_run(run)
end

handlers["model.reasoning_delta"] = function(evt)
  local run = ensure_run_block()
  run.reasoning = run.reasoning .. ((evt.payload or {}).delta or "")
  transcript().render_run(run)
end

handlers["model.response_completed"] = function(evt)
  local run = ensure_run_block()
  local text = (evt.payload or {}).text
  if text then
    run.text = text -- canonical 文本校正 lossy delta 草稿（SERVE_DESIGN §5.4）
  end
  transcript().render_run(run)
end

handlers["model.request_failed"] = function(evt)
  local p = evt.payload or {}
  finish(p.message or "model request failed", "idle")
  util.notify("模型请求失败: " .. (p.message or "?"), vim.log.levels.ERROR)
end

handlers["model.request_retrying"] = function(evt)
  local p = evt.payload or {}
  transcript().add_system_block(
    ("模型请求重试：attempt %d/%d，等待 %dms（%s）"):format(
      p.attempt or 0,
      p.max_attempts or 0,
      p.wait_ms or 0,
      p.code or "?"
    )
  )
end

handlers["tool.prepared"] = function(evt)
  local p = evt.payload or {}
  if not p.call_id then
    return
  end
  local run = ensure_run_block()
  run.tools[p.call_id] = {
    name = p.tool_name,
    target = p.target,
    diff = p.diff,
    status = "prepared",
  }
  -- write/edit：执行前快照旧文本（此时盘上尚未改动），供完成后铺编辑器内 diff。
  if (p.tool_name == "write" or p.tool_name == "edit") and p.target then
    local dv = require("loom.ui.diffview")
    local path = dv.resolve_path(p.target)
    if path then
      run.tools[p.call_id].old_path = path
      run.tools[p.call_id].old_lines = dv.read_old(path)
    end
  end
  run.tool_order[#run.tool_order + 1] = p.call_id
  transcript().render_run(run)
end

handlers["tool.started"] = function(evt)
  local p = evt.payload or {}
  local run = state.run
  if run and p.call_id and run.tools[p.call_id] then
    run.tools[p.call_id].status = "running"
    transcript().render_run(run)
  end
end

handlers["tool.completed"] = function(evt)
  local p = evt.payload or {}
  local run = state.run
  if not (run and p.call_id and run.tools[p.call_id]) then
    return
  end
  local t = run.tools[p.call_id]
  t.status = p.status or "success"
  t.duration_ms = p.duration_ms
  t.preview = p.preview
  t.error = p.error_message or p.error
  transcript().render_run(run)
  -- write/edit 成功：编辑器文件区铺 before/after diff（自动批准路径的主要可见反馈）。
  if t.status == "success" and t.old_path and require("loom.config").get().ui.diff_after_edit then
    require("loom.ui.diffview").show_completed(t.old_path, t.old_lines)
  end
end

handlers["approval.requested"] = function(evt)
  local p = evt.payload or {}
  if not p.approval_id then
    return
  end
  state.pending_approvals[p.approval_id] = p
  approval_mod().requested(p)
  -- file-editing 工具：编辑器内铺真实 diff（write/edit 以外工具 no-op）
  require("loom.ui.diffview").show(p)
end

handlers["approval.resolved"] = function(evt)
  local p = evt.payload or {}
  if p.approval_id then
    state.pending_approvals[p.approval_id] = nil
    approval_mod().resolved(p)
    require("loom.ui.diffview").hide(p.approval_id)
  end
end

handlers["question.asked"] = function(evt)
  local p = evt.payload or {}
  local qid = p.question_id or p.id
  if not qid then
    return
  end
  state.pending_questions[qid] = p
  approval_mod().question(p)
end

handlers["question.answered"] = function(evt)
  local p = evt.payload or {}
  local qid = p.question_id or p.id
  if qid then
    state.pending_questions[qid] = nil
  end
end

handlers["steer.queued"] = function(evt)
  local p = evt.payload or {}
  util.notify("消息已入队（steer）: " .. (p.text or p.prompt or ""))
end

handlers["steer.injected"] = function(evt)
  local p = evt.payload or {}
  transcript().add_system_block("steer 已注入: " .. (p.text or ""))
end

handlers["run.cancel_requested"] = function()
  transcript().add_system_block("已请求取消…")
end

handlers["run.cancelled"] = function()
  finish("已取消", "idle")
end

handlers["turn.finished"] = function(evt)
  local p = evt.payload or {}
  finish(p.error, "idle")
end

handlers["context.compacted"] = function(evt)
  local p = evt.payload or {}
  transcript().add_system_block(
    ("context compacted: %d → %d tokens"):format(p.est_tokens_before or 0, p.est_tokens_after or 0)
  )
end

handlers["budget.notice"] = function(evt)
  local p = evt.payload or {}
  if p.text then
    util.notify(p.text, vim.log.levels.WARN)
  end
end

handlers["runtime.warning"] = function(evt)
  local p = evt.payload or {}
  transcript().add_system_block("warning: " .. (p.message or "?"))
end

handlers["runtime.fatal"] = function(evt)
  local p = evt.payload or {}
  transcript().add_system_block("fatal: " .. (p.message or "?"))
  finish(p.message, "fatal")
  util.notify("runtime fatal: " .. (p.message or "?"), vim.log.levels.ERROR)
end

-- ---------------------------------------------------------------------------
-- 入口
-- ---------------------------------------------------------------------------

function M.dispatch(evt)
  local h = evt and evt.kind and handlers[evt.kind]
  if h then
    local ok, err = pcall(h, evt)
    if not ok then
      util.notify(("事件处理失败（%s）: %s"):format(evt.kind, err), vim.log.levels.ERROR)
    end
  end
end

--- attach/resync 之后：根据 snapshot.pending_requests 重建 pending 卡片。
function M.rebuild_from_snapshot()
  for _, p in pairs(state.pending_approvals) do
    approval_mod().requested(p)
  end
  for _, q in pairs(state.pending_questions) do
    vim.schedule(function()
      approval_mod().question(q)
    end)
  end
end

return M
