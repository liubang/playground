// api.js — REST 封装（docs/WEB_DESIGN.md §3.4）。
// 统一：Bearer header、wire 错误模型解析、401 全局回 gate、幂等键。

export class ApiError extends Error {
  constructor(status, code, message) {
    super(message || code || `HTTP ${status}`)
    this.status = status
    this.code = code || ''
  }
}

export function createApi({ getToken, onUnauthorized }) {
  async function req(method, path, body, extraHeaders) {
    const headers = { Authorization: 'Bearer ' + getToken() }
    if (body !== undefined) headers['Content-Type'] = 'application/json'
    Object.assign(headers, extraHeaders || {})
    let res
    try {
      res = await fetch(path, {
        method,
        headers,
        body: body !== undefined ? JSON.stringify(body) : undefined,
      })
    } catch (e) {
      throw new ApiError(0, 'network', e.message)
    }
    if (res.status === 401) {
      onUnauthorized()
      throw new ApiError(401, 'unauthenticated', 'token rejected')
    }
    if (!res.ok) {
      let code = '',
        message = ''
      try {
        const payload = await res.json()
        code = payload?.error?.code || ''
        message = payload?.error?.message || ''
      } catch {
        /* non-JSON error body */
      }
      throw new ApiError(res.status, code, message || res.statusText)
    }
    if (res.status === 204) return null
    return res.json()
  }

  return {
    metaVersion: () => req('GET', '/v1/meta/version'),
    metaModels: () => req('GET', '/v1/meta/models'),
    // 工具链/PATH 运行时报告（设置面板「开发环境」卡片）
    metaEnvironment: () => req('GET', '/v1/meta/environment'),
    // 配置（设置面板）：GET 返回 {path, exists, revision, config}（密钥已脱敏）；
    // PUT 携带 revision 乐观锁，409 config_conflict 表示文件被外部修改
    getConfig: () => req('GET', '/v1/config'),
    putConfig: (revision, config) => req('PUT', '/v1/config', { revision, config }),
    // 按需查看单个已存密钥的明文（GET 只下发掩码）；ref = {kind, name, field}
    revealSecret: (ref) => req('POST', '/v1/config/reveal', ref),
    // 聚合所有工作区的 skill 目录（设置面板 Skills tab）
    listSkills: () => req('GET', '/v1/skills'),
    // 按名称禁用/启用：写入 config 的 skills.disabled 并热应用（按名称跨
    // 工作区生效）；响应携带最新 {revision, disabled, applied}
    setSkillDisabled: (name, disabled) =>
      req('PUT', `/v1/skills/${encodeURIComponent(name)}/disabled`, { disabled }),
    // 按 SKILL.md 路径从磁盘删除整个 skill 目录（服务端限定在发现根目录内，不可恢复）
    deleteSkill: (path) => req('DELETE', '/v1/skills', { path }),
    // MCP 服务器实时状态与重连（设置面板）
    listMcpServers: () => req('GET', '/v1/mcp/servers'),
    reconnectMcpServer: (name) =>
      req('POST', `/v1/mcp/servers/${encodeURIComponent(name)}/reconnect`, {}),
    // 规则包（设置面板）：列出内置包与安装状态；安装/卸载写入用户规则目录并热重载
    listRulePacks: () => req('GET', '/v1/rules/packs'),
    installRulePack: (id) => req('PUT', `/v1/rules/packs/${encodeURIComponent(id)}/install`, {}),
    uninstallRulePack: (id) => req('DELETE', `/v1/rules/packs/${encodeURIComponent(id)}`),
    listSessions: (limit = 50, cursor = '', archived = false, workspaceId = '') =>
      req(
        'GET',
        `/v1/sessions?limit=${limit}${cursor ? '&cursor=' + encodeURIComponent(cursor) : ''}${archived ? '&archived=1' : ''}${workspaceId ? '&workspace_id=' + encodeURIComponent(workspaceId) : ''}`,
      ),
    // workspaces（docs/WORKSPACE_DESIGN.md §8.1）
    listWorkspaces: () => req('GET', '/v1/workspaces'),
    registerWorkspace: (rootPath, name) =>
      req('POST', '/v1/workspaces', { root_path: rootPath, name }),
    // 删除工作区：级联删除其下全部会话（存活会话被关闭，不可恢复）；磁盘
    // 目录不动。默认工作区不可删（409 workspace_in_use）
    deleteWorkspace: (id) => req('DELETE', `/v1/workspaces/${id}`),
    browseDirectories: (path) =>
      req('GET', `/v1/files/browse?path=${encodeURIComponent(path || '')}`),
    archiveSession: (id, archived) => req('POST', `/v1/sessions/${id}/archive`, { archived }),
    deleteSession: (id) => req('DELETE', `/v1/sessions/${id}`),
    // 分享链接：创建幂等（重复调用返回同一 token）；撤销后原链接立即失效
    shareSession: (id) => req('POST', `/v1/sessions/${id}/share`),
    revokeShare: (id) => req('DELETE', `/v1/sessions/${id}/share`),
    // 局域网分享监听（桌面端）：开关写穿到 share.enabled 并热应用
    // （即时生效且持久）；无 ShareManager 的 server（loom serve）返回 404
    getShareEndpoint: () => req('GET', '/v1/share/endpoint'),
    setShareEndpoint: (enabled) => req('POST', '/v1/share/endpoint', { enabled }),
    // 用户反馈：对某一轮（run）投 赞=1/踩=0，落为 Langfuse BOOLEAN 分数
    submitFeedback: (id, runId, value, comment = '') =>
      req('POST', `/v1/sessions/${id}/feedback`, { run_id: runId, value, comment }),
    // createSession / resumeSession share POST /v1/sessions. createSession
    // takes an optional workspaceId (empty = the server's default workspace).
    createSession: (workspaceId = '') =>
      req('POST', '/v1/sessions', workspaceId ? { workspace_id: workspaceId } : {}),
    resumeSession: (id) => req('POST', '/v1/sessions', { resume: id }),
    snapshot: (id) => req('GET', `/v1/sessions/${id}/snapshot`),
    transcript: (id, after = 0, limit = 200) =>
      req('GET', `/v1/sessions/${id}/transcript?after=${after}&limit=${limit}`),
    submitPrompt: (id, prompt, idemKey, images = [], followup = false) =>
      req(
        'POST',
        `/v1/sessions/${id}/prompts`,
        { prompt, ...(images.length ? { images } : {}), ...(followup ? { followup: true } : {}) },
        { 'Idempotency-Key': idemKey },
      ),
    cancelTurn: (id) => req('POST', `/v1/sessions/${id}/cancel`, {}),
    setModel: (id, ref) => {
      const [provider, ...rest] = ref.split('/')
      return req('POST', `/v1/sessions/${id}/model`, { provider, model: rest.join('/') })
    },
    setReasoning: (id, effort) => req('POST', `/v1/sessions/${id}/reasoning`, { effort }),
    resolveApproval: (id, approvalId, { callId, argsHash, decision, ruleHint }) =>
      req('POST', `/v1/sessions/${id}/approvals/${approvalId}`, {
        call_id: callId,
        args_hash: argsHash,
        decision,
        client: 'web',
        ...(ruleHint ? { rule_hint: ruleHint } : {}),
      }),
    answerQuestion: (id, questionId, answer) =>
      req('POST', `/v1/sessions/${id}/questions/${questionId}`, answer),
  }
}
