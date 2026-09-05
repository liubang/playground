-- transcript.lua — Transcript 缓冲与 block 管理（通用层；不管内容怎么画）。
--
-- 职责边界：
--   - 只管 buffer 生命周期、block 行区间（extmark）、行级 deco 铺/清、跟随滚动、
--     winbar 状态。内容渲染全部委托 blocks.lua（纯函数）。
--   - 两个 namespace：ns（block 区间定位）与 ns_deco（行高亮），重渲染时区间级清重。
--   - 所有 buffer 写操作经由 with_modifiable 单点收口。
local util = require("loom.util")

require("loom.ui.hl").setup()

local blocks = require("loom.ui.blocks")
local spinner = require("loom.ui.spinner")

local ns = vim.api.nvim_create_namespace("loom_transcript")
local ns_deco = vim.api.nvim_create_namespace("loom_transcript_deco")

local STATUS_ICONS = require("loom.ui.hl").icons.status

---@class LoomBlock
---@field id integer  extmark id（ns）
---@field kind string user | run | approval | system | misc

local M = {
  buf = nil, ---@type integer|nil
  blocks = {}, ---@type LoomBlock[]
  run_block = nil, ---@type LoomBlock|nil
  status = "idle", ---@type string
  model = nil, ---@type string|nil
}

-- =========================================================== buffer 基础 ==

local function valid()
  return M.buf ~= nil and vim.api.nvim_buf_is_valid(M.buf)
end

local function winbar_text()
  local model = M.model and (" · " .. M.model) or ""
  local usage = M.usage and (" · " .. M.usage) or ""
  return ("%%#LoomAssistantBar# %s loom · %s%s%s %%*"):format(STATUS_ICONS[M.status] or "●", M.status, model, usage)
end

local function refresh_winbars()
  for _, win in ipairs(vim.fn.win_findbuf(M.buf)) do
    vim.wo[win].winbar = winbar_text()
  end
end

--- 更新会话状态到 winbar。status 非 nil 才替换；model 非 nil 才替换。
function M.set_status(status, model)
  if status then
    M.status = status
  end
  if model then
    M.model = model
  end
  if valid() then
    refresh_winbars()
  end
end

--- winbar 追加上下文占用（snapshot.occupancy / context_window），如 `ctx 12.3k/200k`。
--- occupancy 非正数则清除。
function M.set_usage(occupancy, limit)
  M.usage = nil
  if type(occupancy) == "number" and occupancy > 0 then
    local function fmt(x)
      if x >= 1000 then
        local v = x / 1000
        return (v % 1 == 0) and string.format("%dk", v) or string.format("%.1fk", v)
      end
      return tostring(x)
    end
    M.usage = ("ctx %s%s"):format(fmt(occupancy), limit and ("/" .. fmt(limit)) or "")
  end
  if valid() then
    refresh_winbars()
  end
end

--- 绑定到既有窗口（由 ui/init 创建布局后调用）。
function M.bind(win)
  if not valid() then
    M.buf = vim.api.nvim_create_buf(false, true)
    vim.bo[M.buf].buftype = "nofile"
    vim.bo[M.buf].bufhidden = "hide"
    vim.bo[M.buf].swapfile = false
    vim.bo[M.buf].filetype = "markdown"
    vim.bo[M.buf].modifiable = false
  end
  vim.api.nvim_win_set_buf(win, M.buf)
  vim.wo[win].winbar = winbar_text()
  vim.wo[win].number = false
  vim.wo[win].relativenumber = false
  vim.wo[win].wrap = true
  vim.wo[win].signcolumn = "no"
  -- markdown 语法标记符（**、```）隐藏，渲染成熟悉的可视样式
  vim.wo[win].conceallevel = 2
  vim.wo[win].concealcursor = "nc"
  vim.wo[win].foldenable = false
  -- 首次打开展示欢迎页；首个 turn.started 会经 clear_kind("welcome") 清理
  if #M.blocks == 0 and vim.api.nvim_buf_line_count(M.buf) == 1
    and vim.api.nvim_buf_get_lines(M.buf, 0, 1, false)[1] == "" then
    local lines, deco = blocks.welcome()
    M.add_block(lines, "welcome", deco)
  end
  return M.buf
end

-- 跟随滚动：渲染前光标处于靠近底部的窗口，渲染后继续吸底。
local function record_follow()
  local follow = {}
  local total = vim.api.nvim_buf_line_count(M.buf)
  for _, win in ipairs(vim.fn.win_findbuf(M.buf)) do
    follow[win] = vim.api.nvim_win_get_cursor(win)[1] >= total - 1
  end
  return follow
end

local function apply_follow(follow)
  local total = vim.api.nvim_buf_line_count(M.buf)
  for win, yes in pairs(follow) do
    if yes and vim.api.nvim_win_is_valid(win) then
      pcall(vim.api.nvim_win_set_cursor, win, { total, 0 })
    end
  end
end

--- buffer 写操作收口：buffer 常态 modifiable=false，仅在这里短暂置真。
local function with_modifiable(fn)
  vim.bo[M.buf].modifiable = true
  local ok, err = pcall(fn)
  vim.bo[M.buf].modifiable = false
  if not ok then
    util.notify("transcript 渲染失败: " .. tostring(err), vim.log.levels.ERROR)
  end
end

--- 在 [row, row+count) 区间上重铺 deco（行级 line_hl_group；区间级 hl_group）。
local function apply_deco(row, count, deco)
  vim.api.nvim_buf_clear_namespace(M.buf, ns_deco, row, row + count)
  for _, d in ipairs(deco or {}) do
    if d.col0 then
      pcall(vim.api.nvim_buf_set_extmark, M.buf, ns_deco, row + d.row, d.col0, {
        end_col = d.col1,
        hl_group = d.group,
      })
    else
      pcall(vim.api.nvim_buf_set_extmark, M.buf, ns_deco, row + d.row, 0, {
        line_hl_group = d.group,
        hl_eol = d.eol or false,
      })
    end
  end
end

-- ============================================================= block 管理 ==

--- 在 buffer 末尾追加一个 block。@return LoomBlock|nil
function M.add_block(lines, kind, deco)
  if not valid() or #lines == 0 then
    return nil
  end
  local block
  with_modifiable(function()
    local follow = record_follow()
    local start = vim.api.nvim_buf_line_count(M.buf)
    vim.api.nvim_buf_set_lines(M.buf, start, start, false, lines)
    block = {
      id = vim.api.nvim_buf_set_extmark(M.buf, ns, start, 0, {
        end_row = start + #lines,
        right_gravity = false,
        end_right_gravity = false,
      }),
      kind = kind or "misc",
    }
    M.blocks[#M.blocks + 1] = block
    apply_deco(start, #lines, deco)
    apply_follow(follow)
  end)
  return block
end

--- 整段替换 block 内容（行数可变）。
function M.replace_block(block, lines, deco)
  if not valid() or not block or #lines == 0 then
    return
  end
  with_modifiable(function()
    local pos = vim.api.nvim_buf_get_extmark_by_id(M.buf, ns, block.id, { details = true })
    if not pos or #pos == 0 then
      return
    end
    local row, end_row = pos[1], pos[3].end_row
    local follow = record_follow()
    vim.api.nvim_buf_del_extmark(M.buf, ns, block.id)
    vim.api.nvim_buf_clear_namespace(M.buf, ns_deco, row, end_row)
    vim.api.nvim_buf_set_lines(M.buf, row, end_row, false, lines)
    block.id = vim.api.nvim_buf_set_extmark(M.buf, ns, row, 0, {
      end_row = row + #lines,
      right_gravity = false,
      end_right_gravity = false,
    })
    apply_deco(row, #lines, deco)
    apply_follow(follow)
  end)
end

function M.clear_all()
  if not valid() then
    return
  end
  with_modifiable(function()
    vim.api.nvim_buf_clear_namespace(M.buf, ns, 0, -1)
    vim.api.nvim_buf_clear_namespace(M.buf, ns_deco, 0, -1)
    vim.api.nvim_buf_set_lines(M.buf, 0, -1, false, { "" })
  end)
  M.blocks = {}
  M.run_block = nil
end

function M.wipe()
  if valid() then
    pcall(vim.api.nvim_buf_delete, M.buf, { force = true })
  end
  M.buf = nil
  M.blocks = {}
  M.run_block = nil
end

-- ============================================================ 编辑器联动 ==

--- 找到当前 tab 中"用户编辑窗口"（buftype 为空的第一个非 loom buffer 窗口）。
local function editor_win()
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    local b = vim.api.nvim_win_get_buf(win)
    if vim.bo[b].buftype == "" then
      return win
    end
  end
  return nil
end

--- transcript 里 <CR>：打开光标下的文件。
--- 支持两种形态：绝对/相对路径（cfile）与 `path:line` 文本。
function M.open_file_under_cursor()
  local target_win = editor_win()
  if not target_win then
    return
  end
  local cfile = vim.fn.expand("<cfile>")
  local path, lnum = cfile, nil
  if vim.fn.filereadable(cfile) ~= 1 then
    -- 退一步：解析当前行的 `path:line`（tool target / 系统消息常见形态）
    local line = vim.api.nvim_get_current_line()
    local p, ln = line:match("([~%./][%w%-%._/]+):(%d+)")
    if not p or vim.fn.filereadable(vim.fn.fnamemodify(p, ":p")) ~= 1 then
      return
    end
    path = vim.fn.fnamemodify(p, ":p")
    lnum = tonumber(ln)
  end
  vim.api.nvim_set_current_win(target_win)
  if lnum then
    vim.cmd(("edit +%d %s"):format(lnum, vim.fn.fnameescape(path)))
  else
    vim.cmd("edit " .. vim.fn.fnameescape(path))
  end
end

--- 光标所在 fenced code block（assistant markdown 里的 ```lang … ```）。
--- 用 fence 计数奇偶判定：上方最近 fence 必须是"第偶数个之后"（即 opener）。
--- @return {lines: string[], lang: string} | nil
function M.fence_at_cursor()
  if not valid() then
    return nil
  end
  local win = vim.fn.bufwinid(M.buf)
  if win == -1 then
    return nil
  end
  local row = vim.api.nvim_win_get_cursor(win)[1]
  local lines = vim.api.nvim_buf_get_lines(M.buf, 0, -1, false)
  local op
  for i = row, 1, -1 do
    if lines[i]:match("^%s*```") then
      local n = 0
      for j = 1, i - 1 do
        if lines[j]:match("^%s*```") then
          n = n + 1
        end
      end
      if n % 2 == 0 then
        op = i -- 它前面的 fence 成对闭合过，本行必是 opener
      end
      break -- 最近的 fence 若是 closer，说明光标不在代码块内
    end
  end
  if not op or op > row then
    return nil
  end
  local cl
  for i = op + 1, #lines do
    if lines[i]:match("^%s*```%s*$") then
      cl = i
      break
    end
  end
  if not cl or row > cl then
    return nil
  end
  local body = {}
  for i = op + 1, cl - 1 do
    body[#body + 1] = lines[i]
  end
  if #body == 0 then
    return nil
  end
  return { lines = body, lang = lines[op]:match("^%s*```(%w*)") or "" }
end

--- gy：复制光标所在代码块到无名寄存器。
function M.yank_fence()
  local f = M.fence_at_cursor()
  if not f then
    return
  end
  vim.fn.setreg('"', table.concat(f.lines, "\n"))
  util.notify(("已复制 %d 行（%s）"):format(#f.lines, f.lang ~= "" and f.lang or "text"))
end

--- gi：把光标所在代码块插入编辑窗口光标下方。
function M.insert_fence()
  local f = M.fence_at_cursor()
  if not f then
    return
  end
  local win = editor_win()
  if not win then
    util.notify("没有可用的编辑窗口", vim.log.levels.WARN)
    return
  end
  vim.api.nvim_set_current_win(win)
  vim.api.nvim_put(f.lines, "l", true, true)
  util.notify(("已插入 %d 行"):format(#f.lines))
end

--- 移除所有指定 kind 的 block（目前用于欢迎页清除）。
function M.clear_kind(kind)
  if not valid() then
    return
  end
  local keep = {}
  local doomed = {}
  for _, b in ipairs(M.blocks) do
    if b.kind == kind then
      doomed[b.id] = true
    else
      keep[#keep + 1] = b
    end
  end
  if next(doomed) == nil then
    return
  end
  with_modifiable(function()
    local ranges = {}
    for _, b in ipairs(M.blocks) do
      if doomed[b.id] then
        local pos = vim.api.nvim_buf_get_extmark_by_id(M.buf, ns, b.id, { details = true })
        if pos and #pos > 0 then
          ranges[#ranges + 1] = { pos[1], pos[3].end_row }
        end
        vim.api.nvim_buf_del_extmark(M.buf, ns, b.id)
      end
    end
    -- 从下往上删，避免行号漂移；其余 block 的 extmark 自动跟随
    table.sort(ranges, function(a, c)
      return a[1] > c[1]
    end)
    for _, r in ipairs(ranges) do
      vim.api.nvim_buf_clear_namespace(M.buf, ns_deco, r[1], r[2])
      vim.api.nvim_buf_set_lines(M.buf, r[1], r[2], false, {})
    end
  end)
  M.blocks = keep
end

-- ============================================================ 内容 facade ==
-- （包装 blocks 构造器；让 events/approval 层不用直接 import blocks 也行）

function M.add_user_block(prompt)
  local lines, deco = blocks.user(prompt)
  M.add_block(lines, "user", deco)
end

function M.add_system_block(text)
  local lines, deco = blocks.system(text)
  M.add_block(lines, "system", deco)
end

function M.add_approval_block(payload)
  local lines, deco = blocks.approval_card(payload)
  return M.add_block(lines, "approval", deco)
end

function M.replace_with_resolved(block, tool, decision)
  local lines, deco = blocks.approval_resolved(tool, decision)
  M.replace_block(block, lines, deco)
end

function M.new_run_block(run)
  M.run_block = M.add_block(blocks.run(run, M.model, spinner.current()))
end

--- 重绘当前 run block（delta/tool/完成/spinner tick 共用入口）。
function M.render_run(run)
  local lines, deco = blocks.run(run, M.model, spinner.current())
  if M.run_block then
    M.replace_block(M.run_block, lines, deco)
  else
    M.run_block = M.add_block(lines, "run", deco)
  end
end

return M
