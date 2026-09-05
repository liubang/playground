-- loom.nvim — Neovim client for `loom serve` (REST + SSE).
-- 注册 :Loom* 用户命令；真正的初始化在用户调用 require("loom").setup() 之后生效。
if vim.g.loaded_loom then
  return
end
vim.g.loaded_loom = 1

vim.api.nvim_create_user_command("Loom", function()
  require("loom").open()
end, { desc = "打开（聚焦）Loom 面板" })

vim.api.nvim_create_user_command("LoomNew", function()
  require("loom").new_session()
end, { desc = "新建 Loom 会话并挂载" })

vim.api.nvim_create_user_command("LoomSessions", function()
  require("loom.ui.sessions").pick()
end, { desc = "选择/恢复一个历史 Loom 会话" })

vim.api.nvim_create_user_command("LoomCancel", function()
  require("loom").cancel()
end, { desc = "取消当前 turn" })

-- 用法：visual 选中后 :'<,'>LoomSend [附加说明]；不带范围时同普通提问。
vim.api.nvim_create_user_command("LoomSend", function(opts)
  require("loom").send_selection(opts.args, opts.range > 0, opts.line1, opts.line2)
end, { range = true, nargs = "?", desc = "发送（选区+上下文）给 Loom" })

vim.api.nvim_create_user_command("LoomHide", function()
  require("loom.ui").hide()
end, { desc = "隐藏 Loom 面板（状态保留）" })

vim.api.nvim_create_user_command("LoomClose", function()
  require("loom.ui").close()
end, { desc = "关闭 Loom 面板（清理 buffer）" })

vim.api.nvim_create_user_command("LoomStop", function()
  require("loom.server").stop()
end, { desc = "停止由插件拉起的 loom serve" })
