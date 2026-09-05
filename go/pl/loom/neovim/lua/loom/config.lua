-- config.lua — 默认值与用户配置合并。
local M = {}

M.defaults = {
  server = {
    url = "http://127.0.0.1:7680",
    auto_start = true,
    keep_alive = false, -- 退出 nvim 时是否保留拉起的 serve
    startup_timeout_ms = 10000,
    loom_bin = "loom",
    token = nil, -- 显式 token；nil 时从 token_file 读取
    token_file = nil, -- 默认 <LOOM_HOME|~/.loom>/sessions/serve.token
  },
  ui = {
    position = "right", -- right | left
    width_ratio = 0.40, -- transcript 宽度占比
    composer_height = 3, -- prompt buffer，是单次输入条
    max_tool_preview_lines = 20,
    max_tool_diff_lines = 40,
    diff_after_edit = true, -- write/edit 成功后在编辑器文件区铺 before/after diff
    reasoning_style = "hint", -- hint（一行折叠提示）| full（全量引用）| hide
  },
  keymaps = {
    -- composer 是 prompt buffer：Insert/Normal 下按 <CR> 发送，不需要 submit 键位
    cancel = "<C-c>", -- composer（normal）：取消当前 turn
    approve = "y", -- transcript（normal）：批准最新挂起审批
    deny = "n", -- transcript（normal）：拒绝最新挂起审批
  },
}

M.values = nil

function M.setup(opts)
  M.values = vim.tbl_deep_extend("force", {}, M.defaults, opts or {})
  return M.values
end

function M.get()
  if not M.values then
    M.setup({})
  end
  return M.values
end

return M
