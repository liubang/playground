-- init.lua — loom.nvim 主入口：setup / attach 流程 / 会话与 SSE 生命周期。
--
-- 使用：
--   require("loom").setup({ ... })
--   :Loom            打开面板并挂载（首次自动新建会话）
--   :LoomSessions    恢复历史会话
local api = require("loom.api")
local config = require("loom.config")
local context = require("loom.context")
local events = require("loom.events")
local server = require("loom.server")
local spinner = require("loom.ui.spinner")
local sse = require("loom.sse")
local state = require("loom.state")
local transcript = require("loom.ui.transcript")
local ui = require("loom.ui")
local util = require("loom.util")

local M = {}
local sse_handle = nil
local pending_send = nil -- attach 完成前暂存的待发 prompt（:LoomSend 在会话未建立时）

function M.setup(opts)
  config.setup(opts)
end

local function close_sse()
  if sse_handle then
    sse_handle.close()
    sse_handle = nil
  end
end

local function connect_sse()
  local cfg = config.get()
  local token, terr = server.token()
  if not token then
    util.notify(terr, vim.log.levels.ERROR)
    return
  end
  sse_handle = sse.connect({
    url = cfg.server.url,
    token = token,
    session_id = state.session_id,
    after = state.event_seq,
    handlers = {
      on_event = function(evt)
        events.dispatch(evt)
      end,
      on_resync = function(kind, reason)
        M.resync(kind, reason)
      end,
      on_error = function(msg)
        util.notify("SSE: " .. msg, vim.log.levels.WARN)
      end,
    },
  })
end

--- instance 变化 / server.resync 之后的恢复路径：
--- 重新取 snapshot → 清空动态内容 → 重建 pending 卡片 → 新 cursor 重挂 SSE。
--- 触发条件只剩两类（都是真实事件，不常见）：
---   - instance：服务端进程重启（全局序号清零，旧 cursor 失效）
---   - cursor_invalid：replay 窗口被轮转/跨进程重启后命中
--- sequence 连续性（gap）判定在服务端，客户端不做（见 sse.lua 注释）。
local RESYNC_HINT = {
  cursor_invalid = "会话事件游标失效——loomd 重启或事件窗口已轮转",
  shutdown = "loomd 正在关闭",
  ["server signaled"] = "服务端指令",
}
function M.resync(kind, reason)
  if not state.session_id then
    return
  end
  spinner.stop() -- 丢弃上个 turn 的动画，重建期间避免渲染竞态
  require("loom.ui.diffview").hide_all() -- diff 视图是易失态，resync 时全清
  util.notify(("重新同步：%s"):format(RESYNC_HINT[reason] or reason or "服务端指令"))
  api.snapshot(state.session_id, function(err, snap)
    if err then
      util.notify("resync 失败: " .. err, vim.log.levels.ERROR)
      return
    end
    state.apply_snapshot(snap)
    transcript.clear_all()
    transcript.set_status(snap.state or "idle", snap.model_name)
    transcript.set_usage(snap.occupancy, snap.context_window)
    events.rebuild_from_snapshot()
    require("loom.history").render(snap, snap.state) -- idle 才重放（running 靠 live 流）
    connect_sse()
  end)
end

--- 挂载到会话。resume 为 session id 恢复，nil 新建。主流程见 NEOVIM_UI_DESIGN.md §4.1。
function M.attach(resume)
  ui.open()
  server.start(function(err)
    if err then
      util.notify(err, vim.log.levels.ERROR)
      return
    end
    api.version(function(verr, v)
      if verr or not v then
        util.notify("协议协商失败: " .. (verr or "无响应"), vim.log.levels.ERROR)
        return
      end
      if v.protocol ~= 1 then
        util.notify(("不支持的协议版本: %s"):format(tostring(v.protocol)), vim.log.levels.ERROR)
        return
      end
      api.create_session(resume, function(cerr, sess)
        if cerr or not sess then
          util.notify("创建/恢复会话失败: " .. (cerr or "空响应"), vim.log.levels.ERROR)
          return
        end
        close_sse()
        spinner.stop()
        state.reset()
        state.session_id = sess.session_id or sess.id
        state.workspace_id = sess.workspace_id
        api.snapshot(state.session_id, function(serr, snap)
          if serr or not snap then
            util.notify("获取快照失败: " .. (serr or "空响应"), vim.log.levels.ERROR)
            return
          end
          state.apply_snapshot(snap)
          transcript.clear_all()
          transcript.set_status(snap.state or "idle", snap.model_name)
          transcript.set_usage(snap.occupancy, snap.context_window)
          events.rebuild_from_snapshot()
          -- idle 快照 = turn 边界：历史一次性重放；running 态留给 live 事件流补齐。
          if not require("loom.history").render(snap, snap.state) and (snap.turn_count or 0) > 0 then
            transcript.add_system_block(
              ("已挂载会话 %s（%d turns，进行中）"):format(util.short_id(state.session_id), snap.turn_count)
            )
          end
          connect_sse()
          if pending_send then
            local text = pending_send
            pending_send = nil
            require("loom.ui.composer").submit_text(text)
          end
          ui.focus_composer()
        end)
      end)
    end)
  end)
end

function M.open()
  if state.session_id then
    ui.open()
  else
    M.attach(nil)
  end
end

function M.new_session()
  M.attach(nil)
end

function M.cancel()
  if not state.session_id then
    return
  end
  api.cancel(state.session_id, function(err)
    if err then
      util.notify("取消失败: " .. err, vim.log.levels.WARN)
    end
  end)
end

--- 发送 prompt；has_range 时把命令行范围（visual `:'<,'>`）组装成上下文。
--- 用法：`:'<,'>LoomSend 看下这段为什么错` 或 `:LoomSend 你好`
function M.send_selection(msg, has_range, l1, l2)
  ui.open()
  local text = msg or ""
  if has_range and l1 and l2 and l2 >= l1 then
    text = context.format(context.range(vim.api.nvim_get_current_buf(), l1, l2), msg)
  end
  if text == "" then
    ui.focus_composer()
    return
  end
  if state.session_id then
    require("loom.ui.composer").submit_text(text)
  else
    pending_send = text
    M.attach(nil)
  end
end

vim.api.nvim_create_autocmd("VimLeavePre", {
  group = vim.api.nvim_create_augroup("LoomServerLifecycle", { clear = true }),
  callback = function()
    close_sse()
    require("loom.ui.diffview").hide_all()
    if not config.get().server.keep_alive then
      server.stop()
    end
  end,
})

return M
