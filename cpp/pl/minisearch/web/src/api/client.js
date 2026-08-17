import { session } from '../store/session'

const BASE = '/api/v2'

/**
 * 底层 fetch 封装。401 时清会话并跳登录页。
 * 后端错误统一为 { ok:false, error: "..." }，非 2xx 一律抛 Error。
 */
async function request(method, path, body = null, query = null) {
  let url = BASE + path
  if (query) {
    const params = new URLSearchParams()
    for (const [k, v] of Object.entries(query)) {
      if (v !== undefined && v !== null) params.set(k, String(v))
    }
    const qs = params.toString()
    if (qs) url += '?' + qs
  }

  const opts = { method, headers: {} }
  if (session.token) opts.headers['Authorization'] = 'Bearer ' + session.token
  if (body !== null && body !== undefined) {
    opts.headers['Content-Type'] = 'application/json'
    opts.body = JSON.stringify(body)
  }

  const resp = await fetch(url, opts)
  const text = await resp.text()
  let data = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      data = { error: text }
    }
  }

  if (resp.status === 401 && !path.startsWith('/auth/login')) {
    session.token = ''
    session.user = ''
    // 触发路由守卫跳登录
    window.dispatchEvent(new CustomEvent('mss:unauthorized'))
  }

  if (!resp.ok) {
    const msg = (data && (data.error || data.message)) || `HTTP ${resp.status}`
    const err = new Error(msg)
    err.status = resp.status
    err.data = data
    throw err
  }
  return data
}

// ---- Auth ----
export async function login(user, password) {
  const resp = await fetch(BASE + '/auth/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user, password }),
  })
  const data = await resp.json().catch(() => ({}))
  if (!resp.ok) throw new Error(data.error || `HTTP ${resp.status}`)
  return data // { token, role, tenant, expires_in }
}

export function logout() {
  // 尽力而为，忽略网络错误
  return request('POST', '/auth/logout').catch(() => null)
}

export function whoami() {
  return request('GET', '/auth/whoami')
}

export function changePassword(user, oldPassword, newPassword) {
  return request('POST', '/auth/password', {
    user,
    old_password: oldPassword,
    new_password: newPassword,
  })
}

// ---- Collections ----
export function listCollections() {
  return request('GET', '/collections')
}

export function createCollection(spec) {
  return request('POST', '/collections', spec)
}

export function dropCollection(name) {
  return request('DELETE', `/collections/${encodeURIComponent(name)}`, null, { confirm: name })
}

// ---- Documents ----
export function listDocuments(collection, offset = 0, limit = 50) {
  return request('GET', `/${encodeURIComponent(collection)}/documents`, null, { offset, limit })
}

export function getDocument(collection, id) {
  return request('GET', `/${encodeURIComponent(collection)}/documents/${encodeURIComponent(id)}`)
}

export function upsertDocument(collection, id, doc) {
  return request(
    'PUT',
    `/${encodeURIComponent(collection)}/documents/${encodeURIComponent(id)}`,
    doc,
  )
}

export function deleteDocument(collection, id) {
  return request('DELETE', `/${encodeURIComponent(collection)}/documents/${encodeURIComponent(id)}`)
}

export function importMarkdown(collection, body) {
  return request('POST', `/${encodeURIComponent(collection)}/documents:import`, body)
}

// ---- Search ----
export function search(collection, params) {
  return request('POST', `/${encodeURIComponent(collection)}/search`, params)
}

export function analyze(collection, text) {
  return request('POST', `/${encodeURIComponent(collection)}/queries:analyze`, { text })
}

// ---- Admin: Tenants ----
export function listTenants() {
  return request('GET', '/admin/tenants')
}

export function createTenant(name) {
  return request('POST', '/admin/tenants', { name })
}

export function dropTenant(name) {
  return request('DELETE', `/admin/tenants/${encodeURIComponent(name)}`, null, { confirm: name })
}

// ---- Admin: Keys ----
export function listKeys(tenant) {
  return request('GET', `/admin/tenants/${encodeURIComponent(tenant)}/keys`)
}

export function issueKey(tenant, role, collections) {
  const body = { role }
  if (collections && collections.length > 0) body.collections = collections
  return request('POST', `/admin/tenants/${encodeURIComponent(tenant)}/keys`, body)
}

export function revokeKey(tenant, keyId) {
  return request(
    'DELETE',
    `/admin/tenants/${encodeURIComponent(tenant)}/keys/${encodeURIComponent(keyId)}`,
  )
}

// ---- Admin: Stats ----
export function getStats() {
  return request('GET', '/admin/stats')
}
