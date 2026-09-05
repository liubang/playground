-- history.lua — snapshot.messages 全量重放（attach / resync）。
--
-- 渲染纪律（避免与 live 事件流双写）：
--   只在快照状态为 idle 时重放。idle 快照的水位恰好是 turn 边界，SSE 以
--   after=event_seq 续传不会重播这些消息对应的事件，因此历史块与未来的
--   turn.started/user/run 块严格衔接、不重复。running 态（turn 进行中
--   挂接）走原来"占位提示 + live 事件流"的路径，不重放历史。
local blocks = require("loom.ui.blocks")

local M = {}

local function text_of(parts)
  local acc = {}
  for _, p in ipairs(parts or {}) do
    if p.kind == "text" and p.text then
      acc[#acc + 1] = p.text
    end
  end
  return table.concat(acc, "")
end

--- 返回 true 表示渲染了历史；false 表示不适用（空历史或非 idle）。
function M.render(snap, state_name)
  local msgs = snap and snap.messages or nil
  if not msgs or #msgs == 0 or (state_name or snap.state or "idle") ~= "idle" then
    return false
  end
  local transcript = require("loom.ui.transcript")
  -- 清理欢迎页（历史与欢迎互斥），然后按 domain 顺序重放
  transcript.clear_kind("welcome")
  local n = 0
  for _, m in ipairs(msgs) do
    if m.role == "user" then
      local t = text_of(m.parts)
      if t ~= "" then
        transcript.add_user_block(t)
        n = n + 1
      end
    elseif m.role == "assistant" then
      local lines, deco = blocks.history_assistant(m)
      if #lines > 0 then
        transcript.add_block(lines, "history", deco)
        n = n + 1
      end
    end
    -- system role 不入 transcript（系统提示不属于会话呈现面）
  end
  transcript.add_system_block(("以上为 %d 条历史消息"):format(n))
  return true
end

return M
