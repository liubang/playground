-- ui/init.lua — 布局编排：右侧 vsplit（transcript）+ 底部 split（composer）。
local config = require("loom.config")
local composer = require("loom.ui.composer")
local transcript = require("loom.ui.transcript")

local M = { transcript_win = nil, composer_win = nil }

local function valid(win)
  return win ~= nil and vim.api.nvim_win_is_valid(win)
end

function M.open()
  local cfg = config.get()
  if valid(M.transcript_win) then
    vim.api.nvim_set_current_win(M.transcript_win)
    return
  end

  vim.cmd("botright vsplit")
  M.transcript_win = vim.api.nvim_get_current_win()
  vim.api.nvim_win_set_width(
    M.transcript_win,
    math.max(40, math.floor(vim.o.columns * cfg.ui.width_ratio))
  )
  local tbuf = transcript.bind(M.transcript_win)

  vim.api.nvim_set_current_win(M.transcript_win)
  vim.cmd("belowright split")
  M.composer_win = vim.api.nvim_get_current_win()
  composer.bind(M.composer_win)
  pcall(vim.api.nvim_win_set_height, M.composer_win, cfg.ui.composer_height)

  -- 审批快捷键作用于 transcript buffer（resolving 最新挂起卡）。
  vim.keymap.set("n", cfg.keymaps.approve, function()
    require("loom.ui.approval").resolve_latest("allow")
  end, { buffer = tbuf, desc = "Loom: 批准待审批操作" })
  vim.keymap.set("n", cfg.keymaps.deny, function()
    require("loom.ui.approval").resolve_latest("deny")
  end, { buffer = tbuf, desc = "Loom: 拒绝待审批操作" })
  vim.keymap.set("n", "<CR>", function()
    require("loom.ui.transcript").open_file_under_cursor()
  end, { buffer = tbuf, desc = "Loom: 打开光标下的文件" })
  vim.keymap.set("n", "gy", function()
    require("loom.ui.transcript").yank_fence()
  end, { buffer = tbuf, desc = "Loom: 复制光标所在代码块" })
  vim.keymap.set("n", "gi", function()
    require("loom.ui.transcript").insert_fence()
  end, { buffer = tbuf, desc = "Loom: 把光标所在代码块插入编辑窗口" })

  if cfg.ui.position == "left" then
    vim.cmd("wincmd H")
  end

  M.focus_composer()
end

function M.hide()
  if valid(M.composer_win) then
    vim.api.nvim_win_close(M.composer_win, true)
  end
  if valid(M.transcript_win) then
    vim.api.nvim_win_close(M.transcript_win, true)
  end
  M.composer_win, M.transcript_win = nil, nil
end

function M.close()
  M.hide()
  transcript.wipe()
end

function M.focus_transcript()
  if valid(M.transcript_win) then
    vim.api.nvim_set_current_win(M.transcript_win)
  end
end

function M.focus_composer()
  if valid(M.composer_win) then
    vim.api.nvim_set_current_win(M.composer_win)
    vim.cmd("startinsert")
  end
end

return M
