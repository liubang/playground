// api.ts — REST wrapper (docs/WEB_DESIGN.md §3.4).
// Uniform: Bearer header, wire error-model parsing, global 401 → gate redirect,
// idempotency keys.
// Logic is one-to-one with the old static/js/api.js; only types were added.

import type {
  ApprovalMode,
  ConfigEnvelope,
  DirBrowseResult,
  EnvironmentReport,
  MazeData,
  McpServerStatus,
  ModelCatalog,
  PutConfigResult,
  RulePack,
  SecretRef,
  SessionSummary,
  SetModelResult,
  SetReasoningResult,
  ShareCreateResult,
  ShareEndpoint,
  SkillListResult,
  Snapshot,
  Workspace,
  WorkspaceFileContent,
  WorkspaceFileList,
  WorkspaceFileSearchResult,
  WorkspaceGitDiff,
  WorkspaceGitStatus,
} from './types'

export class ApiError extends Error {
  status: number
  code: string

  constructor(status: number, code: string, message?: string) {
    super(message || code || `HTTP ${status}`)
    this.status = status
    this.code = code || ''
  }
}

export interface ApiOptions {
  getToken: () => string
  onUnauthorized: () => void
}

export function createApi({ getToken, onUnauthorized }: ApiOptions) {
  async function req<T = unknown>(
    method: string,
    path: string,
    body?: unknown,
    extraHeaders?: Record<string, string>,
  ): Promise<T> {
    const headers: Record<string, string> = { Authorization: 'Bearer ' + getToken() }
    if (body !== undefined) headers['Content-Type'] = 'application/json'
    Object.assign(headers, extraHeaders || {})
    let res: Response
    try {
      res = await fetch(path, {
        method,
        headers,
        body: body !== undefined ? JSON.stringify(body) : undefined,
      })
    } catch (e) {
      throw new ApiError(0, 'network', (e as Error).message)
    }
    if (res.status === 401) {
      onUnauthorized()
      throw new ApiError(401, 'unauthenticated', 'token rejected')
    }
    if (!res.ok) {
      let code = ''
      let message = ''
      try {
        const payload = await res.json()
        code = payload?.error?.code || ''
        message = payload?.error?.message || ''
      } catch {
        /* non-JSON error body */
      }
      throw new ApiError(res.status, code, message || res.statusText)
    }
    if (res.status === 204) return null as T
    return res.json() as Promise<T>
  }

  return {
    metaVersion: () => req<{ version?: string }>('GET', '/v1/meta/version'),
    metaModels: () => req<ModelCatalog>('GET', '/v1/meta/models'),
    // Toolchain/PATH runtime report (the settings panel's Dev Environment card)
    metaEnvironment: () => req<EnvironmentReport>('GET', '/v1/meta/environment'),
    // Config (settings panel): GET returns {path, exists, revision, config}
    // (secrets redacted); PUT carries a revision optimistic lock — 409
    // config_conflict means the file was modified externally
    getConfig: () => req<ConfigEnvelope>('GET', '/v1/config'),
    putConfig: (revision: string, config: Record<string, unknown>) =>
      req<PutConfigResult>('PUT', '/v1/config', { revision, config }),
    // Reveal one stored secret's plaintext on demand (GET only sends masks);
    // ref = {kind, name, field}
    revealSecret: (ref: SecretRef) => req<{ value?: string }>('POST', '/v1/config/reveal', ref),
    // Aggregates skill directories across all workspaces (settings panel Skills tab)
    listSkills: () => req<SkillListResult>('GET', '/v1/skills'),
    // Disable/enable by name: writes to config's skills.disabled and hot-applies
    // (name-based, effective across workspaces); the response carries the latest
    // {revision, disabled, applied}
    setSkillDisabled: (name: string, disabled: boolean) =>
      req<{ revision?: string; disabled?: string[] }>(
        'PUT',
        `/v1/skills/${encodeURIComponent(name)}/disabled`,
        { disabled },
      ),
    // Delete a whole skill directory from disk by its SKILL.md path (server
    // restricts this to discovery roots; unrecoverable)
    deleteSkill: (path: string) => req('DELETE', '/v1/skills', { path }),
    // MCP server live status and reconnect (settings panel)
    listMcpServers: () => req<{ servers?: McpServerStatus[] }>('GET', '/v1/mcp/servers'),
    reconnectMcpServer: (name: string) =>
      req<McpServerStatus>('POST', `/v1/mcp/servers/${encodeURIComponent(name)}/reconnect`, {}),
    // Rule packs (settings panel): list built-in packs and install status;
    // install/uninstall write to the user rules directory and hot-reload
    listRulePacks: () => req<{ packs?: RulePack[] }>('GET', '/v1/rules/packs'),
    installRulePack: (id: string) =>
      req('PUT', `/v1/rules/packs/${encodeURIComponent(id)}/install`, {}),
    uninstallRulePack: (id: string) => req('DELETE', `/v1/rules/packs/${encodeURIComponent(id)}`),
    listSessions: (limit = 50, cursor = '', archived = false, workspaceId = '') =>
      req<{ sessions?: SessionSummary[]; next_cursor?: string }>(
        'GET',
        `/v1/sessions?limit=${limit}${cursor ? '&cursor=' + encodeURIComponent(cursor) : ''}${archived ? '&archived=1' : ''}${workspaceId ? '&workspace_id=' + encodeURIComponent(workspaceId) : ''}`,
      ),
    // workspaces (docs/WORKSPACE_DESIGN.md §8.1)
    listWorkspaces: () => req<{ workspaces?: Workspace[] }>('GET', '/v1/workspaces'),
    registerWorkspace: (rootPath: string, name: string) =>
      req<{ workspace: Workspace }>('POST', '/v1/workspaces', { root_path: rootPath, name }),
    // Delete workspace: cascades to all its sessions (live sessions are closed;
    // unrecoverable); the on-disk directory is left untouched. The default
    // workspace cannot be deleted (409 workspace_in_use)
    deleteWorkspace: (id: string) => req('DELETE', `/v1/workspaces/${id}`),
    browseDirectories: (path: string) =>
      req<DirBrowseResult>('GET', `/v1/files/browse?path=${encodeURIComponent(path || '')}`),
    // Workspace right panel: file tree / file preview / git changes / diff (all
    // confined to the workspace root)
    listWorkspaceFiles: (id: string, path: string, showAll = false) =>
      req<WorkspaceFileList>(
        'GET',
        `/v1/workspaces/${id}/files?path=${encodeURIComponent(path)}${showAll ? '&all=1' : ''}`,
      ),
    readWorkspaceFile: (id: string, path: string) =>
      req<WorkspaceFileContent>(
        'GET',
        `/v1/workspaces/${id}/file?path=${encodeURIComponent(path)}`,
      ),
    workspaceGitStatus: (id: string) =>
      req<WorkspaceGitStatus>('GET', `/v1/workspaces/${id}/git/status`),
    workspaceGitDiff: (id: string, path: string, staged = false) =>
      req<WorkspaceGitDiff>(
        'GET',
        `/v1/workspaces/${id}/git/diff?path=${encodeURIComponent(path)}${staged ? '&staged=1' : ''}`,
      ),
    // Composer @ completion: fuzzy file search within the workspace
    searchWorkspaceFiles: (id: string, q: string) =>
      req<WorkspaceFileSearchResult>(
        'GET',
        `/v1/workspaces/${id}/files/search?q=${encodeURIComponent(q)}`,
      ),
    // Approval-mode quick toggle: workspace-level override, effective next turn,
    // not persisted
    setWorkspaceApprovalMode: (id: string, mode: ApprovalMode) =>
      req<{ mode?: string }>('POST', `/v1/workspaces/${id}/approval-mode`, { mode }),
    // Effective approval mode (live override or config default); pages re-read
    // this after session opens so a reload never misreports an earlier switch
    getWorkspaceApprovalMode: (id: string) =>
      req<{ mode?: string }>('GET', `/v1/workspaces/${id}/approval-mode`),
    archiveSession: (id: string, archived: boolean) =>
      req('POST', `/v1/sessions/${id}/archive`, { archived }),
    deleteSession: (id: string) => req('DELETE', `/v1/sessions/${id}`),
    // Share links: creation is idempotent (repeat calls return the same token);
    // revoking invalidates the original link immediately
    shareSession: (id: string) => req<ShareCreateResult>('POST', `/v1/sessions/${id}/share`),
    revokeShare: (id: string) => req('DELETE', `/v1/sessions/${id}/share`),
    // LAN share listener (desktop): the toggle writes through to share.enabled
    // and hot-applies (immediate and persistent); servers without a ShareManager
    // (loom serve) return 404
    getShareEndpoint: () => req<ShareEndpoint>('GET', '/v1/share/endpoint'),
    setShareEndpoint: (enabled: boolean) =>
      req<{ endpoint?: ShareEndpoint }>('POST', '/v1/share/endpoint', { enabled }),
    // User feedback: thumbs-up=1 / thumbs-down=0 for a turn (run), recorded as a
    // Langfuse BOOLEAN score
    submitFeedback: (id: string, runId: string, value: 0 | 1, comment = '') =>
      req('POST', `/v1/sessions/${id}/feedback`, { run_id: runId, value, comment }),
    // createSession / resumeSession share POST /v1/sessions. createSession
    // takes an optional workspaceId (empty = the server's default workspace).
    createSession: (workspaceId = '') =>
      req<{ session_id: string }>(
        'POST',
        '/v1/sessions',
        workspaceId ? { workspace_id: workspaceId } : {},
      ),
    resumeSession: (id: string) => req('POST', '/v1/sessions', { resume: id }),
    snapshot: (id: string) => req<Snapshot>('GET', `/v1/sessions/${id}/snapshot`),
    // Execution-trace maze (shared by the trace tab and the compare view)
    maze: (id: string) => req<MazeData>('GET', `/v1/sessions/${id}/maze`),
    transcript: (id: string, after = 0, limit = 200) =>
      req('GET', `/v1/sessions/${id}/transcript?after=${after}&limit=${limit}`),
    submitPrompt: (
      id: string,
      prompt: string,
      idemKey: string,
      images: { media_type: string; data: string }[] = [],
      followup = false,
    ) =>
      req(
        'POST',
        `/v1/sessions/${id}/prompts`,
        { prompt, ...(images.length ? { images } : {}), ...(followup ? { followup: true } : {}) },
        { 'Idempotency-Key': idemKey },
      ),
    cancelTurn: (id: string) => req('POST', `/v1/sessions/${id}/cancel`, {}),
    setModel: (id: string, ref: string) => {
      const [provider, ...rest] = ref.split('/')
      return req<SetModelResult>('POST', `/v1/sessions/${id}/model`, {
        provider,
        model: rest.join('/'),
      })
    },
    setReasoning: (id: string, effort: string) =>
      req<SetReasoningResult>('POST', `/v1/sessions/${id}/reasoning`, { effort }),
    resolveApproval: (
      id: string,
      approvalId: string,
      {
        callId,
        argsHash,
        decision,
        ruleHint,
      }: {
        callId?: string
        argsHash?: string
        decision: 'allow' | 'deny'
        ruleHint?: { tool_name?: string; arguments?: Record<string, unknown>; trust?: string }
      },
    ) =>
      req('POST', `/v1/sessions/${id}/approvals/${approvalId}`, {
        call_id: callId,
        args_hash: argsHash,
        decision,
        client: 'web',
        ...(ruleHint ? { rule_hint: ruleHint } : {}),
      }),
    answerQuestion: (id: string, questionId: string, answer: unknown) =>
      req('POST', `/v1/sessions/${id}/questions/${questionId}`, answer),
  }
}

export type Api = ReturnType<typeof createApi>
