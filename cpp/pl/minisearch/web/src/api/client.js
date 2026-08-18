import { session } from '../store/session'

const BASE = '/api/v2'

/**
 * 底层 fetch 封装。401 时清会话并跳登录页。
 * 后端错误统一为 { ok:false, error: "..." }，非 2xx 一律抛 Error。
 *
 * 租户语义（DESIGN.md §10）：租户不出现在 URL 路径，数据面请求解析为凭证所属
 * 租户；admin 可通过 ?tenant= 覆盖。数据面函数的 tenant 参数由调用方（页面）
 * 显式传入；非 admin 的 ?tenant= 会被服务端忽略，因此各角色可统一传参。
 */
async function request(method, path, body = null, query = null) {
  let url = BASE + path
  if (query) {
    const params = new URLSearchParams()
    for (const [k, v] of Object.entries(query)) {
      if (v !== undefined && v !== null && v !== '') params.set(k, String(v))
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
export function listCollections(tenant) {
  return request('GET', '/collections', null, { tenant })
}

export function createCollection(spec, tenant) {
  return request('POST', '/collections', spec, { tenant })
}

export function dropCollection(name, tenant) {
  return request('DELETE', `/collections/${encodeURIComponent(name)}`, null, {
    confirm: name,
    tenant,
  })
}

// ---- Documents ----
export function listDocuments(collection, offset = 0, limit = 50, tenant) {
  return request('GET', `/${encodeURIComponent(collection)}/documents`, null, {
    offset,
    limit,
    tenant,
  })
}

// 分块文档的 chunk id 形如 "<name>#chunk_<i>"；不含该后缀的视为单文档
const CHUNK_ID_RE = /^(.*)#chunk_(\d+)$/

export function docNameOf(id) {
  const m = id.match(CHUNK_ID_RE)
  return m ? m[1] : id
}

export function chunkIndexOf(id) {
  const m = id.match(CHUNK_ID_RE)
  return m ? Number(m[2]) : null
}

/**
 * 拉取 collection 全量 chunk（listDocuments 按 chunk 平铺返回），
 * 在前端按文档名聚合成顶层文档列表：[{ name, chunks, docs }]。
 */
export async function listTopLevelDocuments(collection, tenant) {
  const agg = new Map()
  let offset = 0
  const limit = 200
  let total = 0
  do {
    const resp = await listDocuments(collection, offset, limit, tenant)
    total = resp.total || 0
    for (const d of resp.documents || []) {
      const name = docNameOf(d.id)
      const e = agg.get(name) || { name, chunks: 0, docs: [] }
      e.chunks++
      e.docs.push(d)
      agg.set(name, e)
    }
    offset += limit
  } while (offset < total)
  return Array.from(agg.values())
}

export function getDocument(collection, id, tenant) {
  return request(
    'GET',
    `/${encodeURIComponent(collection)}/documents/${encodeURIComponent(id)}`,
    null,
    {
      tenant,
    },
  )
}

export function upsertDocument(collection, id, doc, tenant) {
  return request(
    'PUT',
    `/${encodeURIComponent(collection)}/documents/${encodeURIComponent(id)}`,
    doc,
    { tenant },
  )
}

export function deleteDocument(collection, id, tenant) {
  return request(
    'DELETE',
    `/${encodeURIComponent(collection)}/documents/${encodeURIComponent(id)}`,
    null,
    { tenant },
  )
}

export function importMarkdown(collection, body, tenant) {
  return request('POST', `/${encodeURIComponent(collection)}/documents:import`, body, { tenant })
}

// ---- Search ----
export function search(collection, params, tenant) {
  return request('POST', `/${encodeURIComponent(collection)}/search`, params, { tenant })
}

export function analyze(collection, text, tenant) {
  return request('POST', `/${encodeURIComponent(collection)}/queries:analyze`, { text }, { tenant })
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

// Collection 跨租户迁移（admin）：POST /admin/tenants/{src}/collections/{name}:move
export function moveCollection(srcTenant, name, target) {
  return request(
    'POST',
    `/admin/tenants/${encodeURIComponent(srcTenant)}/collections/${encodeURIComponent(name)}:move`,
    { target },
  )
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

// API Key 跨租户迁移（admin）：POST /admin/tenants/{src}/keys/{key_id}:move
export function moveKey(srcTenant, keyId, target) {
  return request(
    'POST',
    `/admin/tenants/${encodeURIComponent(srcTenant)}/keys/${encodeURIComponent(keyId)}:move`,
    { target },
  )
}

// ---- Admin: Stats ----
export function getStats() {
  return request('GET', '/admin/stats')
}
