-- diffview.lua — 编辑器内真实 diff（vim diffmode，非浮窗预览），两个入口：
--
--   M.show(payload)            审批时刻（write/edit）：右窗 = proposed 新文本（由 arguments 构造）
--   M.show_completed(path, old) 事后时刻（tool.completed）：右窗 = 盘上 fresh 文本，左窗 = prepared 时刻存的旧快照
--
-- 共享 open_pair 引擎：左 = :edit 目标文件（真实 buffer，未保存修改时跳过并警告），
-- 右 = loom-diff:// scratch（nomodifiable + q 关闭）。任意端 approval.resolved 或
-- resync 时自动收敛（approval 视图），事后视图由 q / 下一个同类视图替代。
--
-- 旧文本优先读**已加载 buffer**（所见即用户当前编辑态），否则读盘。
local state = require("loom.state")

local M = {}

-- approval_id -> { file_win, scratch_win, scratch_buf, path }
local approval_views = {}
-- path -> 同构（事后视图；同一 path 只允许一个活跃视图，新的替代旧的）
local post_views = {}

-- ----------------------------------------------------------------- 通用辅助

--- 读文件内容：已加载 buffer 优先（保留用户未保存的编辑态），否则读盘。
function M.read_old(path)
  local b = vim.fn.bufnr(path)
  if b ~= -1 and vim.api.nvim_buf_is_loaded(b) then
    return vim.api.nvim_buf_get_lines(b, 0, -1, false)
  end
  if vim.fn.filereadable(path) == 1 then
    return vim.fn.readfile(path)
  end
  return {}
end

--- 工具的相对路径是 workspace 相对（snapshot.workspace_root 权威），getcwd 仅兜底。
function M.resolve_path(raw)
  if type(raw) ~= "string" or raw == "" then
    return nil
  end
  if raw:sub(1, 1) == "/" or raw:sub(1, 1) == "~" then
    return vim.fn.fnamemodify(raw, ":p")
  end
  local base = state.workspace_root or vim.fn.getcwd()
  return vim.fn.fnamemodify(base .. "/" .. raw, ":p")
end

--- write/edit 工具：返回绝对路径与新文本（新文本 = 由 arguments 客户端构造）。
--- 非 write/edit 或参数不全 → nil。
function M.payload_file_edit(payload)
  local tool = payload.tool_name
  if tool ~= "write" and tool ~= "edit" then
    return nil
  end
  local args = payload.arguments
  if type(args) == "string" then
    local ok, decoded = pcall(vim.json.decode, args)
    args = ok and decoded or nil
  end
  if type(args) ~= "table" or type(args.path) ~= "string" or args.path == "" then
    return nil
  end
  local path = M.resolve_path(args.path)
  local old_lines = M.read_old(path)
  local new_lines
  if tool == "write" then
    new_lines = vim.split(args.content or "", "\n", { plain = true })
  else
    local old = table.concat(old_lines, "\n")
    local o, n = args.old_string or "", args.new_string or ""
    if o == "" then
      return nil
    end
    local new
    if args.replace_all then
      new = old:gsub(vim.pesc(o), (n:gsub("%%", "%%%%")))
    else
      local s, e = old:find(o, 1, true)
      if not s then
        return nil
      end
      new = old:sub(1, s - 1) .. n .. old:sub(e + 1)
    end
    new_lines = vim.split(new, "\n", { plain = true })
  end
  return { path = path, old = old_lines, new = new_lines }
end

local function find_file_win()
  for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if vim.bo[vim.api.nvim_win_get_buf(win)].buftype == "" then
      return win
    end
  end
  return nil
end

--- 关掉一个视图：diffoff 两侧、关 scratch 窗与 buffer。保留用户的文件窗口/buffer。
local function teardown(view)
  for _, win in ipairs({ view.file_win, view.scratch_win }) do
    if vim.api.nvim_win_is_valid(win) then
      vim.api.nvim_win_call(win, function()
        vim.cmd("diffoff!")
      end)
    end
  end
  if vim.api.nvim_win_is_valid(view.scratch_win) then
    pcall(vim.api.nvim_win_close, view.scratch_win, true)
  elseif vim.api.nvim_buf_is_valid(view.scratch_buf) then
    pcall(vim.api.nvim_buf_delete, view.scratch_buf, { force = true })
  end
end

--- 造一个 diff scratch：nomodifiable + filetype 同步 + q 关闭。
local function make_scratch(win, lines, ft, name, title, close_cb)
  local buf = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_win_set_buf(win, buf)
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, lines)
  vim.bo[buf].buftype = "nofile"
  vim.bo[buf].bufhidden = "wipe"
  vim.bo[buf].modifiable = false
  vim.bo[buf].filetype = ft
  vim.api.nvim_buf_set_name(buf, name)
  vim.wo[win].winbar = title
  vim.keymap.set("n", "q", close_cb, { buffer = buf, desc = "Loom: 关闭 diff 视图" })
  return buf
end

--- 开一对 diff 窗。spec:
---   path：目标文件（真实 buffer 总有一侧展示它——edit 后读盘 fresh）
---   scratch_side："left"（旧快照在左，真文件在右=事后模式）
---                 "right"（真文件在左，proposed 在右=审批模式）
---   scratch_lines / scratch_title：scratch 侧内容与标题
--- 用户 buffer 有未保存修改时放弃（E37 前置检查，绝不动用户数据），返回 nil。
--- 返回 view table：{ file_win, scratch_win, scratch_buf, path }（scratch_win 是 scratch 所在窗）。
local function open_pair(spec, on_close)
  local file_win = find_file_win()
  if not file_win then
    return nil
  end
  local prev_win = vim.api.nvim_get_current_win()
  vim.api.nvim_set_current_win(file_win)
  local target_bufnr = vim.fn.bufnr(spec.path)
  if target_bufnr ~= -1 and vim.api.nvim_buf_is_loaded(target_bufnr) and vim.bo[target_bufnr].modified then
    vim.api.nvim_set_current_win(prev_win)
    vim.notify(
      ("loom: %s 有未保存的本地修改，跳过 diff 展示"):format(vim.fn.fnamemodify(spec.path, ":~:.")),
      vim.log.levels.WARN
    )
    return nil
  end
  vim.cmd("edit " .. vim.fn.fnameescape(spec.path))
  file_win = vim.api.nvim_get_current_win()
  local file_buf = vim.api.nvim_win_get_buf(file_win)
  local ft = vim.bo[file_buf].filetype
  local rel = vim.fn.fnamemodify(spec.path, ":~:.")

  local view = { file_win = file_win, path = spec.path }
  local function close()
    teardown(view)
    if on_close then
      on_close(view)
    end
  end

  -- 布局：file 窗（当前）+ 一侧 vsplit 给 scratch
  if spec.scratch_side == "left" then
    vim.cmd("topleft vsplit")
  else
    vim.cmd("vsplit")
  end
  local scratch_win = vim.api.nvim_get_current_win()
  local title = ("  %s — %s（q 关闭）"):format(spec.scratch_title, rel)
  view.scratch_win = scratch_win
  view.scratch_buf =
    make_scratch(scratch_win, spec.scratch_lines, ft, "loom-diff://" .. spec.scratch_title .. "/" .. spec.path, title, close)

  vim.api.nvim_win_call(file_win, function()
    vim.cmd("diffthis")
  end)
  vim.api.nvim_win_call(scratch_win, function()
    vim.cmd("diffthis")
  end)
  vim.api.nvim_set_current_win(prev_win) -- 不抢焦点
  return view
end

-- ----------------------------------------------------------------- 审批时刻

function M.show(payload)
  local edit = M.payload_file_edit(payload)
  if not edit or not payload.approval_id then
    return
  end
  local view = open_pair({
    path = edit.path,
    scratch_side = "right", -- 左 = 现状，右 = proposed
    scratch_lines = edit.new,
    scratch_title = "proposed",
  })
  if view then
    approval_views[payload.approval_id] = view
  end
end

--- approval.resolved（任意端决议）：清理对应审批视图。
function M.hide(approval_id)
  local v = approval_views[approval_id]
  if not v then
    return
  end
  approval_views[approval_id] = nil
  teardown(v)
end

-- ----------------------------------------------------------------- 事后时刻

--- tool.completed（write/edit 成功）后铺 diff：左 = prepared 快照，右 = 盘上 fresh 文本。
--- 同一 path 重复调用时替代旧视图（连续多轮编辑同一文件只留最新一组）。
function M.show_completed(path, old_lines)
  if type(path) ~= "string" or path == "" then
    return
  end
  if post_views[path] then
    teardown(post_views[path])
    post_views[path] = nil
  end
  local view = open_pair({
    path = path,
    scratch_side = "left", -- 左 = prepared 时刻的旧快照，右 = 盘上 fresh 文本
    scratch_lines = old_lines,
    scratch_title = "before",
  }, function(v)
    if post_views[v.path] == v then
      post_views[v.path] = nil
    end
  end)
  if view then
    post_views[path] = view
  end
end

-- ----------------------------------------------------------------- 清理

function M.hide_all()
  for id in pairs(approval_views) do
    M.hide(id)
  end
  for path in pairs(post_views) do
    local v = post_views[path]
    post_views[path] = nil
    teardown(v)
  end
end

return M
