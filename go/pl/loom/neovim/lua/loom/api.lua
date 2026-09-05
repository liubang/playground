-- api.lua — REST 最小协议面（SERVE_DESIGN.md §5.3）。
-- 所有回调签名：cb(err, data, status)。err 为人类可读写；2xx 时 err=nil。
local config = require("loom.config")
local http = require("loom.http")
local server = require("loom.server")
local util = require("loom.util")

local M = {}

local function call(method, path, body_tbl, cb)
  local cfg = config.get()
  local token, terr = server.token()
  if not token then
    cb(terr)
    return
  end
  local body = body_tbl and vim.json.encode(body_tbl) or nil
  http.request({ method = method, url = cfg.server.url .. path, token = token, body = body }, function(err, res)
    if err then
      cb(err)
      return
    end
    local decoded = util.json_decode(res.body)
    if res.status >= 200 and res.status < 300 then
      cb(nil, decoded, res.status)
      return
    end
    local code = decoded and decoded.error and decoded.error.code or nil
    local msg = decoded and decoded.error and decoded.error.message or ("HTTP " .. res.status)
    cb(("%s（HTTP %d%s）"):format(msg, res.status, code and (", " .. code) or ""), decoded, res.status)
  end)
end

function M.version(cb)
  call("GET", "/v1/meta/version", nil, cb)
end

function M.list_sessions(limit, cb)
  call("GET", ("/v1/sessions?limit=%d"):format(limit or 50), nil, cb)
end

--- resume_id 为 nil 时新建会话。
function M.create_session(resume_id, cb)
  -- 注意：vim.json.encode({}) 会编码成 "[]"，Go 端期望对象 —— 用 empty_dict。
  local body = resume_id and { resume = resume_id } or vim.empty_dict()
  call("POST", "/v1/sessions", body, cb)
end

function M.snapshot(session_id, cb)
  call("GET", ("/v1/sessions/%s/snapshot"):format(session_id), nil, cb)
end

--- cb(err, {turn?|steered, queue_len?}, status)
function M.submit_prompt(session_id, prompt, cb)
  call("POST", ("/v1/sessions/%s/prompts"):format(session_id), {
    prompt = prompt,
    idempotency_key = util.uuid(),
  }, cb)
end

function M.cancel(session_id, cb)
  call("POST", ("/v1/sessions/%s/cancel"):format(session_id), nil, cb)
end

--- decision: "allow" | "deny"
function M.resolve_approval(session_id, approval_id, payload, cb)
  call("POST", ("/v1/sessions/%s/approvals/%s"):format(session_id, approval_id), payload, cb)
end

--- payload: { selected = {label,...} } 或 { skipped = true }。
function M.answer_question(session_id, question_id, payload, cb)
  call("POST", ("/v1/sessions/%s/questions/%s"):format(session_id, question_id), payload, cb)
end

function M.set_model(session_id, provider, model, cb)
  call("POST", ("/v1/sessions/%s/model"):format(session_id), { provider = provider, model = model }, cb)
end

function M.set_reasoning(session_id, effort, cb)
  call("POST", ("/v1/sessions/%s/reasoning"):format(session_id), { effort = effort }, cb)
end

-- --------------- workspaces（@ 提及 / 文件内容外联）---------------

--- GET /v1/workspaces/{id}/files/search?q= → { matches: [{path,name,kind}], truncated }
function M.workspace_file_search(ws_id, q, cb)
  call("GET", ("/v1/workspaces/%s/files/search?q=%s"):format(ws_id, util.url_encode(q)), nil, cb)
end

--- GET /v1/workspaces/{id}/file?path= → { path, size, truncated, binary, content? }
function M.workspace_file_read(ws_id, path, cb)
  call("GET", ("/v1/workspaces/%s/file?path=%s"):format(ws_id, util.url_encode(path)), nil, cb)
end

return M
