-- sessions.lua — 会话列表 picker（vim.ui.select；fzf-lua/telescope 可自动接管）。
local api = require("loom.api")
local util = require("loom.util")

local M = {}

function M.pick()
  api.list_sessions(50, function(err, data)
    if err then
      util.notify("获取会话列表失败: " .. err, vim.log.levels.ERROR)
      return
    end
    local items = (data and data.sessions) or {}
    if #items == 0 then
      util.notify("没有历史会话")
      return
    end
    vim.schedule(function()
      vim.ui.select(items, {
        prompt = "Loom 会话（回车恢复）",
        format_item = function(s)
          return ("%s · %s · %s · %s · turns=%d"):format(
            util.short_id(s.id),
            s.title or "(untitled)",
            tostring(s.state or "?"),
            util.time_ago(s.updated_at),
            s.turn_count or 0
          )
        end,
      }, function(choice)
        if choice then
          require("loom").attach(choice.id)
        end
      end)
    end)
  end)
end

return M
