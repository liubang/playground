-- health.lua — :checkhealth loom
local M = {}

local function api()
  if vim.health and vim.health.start then
    return vim.health.start, vim.health.ok, vim.health.warn, vim.health.error
  end
  local h = require("health")
  return h.report_start, h.report_ok, h.report_warn, h.report_error
end

function M.check()
  local start, ok, warn, err = api()
  start("loom.nvim")

  if vim.fn.has("nvim-0.10") == 1 then
    ok("Neovim >= 0.10（vim.system / vim.json）")
  else
    err("需要 Neovim >= 0.10", { "升级 Neovim 到 0.10 或更新" })
  end

  if vim.fn.executable("curl") == 1 then
    ok("curl 可用: " .. vim.fn.exepath("curl"))
  else
    err("找不到 curl（REST/SSE 传输必需）", { "安装 curl 并让它在 PATH 中" })
  end

  local cfg = require("loom.config").get()
  if cfg.server.auto_start then
    if vim.fn.executable(cfg.server.loom_bin) == 1 then
      ok("loom 可执行: " .. vim.fn.exepath(cfg.server.loom_bin))
    else
      warn(
        ("找不到 loom 可执行文件: %s（auto_start 时将失败）"):format(cfg.server.loom_bin),
        { "运行 `bazel build //go/pl/loom/cmd/loom`，或配置 server.loom_bin / server.auto_start=false" }
      )
    end
  end

  local server = require("loom.server")
  local token, terr = server.token()
  if token then
    ok("serve token 解析成功（长度 " .. #token .. "）")
  else
    warn(terr, { "执行 `loom serve` 以生成 token，或配置 server.token" })
  end
end

return M
