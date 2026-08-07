// api.js — REST 封装（docs/WEB_DESIGN.md §3.4）。
// 统一：Bearer header、wire 错误模型解析、401 全局回 gate、幂等键。

export class ApiError extends Error {
  constructor(status, code, message) {
    super(message || code || `HTTP ${status}`);
    this.status = status;
    this.code = code || "";
  }
}

export function createApi({ getToken, onUnauthorized }) {
  async function req(method, path, body, extraHeaders) {
    const headers = { Authorization: "Bearer " + getToken() };
    if (body !== undefined) headers["Content-Type"] = "application/json";
    Object.assign(headers, extraHeaders || {});
    let res;
    try {
      res = await fetch(path, {
        method,
        headers,
        body: body !== undefined ? JSON.stringify(body) : undefined,
      });
    } catch (e) {
      throw new ApiError(0, "network", e.message);
    }
    if (res.status === 401) {
      onUnauthorized();
      throw new ApiError(401, "unauthenticated", "token rejected");
    }
    if (!res.ok) {
      let code = "", message = "";
      try {
        const payload = await res.json();
        code = payload?.error?.code || "";
        message = payload?.error?.message || "";
      } catch { /* non-JSON error body */ }
      throw new ApiError(res.status, code, message || res.statusText);
    }
    if (res.status === 204) return null;
    return res.json();
  }

  return {
    metaVersion: () => req("GET", "/v1/meta/version"),
    metaModels: () => req("GET", "/v1/meta/models"),
    listSessions: (limit = 50, cursor = "", archived = false, workspaceId = "") =>
      req("GET", `/v1/sessions?limit=${limit}${cursor ? "&cursor=" + encodeURIComponent(cursor) : ""}${archived ? "&archived=1" : ""}${workspaceId ? "&workspace_id=" + encodeURIComponent(workspaceId) : ""}`),
    // workspaces（docs/WORKSPACE_DESIGN.md §8.1）
    listWorkspaces: () => req("GET", "/v1/workspaces"),
    registerWorkspace: (rootPath, name) => req("POST", "/v1/workspaces", { root_path: rootPath, name }),
    browseDirectories: (path) => req("GET", `/v1/files/browse?path=${encodeURIComponent(path || "")}`),
    archiveSession: (id, archived) => req("POST", `/v1/sessions/${id}/archive`, { archived }),
    deleteSession: (id) => req("DELETE", `/v1/sessions/${id}`),
    // 分享链接：创建幂等（重复调用返回同一 token）；撤销后原链接立即失效
    shareSession: (id) => req("POST", `/v1/sessions/${id}/share`),
    revokeShare: (id) => req("DELETE", `/v1/sessions/${id}/share`),
    // 用户反馈：对某一轮（run）投 赞=1/踩=0，落为 Langfuse BOOLEAN 分数
    submitFeedback: (id, runId, value, comment = "") =>
      req("POST", `/v1/sessions/${id}/feedback`, { run_id: runId, value, comment }),
    // createSession / resumeSession share POST /v1/sessions. createSession
    // takes an optional workspaceId (empty = the server's default workspace).
    createSession: (workspaceId = "") => req("POST", "/v1/sessions", workspaceId ? { workspace_id: workspaceId } : {}),
    resumeSession: (id) => req("POST", "/v1/sessions", { resume: id }),
    snapshot: (id) => req("GET", `/v1/sessions/${id}/snapshot`),
    transcript: (id, after = 0, limit = 200) =>
      req("GET", `/v1/sessions/${id}/transcript?after=${after}&limit=${limit}`),
    submitPrompt: (id, prompt, idemKey) =>
      req("POST", `/v1/sessions/${id}/prompts`, { prompt }, { "Idempotency-Key": idemKey }),
    cancelTurn: (id) => req("POST", `/v1/sessions/${id}/cancel`, {}),
    setModel: (id, ref) => {
      const [provider, ...rest] = ref.split("/");
      return req("POST", `/v1/sessions/${id}/model`, { provider, model: rest.join("/") });
    },
    setReasoning: (id, effort) => req("POST", `/v1/sessions/${id}/reasoning`, { effort }),
    resolveApproval: (id, approvalId, { callId, argsHash, decision, ruleHint }) =>
      req("POST", `/v1/sessions/${id}/approvals/${approvalId}`, {
        call_id: callId, args_hash: argsHash, decision,
        client: "web",
        ...(ruleHint ? { rule_hint: ruleHint } : {}),
      }),
    answerQuestion: (id, questionId, answer) =>
      req("POST", `/v1/sessions/${id}/questions/${questionId}`, answer),
  };
}
