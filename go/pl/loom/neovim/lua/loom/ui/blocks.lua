-- blocks.lua — 内容渲染层：消息/工具/审批状态 → 可见行 + 行级/区间级高亮。
--
-- 模块契约：
--   - 纯函数：不读 buffer、不开窗口、不依赖事件层；输入是状态，输出可单测。
--   - 每个构造器返回 (lines, deco)。
--   - Deco 两种形态：
--       行级：  { row, group, eol? }              —— pill bar / 整行着色
--       区间级：{ row, col0, col1, group }        —— 字节区间着色（col 为 byte index）
--     row 一律为块内 0-based 偏移；区间级 col 为 byte 下标（含 0 起点，半开区间）。
--
-- 样式纪律：
--   - 头部一律 pill bar（hl_eol），不出现裸 markdown 标题语法；
--   - 工具正文一律 "  │ " 前缀 + LoomDim，不上 markdown fence
--     （fence 在反复整段重渲染下会短暂留下孤儿 ``` 高亮，视觉噪点大）；
--   - 助手正文保留原始 markdown（filetype=markdown + conceallevel=2 渲染）。
local config = require("loom.config")
local icons = require("loom.ui.hl").icons
local util = require("loom.util")

local M = {}

-- ---------------------------------------------------------------------------
-- deco 构造器
-- ---------------------------------------------------------------------------

---@class LoomBuilder
---@field lines string[]
---@field deco  table[]

--- 新建 builder；返回的 add(text) 追加行，add 之后可继续链式标记 deco：
---   b.add("text")                                无高亮
---   b.add("text"):line("LoomDim")                整行
---   b.add("text"):bar("LoomUserBar")             整行 + hl_eol（pill）
---   b.add("text"):span(2, 5, "LoomToolName")     字节区间
--- @return LoomBuilder
local function Builder()
  local self = { lines = {}, deco = {} }
  local last_row = nil

  function self.add(text)
    self.lines[#self.lines + 1] = text
    last_row = #self.lines - 1
    return self
  end

  function self.blank()
    return self.add("")
  end

  --- 行级高亮（eol=true 时整行填充为 pill bar）。
  function self:line(group, eol)
    self.deco[#self.deco + 1] = { row = last_row, group = group, eol = eol or false }
    return self
  end

  function self:bar(group)
    return self:line(group, true)
  end

  --- 区间级高亮（当前行，[col0, col1) byte 区间）。
  function self:span(col0, col1, group)
    self.deco[#self.deco + 1] = { row = last_row, col0 = col0, col1 = col1, group = group }
    return self
  end

  return self
end

-- ---------------------------------------------------------------------------
-- 工具条目
-- ---------------------------------------------------------------------------

local TOOL_STYLE = {
  prepared = { icon = icons.tool.prepared, hl = "LoomToolRun" },
  running = { icon = icons.tool.running, hl = "LoomToolRun" },
  success = { icon = icons.tool.success, hl = "LoomToolOk" },
  error = { icon = icons.tool.error, hl = "LoomToolErr" },
  cancelled = { icon = icons.tool.cancelled, hl = "LoomDim" },
}

--- 尝试把 preview/error 解析成 run_cmd 风格负载 {stdout, stderr, exit_code}。
--- 命中返回 table；不命中返回 nil。
local function run_cmd_payload(raw)
  if type(raw) ~= "string" or raw:sub(1, 1) ~= "{" then
    return nil
  end
  local ok, decoded = pcall(vim.json.decode, raw)
  if not ok or type(decoded) ~= "table" then
    return nil
  end
  if decoded.stdout == nil and decoded.stderr == nil then
    return nil
  end
  return decoded
end

--- 工具正文行：preview/error 统一入口，截断到 limit。
--- @return string[]|nil  nil = 无正文
local function tool_body_lines(t, limit)
  local raw = t.preview
  if (t.error or "") ~= "" then
    raw = t.error
  end
  if not raw or raw == "" then
    return nil
  end
  local lines = {}
  local payload = run_cmd_payload(raw)
  if payload then
    -- {stdout, stderr, exit_code}：结构化呈现，避免把 JSON 原文糊出来。
    if payload.exit_code and payload.exit_code ~= 0 then
      lines[#lines + 1] = ("exit %d"):format(payload.exit_code)
    end
    for _, l in ipairs(util.split_lines(payload.stdout or "")) do
      lines[#lines + 1] = l
    end
    for _, l in ipairs(util.split_lines(payload.stderr or "")) do
      lines[#lines + 1] = l ~= "" and ("stderr: " .. l) or ""
    end
  else
    lines = util.split_lines(raw)
  end
  local total = #lines
  if total > limit then
    local cut = {}
    for i = 1, limit do
      cut[i] = lines[i]
    end
    cut[#cut + 1] = ("…（已截断，共 %d 行）"):format(total)
    lines = cut
  end
  return lines
end

--- 向 builder 追加一个工具条目：
---   ● run_cmd  ls -la /tmp/*.json · 0.4s
---     │ stdout 行…
local function append_tool(b, t, cfg)
  local st = t.status or "running"
  local style = TOOL_STYLE[st] or TOOL_STYLE.running

  local pre = "  " .. style.icon .. " "
  local name = t.name or "tool"
  local head = pre .. name
  local target = (t.target or "") ~= "" and t.target or nil
  local target_off = nil
  if target then
    target_off = #head + 2
    head = head .. "  " .. target
  end
  local dur_off = nil
  if t.duration_ms then
    dur_off = #head + 3
    head = head .. ("   %.1fs"):format(t.duration_ms / 1000)
  end

  b.blank()
  b.add(head)
    :span(2, 2 + #style.icon, style.hl)
    :span(#pre, #pre + #name, "LoomToolName")
  if target_off then
    b:span(target_off, target_off + #target, "LoomDim")
  end
  if dur_off then
    b:span(dur_off, #head, "LoomDim")
  end

  local body
  if t.diff and st == "prepared" and t.diff ~= "" then
    body = util.split_lines(t.diff)
    local total = #body
    if total > cfg.ui.max_tool_diff_lines then
      local cut = {}
      for i = 1, cfg.ui.max_tool_diff_lines do
        cut[i] = body[i]
      end
      cut[#cut + 1] = ("…（已截断，共 %d 行）"):format(total)
      body = cut
    end
  else
    body = tool_body_lines(t, cfg.ui.max_tool_preview_lines)
  end
  for _, l in ipairs(body or {}) do
    b.add("  │ " .. l):line("LoomDim")
  end
end

-- ---------------------------------------------------------------------------
-- 公开构造器
-- ---------------------------------------------------------------------------

--- 欢迎页（transcript 为空时展示，首个 turn.started 到来时清除）。
function M.welcome()
  local b = Builder()
  b.blank()
  b.add("  " .. icons.robot .. " loom"):line("LoomAccent")
  b.blank()
  b.add("  <CR> 发送 · <C-c> 取消 turn · y/n 审批"):line("LoomDim")
  b.add("  :'<,'>LoomSend 发送选区 · <CR> 打开文件 · gy/gi 取用代码块"):line("LoomDim")
  b.add("  :LoomSessions 恢复会话 · :LoomNew 新会话"):line("LoomDim")
  b.blank()
  return b.lines, b.deco
end

--- 用户消息块。
function M.user(prompt)
  local b = Builder()
  b.blank()
  b.add(" " .. icons.user .. " 你"):bar("LoomUserBar")
  b.blank()
  for _, l in ipairs(util.split_lines(prompt)) do
    b.add(l)
  end
  return b.lines, b.deco
end

--- 助手 run 块（含流式草稿、工具条目、spinner 态）。
--- @param run table
--- @param model string|nil
--- @param spinner {frame:string, elapsed:string}|nil 非 nil = turn 进行中
function M.run(run, model, spinner)
  local cfg = config.get()
  local b = Builder()

  -- 头部（活动态附 spinner 帧 + 已耗时）
  b.blank()
  local head = " " .. icons.robot .. " loom"
  if model and model ~= "" then
    head = head .. " · " .. model
  end
  if spinner then
    head = ("%s    %s %s"):format(head, spinner.frame, spinner.elapsed)
  end
  b.add(head):bar("LoomAssistantBar")
  b.blank()

  local has_body = false

  -- reasoning：hint（一行折叠提示，默认）/ full / hide
  local reasoning = run.reasoning or ""
  if reasoning ~= "" and cfg.ui.reasoning_style ~= "hide" then
    has_body = true
    if cfg.ui.reasoning_style == "full" then
      for _, l in ipairs(util.split_lines(reasoning)) do
        b.add("  > " .. l):line("LoomDim")
      end
    else
      b.add(("  %s 思考 %d 字 · 已折叠"):format(icons.thinking, vim.fn.strchars(reasoning))):line("LoomDim")
      b.blank()
    end
  end

  -- 正文（原样 markdown，交给 filetype + conceal 渲染）
  local text_lines = util.split_lines(run.text or "")
  for _, l in ipairs(text_lines) do
    b.add(l)
  end
  if #text_lines > 0 then
    has_body = true
  end

  -- 工具条目
  for _, cid in ipairs(run.tool_order or {}) do
    local t = run.tools[cid]
    if t then
      has_body = true
      append_tool(b, t, cfg)
    end
  end

  -- 错误尾注
  if run.error and run.error ~= "" then
    has_body = true
    b.blank()
    b.add("  " .. icons.warn .. " " .. run.error):line("LoomToolErr")
  end

  -- 空态 / 流式光标
  if not has_body then
    if spinner then
      b.add(("  %s 思考中…"):format(spinner.frame)):line("LoomDim")
    else
      b.add("  ·"):line("LoomDim")
    end
  elseif spinner then
    b.blank()
    b.add("  " .. spinner.frame):line("LoomDim")
  end

  b.blank()
  return b.lines, b.deco
end

--- 系统通知块（budget/steer/compacted/warning 等一次性消息）。
function M.system(text)
  local b = Builder()
  b.blank()
  b.add("  " .. icons.info .. " " .. text):line("LoomDim")
  return b.lines, b.deco
end

-- ---------------------------------------------------------------------------
-- 历史消息（attach/resync 全量重放 snapshot.messages）
-- ---------------------------------------------------------------------------

--- 从工具 arguments 里提取一行 target（history 没有 live target 字段，尽力而为）。
function M.tool_target_from_args(arguments)
  if type(arguments) ~= "string" then
    arguments = nil
    return nil
  end
  local ok, args = pcall(vim.json.decode, arguments)
  if not ok or type(args) ~= "table" then
    return nil
  end
  return args.path or args.command or args.pattern or args.query or args.file_path
end

--- 历史助手卡：pill 头（"· 历史" 标注）+ reasoning hint + 正文 + 工具行 + 中断尾注。
--- msg: domain.Message {role="assistant", parts, status}
function M.history_assistant(msg)
  local b = Builder()
  b.add(" " .. icons.robot .. " loom · 历史"):bar("LoomAssistantBar")
  local text = ""
  local results = {}
  for _, p in ipairs(msg.parts or {}) do
    if p.kind == "reasoning" and p.reasoning and (p.reasoning.text or "") ~= "" then
      b.add(("  %s 思考 %d 字 · 已折叠"):format(icons.thinking, vim.fn.strchars(p.reasoning.text)))
        :line("LoomDim")
    elseif p.kind == "text" and p.text then
      text = text .. p.text
    elseif p.kind == "tool_result" and p.tool_result then
      results[tostring(p.tool_result.call_id)] = p.tool_result
    end
  end
  local TOOL_ICON = {
    success = { icons.tool.success, "LoomToolOk" },
    error = { icons.tool.error, "LoomToolErr" },
    timeout = { icons.tool.error, "LoomToolErr" },
    cancelled = { icons.tool.cancelled, "LoomDim" },
  }
  -- 阅读顺序固定为「正文 → 工具行」（live run 中工具交错出现，历史按整理后的形态折行）
  for _, l in ipairs(util.split_lines(text)) do
    b.add("  " .. l)
  end
  for _, p in ipairs(msg.parts or {}) do
    if p.kind == "tool_call" and p.tool_call then
      local tr = results[tostring(p.tool_call.id)]
      local st = (tr and TOOL_ICON[tr.status or "success"]) or TOOL_ICON.success
      local target = M.tool_target_from_args(p.tool_call.arguments)
      b.add(("  %s %s%s"):format(st[1], p.tool_call.name or "?", target and (" " .. target) or ""))
        :line(st[2])
        :span(#st[1] + 2, nil, "LoomToolName")
    end
  end
  if msg.status == "interrupted" then
    b.add("  " .. icons.warn .. " 已中断"):line("LoomToolErr")
  end
  b.blank()
  return b.lines, b.deco
end

--- 审批卡（approval.requested）。
function M.approval_card(p)
  local b = Builder()
  b.blank()
  b.add((" %s 需要批准 — %s"):format(icons.warn, p.tool_name or "?")):bar("LoomWarnBar")
  if p.target and p.target ~= "" then
    b.add("    " .. p.target)
  end
  if p.risk then
    b.add(("    risk: %s"):format(tostring(p.risk))):line("LoomDim")
  end
  if p.description and p.description ~= "" then
    b.blank()
    for _, l in ipairs(util.split_lines(p.description)) do
      b.add("  " .. l)
    end
  end
  if type(p.arguments) == "table" and next(p.arguments) ~= nil then
    b.blank()
    for k, v in pairs(p.arguments) do
      local vs = type(v) == "string" and v or vim.json.encode(v)
      b.add(("  %s = %s"):format(k, vs)):line("LoomDim")
    end
  end
  b.blank()
  b.add("  y 批准 · n 拒绝"):line("LoomDim")
  b.blank()
  return b.lines, b.deco
end

--- 审批收敛行（approval.resolved；多客户端广播走同一路径）。
function M.approval_resolved(tool, decision)
  local allowed = decision == "allow"
  local b = Builder()
  b.blank()
  b.add(("  %s %s — %s"):format(allowed and icons.ok or icons.err, allowed and "已批准" or "已拒绝", tool))
    :line(allowed and "LoomToolOk" or "LoomToolErr")
  b.blank()
  return b.lines, b.deco
end

return M
