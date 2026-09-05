-- sse.lua — SSE 事件流（curl -N job），重连/instance/resync 语义对齐
-- internal/server/webui/src/protocol/sse.ts 与 SERVE_DESIGN.md §5.4。
local M = {}

--- opts: {
---   url, token, session_id, after (uint cursor),
---   handlers = {
---     on_event(evt),
---     on_resync(kind, reason),  -- kind: "gap" | "instance" | "server.resync" | "server.draining"
---     on_error(msg),
---   }
--- }
--- 返回 handle = { close(), is_closed() }。重连由 handle 内部管理（cursor 自维护）。
--- opts.jobstart 仅供测试注入（默认 vim.fn.jobstart）。
function M.connect(opts)
  local jobstart = opts.jobstart or vim.fn.jobstart
  local state = {
    closed = false,
    job = nil,
    pending = "",
    frame = {},
    last_seq = nil,
    after = opts.after or 0,
    instance = nil,
    backoff = 1, -- 秒，1 → 15 指数退避
    stderr = {},
  }

  local function do_close()
    state.closed = true
    if state.job then
      pcall(vim.fn.jobstop, state.job)
      state.job = nil
    end
  end

  local function emit_resync(kind, reason)
    do_close()
    vim.schedule(function()
      opts.handlers.on_resync(kind, reason)
    end)
  end

  local function dispatch_frame()
    local ev_name = state.frame.event
    local data = state.frame.data
    state.frame = {}
    if not ev_name and not data then
      return
    end
    if ev_name == "server.resync" or ev_name == "server.draining" then
      -- 服务端帧带 reason（如 cursor_invalid / shutdown），尽量展示真实原因。
      local reason = "server signaled"
      if data then
        local ok, pl = pcall(vim.json.decode, data)
        if ok and type(pl) == "table" and pl.reason then
          reason = tostring(pl.reason)
        end
      end
      emit_resync(ev_name, reason)
      return
    end
    if not data then
      return -- 心跳注释行不组帧，无 data 的帧忽略
    end
    local ok, evt = pcall(vim.json.decode, data)
    if not ok or type(evt) ~= "table" then
      return
    end
    -- sequence 是全局 broker 序号（跨会话共享，天然稀疏）：客户端只保序不判跳变。
    -- 真 gap 由服务端 Replay.Since 判定（cursor 不可挽回 → server.resync 帧），
    -- 客户端做 +1 连续性检查会把其他会话推进的序号误判成本会话丢帧。
    local seq = tonumber(evt.sequence)
    if seq then
      state.last_seq = seq
      if seq > state.after then
        state.after = seq -- 重连 cursor
      end
    end
    state.backoff = 1
    vim.schedule(function()
      opts.handlers.on_event(evt)
    end)
  end

  local function process_line(line)
    line = line:gsub("\r$", "")
    if line == "" then
      dispatch_frame()
      return
    end
    if line:sub(1, 1) == ":" then
      -- 首帧 ": connected, instance=<id>"；心跳 ": hb <unix>"
      local instance = line:match("instance=([%w%-_]+)")
      if instance then
        if state.instance and state.instance ~= instance then
          emit_resync("instance", "server 实例变化")
          return
        end
        state.instance = instance
        state.backoff = 1
      end
      return
    end
    local field, value = line:match("^(%a+):%s?(.*)$")
    if not field then
      return
    end
    if field == "event" then
      state.frame.event = value
    elseif field == "data" then
      state.frame.data = state.frame.data and (state.frame.data .. "\n" .. value) or value
    end
  end

  local function handle_chunk(data)
    if not data or #data == 0 then
      return
    end
    -- jobstart 语义：data[1] 接续上一个 partial line；最后一个元素是新的 partial。
    -- 拼接后的整行必须在【本次】入循环处理，否则跨 chunk 的首行行被静默丢弃
    -- （丢 blank line 会把相邻两帧合并，进而造成 seq/instance 判定污染）。
    data[1] = state.pending .. data[1]
    state.pending = table.remove(data)
    for _, line in ipairs(data) do
      if state.closed then
        return
      end
      process_line(line)
    end
  end

  local function open()
    if state.closed then
      return
    end
    local endpoint = ("%s/v1/sessions/%s/events?after=%d"):format(opts.url, opts.session_id, state.after)
    state.job = jobstart({
      "curl",
      "-sS",
      "-N",
      "--no-buffer",
      "-H",
      "Authorization: Bearer " .. opts.token,
      "-H",
      "Accept: text/event-stream",
      endpoint,
    }, {
      stdout_buffered = false,
      on_stdout = function(_, data)
        handle_chunk(data)
      end,
      on_stderr = function(_, data)
        for _, l in ipairs(data or {}) do
          if l ~= "" then
            state.stderr[#state.stderr + 1] = l
          end
        end
      end,
      on_exit = function(_, code)
        state.job = nil
        if state.closed then
          return
        end
        if #state.stderr > 0 and code ~= 0 then
          local msg = table.concat(state.stderr, " ")
          state.stderr = {}
          vim.schedule(function()
            opts.handlers.on_error(msg)
          end)
        end
        vim.defer_fn(function()
          open()
        end, state.backoff * 1000)
        state.backoff = math.min(state.backoff * 2, 15)
      end,
    })
    if state.job <= 0 then
      state.job = nil
      vim.schedule(function()
        opts.handlers.on_error("jobstart curl 失败")
      end)
    end
  end

  open()

  return {
    close = do_close,
    is_closed = function()
      return state.closed
    end,
  }
end

return M
