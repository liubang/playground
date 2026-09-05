-- http.lua — 基于 curl 的一次性 HTTP 请求（vim.system，nvim >= 0.10）。
-- 响应格式：cb(err, { status = number, body = string })
local M = {}

--- opts: { method, url, token?, body?, timeout? }
--- body 为已序列化的 JSON 字符串（或直接 string）。
function M.request(opts, cb)
  local args = {
    "-sS",
    "-X",
    opts.method,
    "-w",
    "\n%{http_code}",
    "--max-time",
    tostring(opts.timeout or 30),
  }
  if opts.token then
    args[#args + 1] = "-H"
    args[#args + 1] = "Authorization: Bearer " .. opts.token
  end
  if opts.body then
    args[#args + 1] = "-H"
    args[#args + 1] = "Content-Type: application/json"
    args[#args + 1] = "--data-binary"
    args[#args + 1] = "@-"
  end
  args[#args + 1] = opts.url

  vim.system({ "curl", unpack(args) }, { text = true, stdin = opts.body }, function(res)
    vim.schedule(function()
      if res.code ~= 0 then
        cb(("curl 失败（exit %d）: %s"):format(res.code, vim.trim(res.stderr or "")))
        return
      end
      local body, status = (res.stdout or ""):match("^(.*)\n(%d%d%d)$")
      if not status then
        -- body 恰为空时输出为 "\n200"
        status = (res.stdout or ""):match("^%s*(%d%d%d)$")
        body = ""
      end
      if not status then
        cb("无法解析 curl 响应: " .. (res.stdout or ""))
        return
      end
      cb(nil, { status = tonumber(status), body = body or "" })
    end)
  end)
end

return M
