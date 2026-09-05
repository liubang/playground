-- composer.lua — 底部输入区：prompt buffer（❯ 提示符，<CR> 发送）+ @ 文件提及。
--
-- @ 提及（avante 语义落地）：
--   输入 `@` 后接任一字符起补全（TextChangedI 驱动，search API + 代际号防乱序）；
--   提交时扫描 `@path` token，逐个走 /file 读取，把内容以 ```ft fence 附在 prompt
--   之后——原文里的 @path 原样保留（用户消息仍是可读的形式），上下文展开
--   由客户端完成，server 不感知提及语义。
local api = require("loom.api")
local state = require("loom.state")
local util = require("loom.util")

local M = { buf = nil, win = nil }

local aug = vim.api.nvim_create_augroup("LoomComposer", { clear = true })
local complete_gen = 0 -- 补全代际号：异步回到时的过期判决

-- ---------------------------------------------------------------------------
-- @ 提及：解析与补全
-- ---------------------------------------------------------------------------

--- 光标处的 @token：返回 token头（含@）、query、@ 所在列（1-based）。
--- 只取光标左侧 "@" 之后连续的 [word._/-]+ 段；不在 @ 后的上下文中返回 nil。
local function token_at_cursor(line, col)
  local head = line:sub(1, col)
  local at = head:reverse():find("@", 1, true)
  if not at then
    return nil
  end
  local atpos = #head - at + 1 -- '@' 在 head 中的位置
  if atpos > 1 and head:sub(atpos - 1, atpos - 1):match("[%w%._%-/]") then
    return nil -- 紧前是字母/路径字符，形如 pwd@host / file@2x.png 不算提及
  end
  local query = head:sub(atpos + 1)
  if not query:match("^[%w%._%-/]*$") then
    return nil
  end
  return atpos, query -- '@' 列(1-based 字节), query
end

--- TextChangedI：有 @token 且 query 非空 → 后端搜索 → vim.fn.complete。
local function maybe_complete()
  if not state.workspace_id then
    return
  end
  local line = vim.api.nvim_get_current_line()
  local col = vim.api.nvim_win_get_cursor(0)[2]
  local atpos, query = token_at_cursor(line, col)
  if not atpos or query == "" then
    return
  end
  complete_gen = complete_gen + 1
  local gen = complete_gen
  api.workspace_file_search(state.workspace_id, query, function(err, data)
    if err or not data or gen ~= complete_gen then
      return
    end
    local items = {}
    for _, m in ipairs(data.matches or {}) do
      if m.kind == "file" then
        items[#items + 1] = {
          word = m.path,
          abbr = "@" .. m.path,
          menu = m.truncated and "·" or "",
        }
      end
    end
    if #items > 0 and vim.fn.mode():find("i") then
      -- 起点 = '@' 后第一个 query 字符处（col 0-based）
      local start_col = atpos + 1 -- 1-based byte of query
      vim.fn.complete(start_col - 1, items)
    end
  end)
end

--- 扫描 prompt 中的 @token 列表（去重保序）："@a @b" → {"a","b"}
local function collect_mentions(text)
  local seen, out = {}, {}
  for tok in text:gmatch("@([%w%._%-/]+)") do
    if not seen[tok] then
      seen[tok] = true
      out[#out + 1] = tok
    end
  end
  return out
end

-- ---------------------------------------------------------------------------
-- 基础收发
-- ---------------------------------------------------------------------------

function M.bind(win)
  if not (M.buf and vim.api.nvim_buf_is_valid(M.buf)) then
    M.buf = vim.api.nvim_create_buf(false, true)
    vim.bo[M.buf].buftype = "prompt"
    vim.bo[M.buf].bufhidden = "hide"
    vim.bo[M.buf].swapfile = false
    vim.bo[M.buf].filetype = "loom-composer"
    vim.fn.prompt_setprompt(M.buf, "❯ ")
    vim.fn.prompt_setcallback(M.buf, function(text)
      vim.schedule(function()
        M.submit_text(text)
      end)
    end)
    vim.api.nvim_create_autocmd("TextChangedI", {
      group = aug,
      buffer = M.buf,
      callback = maybe_complete,
    })
  end
  M.win = win
  vim.api.nvim_win_set_buf(win, M.buf)
  vim.wo[win].winbar = "%#LoomDim# loom · <CR> 发送 · @ 提及文件 · <C-c> 取消 · <Esc> 回 transcript %*"
  vim.wo[win].number = false
  vim.wo[win].relativenumber = false
  vim.wo[win].signcolumn = "no"
  vim.keymap.set("n", "<Esc>", function()
    require("loom.ui").focus_transcript()
  end, { buffer = M.buf, desc = "Loom: 切回 transcript" })
  vim.keymap.set("n", "<C-c>", function()
    require("loom").cancel()
  end, { buffer = M.buf, desc = "Loom: 取消当前 turn" })
end

--- 展开 @提及 → prompt + 末尾追加「引用文件」段（错误忽略该条，仅保留占位）。
--- 全部读完后回调 done(expanded)。
local function expand_mentions(text, done)
  local tokens = state.workspace_id and collect_mentions(text) or {}
  if #tokens == 0 then
    done(text)
    return
  end
  local sections = {}
  local idx = 0
  local function next_one()
    idx = idx + 1
    local path = tokens[idx]
    if not path then
      if #sections > 0 then
        done(text .. "\n\n---\n引用文件：\n" .. table.concat(sections, "\n"))
      else
        done(text)
      end
      return
    end
    api.workspace_file_read(state.workspace_id, path, function(err, data)
      if not err and data and not data.binary and data.content then
        local ft = vim.filetype.match({ filename = path }) or ""
        sections[#sections + 1] = ("`%s`:\n```%s\n%s%s\n```"):format(
          path,
          ft,
          data.content,
          data.truncated and "\n…（已截断）" or ""
        )
      end
      next_one()
    end)
  end
  next_one()
end

function M.submit_text(text)
  text = util.trim(text or "")
  if text == "" then
    return
  end
  if not state.session_id then
    util.notify("尚未挂载会话", vim.log.levels.WARN)
    return
  end
  -- prompt buffer 提交后历史行会留在 buffer 里（"❯ 上一条" 堆叠）；
  -- 发送后整清，保持输入条干净。
  vim.api.nvim_buf_set_lines(M.buf, 0, -1, false, { "" })
  vim.cmd("startinsert")
  expand_mentions(text, function(expanded)
    api.submit_prompt(state.session_id, expanded, function(err, data)
      if err then
        util.notify("提交失败: " .. err, vim.log.levels.ERROR)
        return
      end
      if data and data.steered then
        util.notify(("Turn 忙，消息已入队（queue_len=%d）"):format(data.queue_len or 0))
      end
      -- 用户消息块由 turn.started 事件权威渲染（SERVE_DESIGN §4.3-5）。
    end)
  end)
end

return M
