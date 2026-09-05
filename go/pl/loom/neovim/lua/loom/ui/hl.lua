-- hl.lua — 高亮组与图标体系（全 default，colorscheme/用户可覆盖）。
--
-- 图标选用 Nerd Font 的 Font Awesome 段（U+F000–F2E0，任何 Nerd Font 必有；
-- lualine/snacks 的 icons_enabled 已保证字体就位）。语义映射：
--   角色    → fa-user / fa-android
--   工具状态 → fa-ellipsis-h（待发）/ fa-dot-circle-o（执行中）/ fa-check / fa-times / fa-ban
--   会话状态 → fa-circle / fa-bolt / fa-hand-paper-o / fa-times-circle
-- 全部集中在 M.icons，用户可在 setup 后覆盖其中的任意键。
local M = {}

M.groups = {
  -- 行级 pill bar（hl_eol 整行）
  LoomUserBar = { link = "DiagnosticVirtualTextHint" },
  LoomAssistantBar = { link = "DiagnosticVirtualTextInfo" },
  LoomWarnBar = { link = "DiagnosticVirtualTextWarn" },
  -- 行级着色
  LoomDim = { link = "Comment" }, -- reasoning 提示、系统消息、工具输出体、耗时
  LoomToolOk = { link = "DiagnosticOk" },
  LoomToolErr = { link = "DiagnosticError" },
  LoomToolRun = { link = "DiagnosticWarn" },
  -- 区间级
  LoomToolName = { bold = true }, -- 工具名
  LoomAccent = { link = "Special" }, -- 欢迎页标题等强调位
}

-- 以 Lua escape 书写，避免字体把文件内容渲染错误时肉眼无法校对。
M.icons = {
  -- 角色
  user = "\u{f007}", -- fa-user
  robot = "\u{f17b}", -- fa-android
  -- 语义事件
  warn = "\u{f071}", -- fa-exclamation-triangle
  ok = "\u{f00c}", -- fa-check
  err = "\u{f00d}", -- fa-times
  info = "\u{f05a}", -- fa-info-circle
  thinking = "\u{f0eb}", -- fa-lightbulb-o
  -- 工具条目状态
  tool = {
    prepared = "\u{f141}", -- fa-ellipsis-h（参数就绪）
    running = "\u{f110}", -- fa-dot-circle-o（执行中）
    success = "\u{f00c}", -- fa-check
    error = "\u{f00d}", -- fa-times
    cancelled = "\u{f05e}", -- fa-ban
  },
  -- 会话状态（winbar）
  status = {
    idle = "\u{f111}", -- fa-circle
    running = "\u{f0e7}", -- fa-bolt
    awaiting_approval = "\u{f256}", -- fa-hand-paper-o
    booting = "\u{f110}", -- fa-dot-circle-o
    cancelling = "\u{f05e}", -- fa-ban
    fatal = "\u{f057}", -- fa-times-circle
    closed = "\u{f10c}", -- fa-circle-o
  },
}

function M.setup()
  for name, opts in pairs(M.groups) do
    vim.api.nvim_set_hl(0, name, vim.tbl_extend("force", { default = true }, opts))
  end
end

return M
