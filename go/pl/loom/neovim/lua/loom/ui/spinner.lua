-- spinner.lua — 流式等待指示器：uv timer 驱动的帧动画 + 已耗时。
--
-- 设计：动画状态（帧/耗时）对渲染层只读（current()），驱动端只收到
-- "该重绘了" 的 tick 回调 —— spinner 不持有任何业务状态，渲染层也不关心
-- 定时器细节，两边解耦。
local uv = vim.uv or vim.loop

local FRAMES = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" }
local INTERVAL_MS = 100

local M = {
  timer = nil, ---@type userdata|nil
  started_at = 0, ---@type integer hrtime（ns）
  ticks = 0, ---@type integer 已走的帧数
}

--- 秒数 → 紧凑耗时串（"3.2s" / "1:05"）。
local function elapsed_text(secs)
  if secs < 60 then
    return ("%.1fs"):format(secs)
  end
  return ("%d:%02d"):format(math.floor(secs / 60), math.floor(secs) % 60)
end

--- 当前动画状态。未运行返回 nil。
--- @return {frame: string, elapsed: string}|nil
function M.current()
  if not M.timer then
    return nil
  end
  local secs = (uv.hrtime() - M.started_at) / 1e9
  return { frame = FRAMES[(M.ticks % #FRAMES) + 1], elapsed = elapsed_text(secs) }
end

--- 启动动画。on_tick() 每 INTERVAL_MS 在主循环调用一次（用于触发重绘）。
--- 重复调用先停旧 timer。
function M.start(on_tick)
  M.stop()
  M.started_at = uv.hrtime()
  M.ticks = 0
  M.timer = assert(uv.new_timer())
  M.timer:start(
    INTERVAL_MS,
    INTERVAL_MS,
    vim.schedule_wrap(function()
      if not M.timer then
        return
      end
      M.ticks = M.ticks + 1
      on_tick()
    end)
  )
end

function M.stop()
  if M.timer then
    M.timer:stop()
    M.timer:close()
    M.timer = nil
  end
end

--- @return boolean
function M.active()
  return M.timer ~= nil
end

return M
