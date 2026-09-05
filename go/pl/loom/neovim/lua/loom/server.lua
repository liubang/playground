-- server.lua — loom serve 进程探测、拉起与 token 解析。
local config = require("loom.config")
local http = require("loom.http")
local util = require("loom.util")

local M = {}

M.job = nil -- 由本插件拉起的 serve job id
M.starting = false

--- 解析 token：config.server.token > token_file > <LOOM_HOME|~/.loom>/sessions/serve.token
function M.token()
  local cfg = config.get()
  if cfg.server.token and cfg.server.token ~= "" then
    return cfg.server.token
  end
  local path = cfg.server.token_file
  if not path then
    local home = vim.env.LOOM_HOME or (vim.fn.expand("~") .. "/.loom")
    path = home .. "/sessions/serve.token"
  end
  local ok, lines = pcall(vim.fn.readfile, path)
  if not ok or not lines or not lines[1] then
    return nil, ("无法读取 token 文件 %s（先跑 `loom serve` 生成，或配置 server.token）"):format(path)
  end
  return util.trim(lines[1])
end

--- 探测 /healthz（免认证）。cb(ok:boolean)
function M.probe(cb)
  local cfg = config.get()
  http.request({ method = "GET", url = cfg.server.url .. "/healthz", timeout = 2 }, function(err, res)
    cb(not err and res ~= nil and res.status == 200)
  end)
end

--- 轮询直到 ready 或超时。cb(err?)。
local function wait_ready(deadline_ms, cb)
  local cfg = config.get()
  http.request({ method = "GET", url = cfg.server.url .. "/readyz", timeout = 2 }, function(err, res)
    if not err and res and res.status == 200 then
      cb(nil)
      return
    end
    if vim.loop.hrtime() / 1e6 > deadline_ms then
      cb(("等待 loom serve 就绪超时（%dms）"):format(cfg.server.startup_timeout_ms))
      return
    end
    vim.defer_fn(function()
      wait_ready(deadline_ms, cb)
    end, 300)
  end)
end

--- 确保 serve 可用：探测 healthz → 失败则 jobstart 拉起 → 轮询 readyz。
--- cb(err?)
function M.start(cb)
  local cfg = config.get()
  if M.starting then
    return
  end
  M.probe(function(ok)
    if ok then
      cb(nil)
      return
    end
    if not cfg.server.auto_start then
      cb("loom serve 未在 " .. cfg.server.url .. " 监听（server.auto_start=false）")
      return
    end
    if vim.fn.executable(cfg.server.loom_bin) ~= 1 then
      cb(("找不到 loom 可执行文件: %s"):format(cfg.server.loom_bin))
      return
    end
    local listen = cfg.server.url:gsub("^https?://", ""):gsub("/$", "")
    M.starting = true
    M.job = vim.fn.jobstart({ cfg.server.loom_bin, "serve", "--listen", listen }, {
      detach = false,
      on_exit = function(_, code)
        M.job = nil
        M.starting = false
        if code ~= 0 then
          util.notify(("loom serve 退出（exit %d）"):format(code), vim.log.levels.WARN)
        end
      end,
    })
    if M.job <= 0 then
      M.starting = false
      cb("jobstart loom serve 失败")
      return
    end
    local deadline = vim.loop.hrtime() / 1e6 + cfg.server.startup_timeout_ms
    wait_ready(deadline, function(werr)
      M.starting = false
      if werr then
        M.stop()
        cb(werr)
        return
      end
      cb(nil)
    end)
  end)
end

--- 停止本插件拉起的 serve（外部已存在的 serve 不受影响）。
function M.stop()
  if M.job then
    pcall(vim.fn.jobstop, M.job)
    M.job = nil
  end
  M.starting = false
end

return M
