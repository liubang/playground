-- util.lua — 通用小工具。
local M = {}

function M.notify(msg, level)
  vim.schedule(function()
    vim.notify(msg, level or vim.log.levels.INFO, { title = "loom" })
  end)
end

--- 把字符串按 \n 拆成行表（保留空行；不含行尾 \r）。
function M.split_lines(text)
  if not text or text == "" then
    return {}
  end
  local lines = vim.split(text, "\n", { plain = true })
  for i, l in ipairs(lines) do
    lines[i] = l:gsub("\r$", "")
  end
  return lines
end

--- JSON 解码；空 body 返回 nil；失败返回 nil, err。
function M.json_decode(body)
  if not body or body == "" then
    return nil
  end
  local ok, decoded = pcall(vim.json.decode, body)
  if not ok then
    return nil, decoded
  end
  return decoded
end

function M.trim(s)
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

--- 只在主循环执行 fn（curl/job 回调里调 UI 前统一过这层）。
function M.schedule(fn)
  return function(...)
    local args = { ... }
    vim.schedule(function()
      fn(unpack(args))
    end)
  end
end

--- URL query 编码（路径/查询串上 server 端统一 url-decoded）。
function M.url_encode(s)
  return (s:gsub("[^%w%-%._~]", function(c)
    return string.format("%%%02X", string.byte(c))
  end))
end

--- 简单 UUID v4，用于 Idempotency-Key。
function M.uuid()
  local rand = math.random
  local template = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
  return (template:gsub("[xy]", function(c)
    local v = (c == "x") and rand(0, 15) or rand(8, 11)
    return string.format("%x", v)
  end))
end

--- "2h ago" 风格相对时间。ts 支持 RFC3339（带时区）或 unix 秒。
function M.time_ago(ts)
  if not ts then
    return ""
  end
  local epoch
  if type(ts) == "number" then
    epoch = ts
  else
    -- 2026-07-30T14:02:11Z / 2026-07-30T14:02:11+08:00
    local y, mo, d, h, mi, s = ts:match("^(%d+)%-(%d+)%-(%d+)T(%d+):(%d+):(%d+)")
    if not y then
      return tostring(ts)
    end
    -- os.time 按本地时区解释字段；先当成 UTC 解释，再按本机时区差与时区标记换算。
    local as_local = os.time({
      year = tonumber(y), month = tonumber(mo), day = tonumber(d),
      hour = tonumber(h), min = tonumber(mi), sec = tonumber(s),
    })
    local utc_offset = os.difftime(os.time(), os.time(os.date("!*t")))
    epoch = as_local + utc_offset -- 视字段为 UTC
    local sign, th, tm = ts:match("([+-])(%d%d):(%d%d)$")
    if sign then
      local off = tonumber(th) * 3600 + tonumber(tm) * 60
      epoch = epoch - (sign == "+" and off or -off)
    end
  end
  local delta = os.time() - epoch
  if delta < 0 then
    delta = 0
  end
  if delta < 60 then
    return delta .. "s ago"
  elseif delta < 3600 then
    return math.floor(delta / 60) .. "m ago"
  elseif delta < 86400 then
    return math.floor(delta / 3600) .. "h ago"
  end
  return math.floor(delta / 86400) .. "d ago"
end

--- 短会话号：sess_ab12cd... → ab12cd
function M.short_id(id)
  if not id then
    return "?"
  end
  local s = tostring(id):gsub("^sess_", "")
  return s:sub(1, 8)
end

return M
