-- context.lua — 编辑器上下文捕获与 prompt 组装。
--
-- Neovim UI 的定位是"写代码时随手把上下文递给 agent"（见 NEOVIM_UI_DESIGN.md §1）：
-- 选区（或行范围）→ 带文件路径/行号/语言 fence 的 prompt，agent 端拿到即可直接定位。
local M = {}

--- 捕获 buffer 的指定行范围。
--- @return {lines: string[], path: string, l1: integer, l2: integer, ft: string}
function M.range(bufnr, l1, l2)
  return {
    lines = vim.api.nvim_buf_get_lines(bufnr, l1 - 1, l2, false),
    path = vim.api.nvim_buf_get_name(bufnr),
    l1 = l1,
    l2 = l2,
    ft = vim.bo[bufnr].filetype,
  }
end

--- 组装 prompt：
---   <msg>
---
---   上下文 `path:ll-cc`：
---   ```lang
---   <code>
---   ```
function M.format(ctx, msg)
  local rel = ctx.path ~= "" and vim.fn.fnamemodify(ctx.path, ":~:.") or "[No Name]"
  local parts = {}
  if msg and msg ~= "" then
    parts[#parts + 1] = msg
    parts[#parts + 1] = ""
  end
  parts[#parts + 1] = ("上下文 `%s:%d-%d`："):format(rel, ctx.l1, ctx.l2)
  parts[#parts + 1] = "```" .. (ctx.ft or "")
  vim.list_extend(parts, ctx.lines)
  parts[#parts + 1] = "```"
  return table.concat(parts, "\n")
end

return M
