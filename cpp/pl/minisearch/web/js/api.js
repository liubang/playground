/**
 * api.js — MiniSearch Console API 客户端
 *
 * 封装所有后端 REST 调用。Token 存储在 sessionStorage，key 为 "mss_token"。
 * 每次请求自动附加 Authorization: Bearer <token> 头（login/changePassword 除外）。
 */
const API = (() => {
  const BASE = '/api/v2'

  function getToken() {
    return sessionStorage.getItem('mss_token') || ''
  }

  function setToken(token) {
    sessionStorage.setItem('mss_token', token)
  }

  function clearToken() {
    sessionStorage.removeItem('mss_token')
    sessionStorage.removeItem('mss_user')
    sessionStorage.removeItem('mss_tenant')
    sessionStorage.removeItem('mss_role')
  }

  function saveUserInfo(data) {
    sessionStorage.setItem('mss_user', data.user || '')
    sessionStorage.setItem('mss_tenant', data.tenant || '')
    sessionStorage.setItem('mss_role', data.role || '')
  }

  function getUserInfo() {
    return {
      user: sessionStorage.getItem('mss_user') || '',
      tenant: sessionStorage.getItem('mss_tenant') || '',
      role: sessionStorage.getItem('mss_role') || '',
    }
  }

  function isLoggedIn() {
    return !!getToken()
  }

  /**
   * 底层 fetch 封装。
   * @param {string} method HTTP method
   * @param {string} path API path (e.g. "/collections")
   * @param {object|null} body JSON body
   * @param {object|null} query query params
   */
  async function request(method, path, body = null, query = null) {
    let url = BASE + path
    if (query) {
      const params = new URLSearchParams()
      for (const [k, v] of Object.entries(query)) {
        if (v !== undefined && v !== null) params.set(k, v)
      }
      const qs = params.toString()
      if (qs) url += '?' + qs
    }

    const opts = { method, headers: {} }
    const token = getToken()
    if (token) {
      opts.headers['Authorization'] = 'Bearer ' + token
    }
    if (body) {
      opts.headers['Content-Type'] = 'application/json'
      opts.body = JSON.stringify(body)
    }

    const resp = await fetch(url, opts)
    let data = null
    const text = await resp.text()
    if (text) {
      try {
        data = JSON.parse(text)
      } catch {
        data = { error: text }
      }
    }

    if (!resp.ok) {
      const error = (data && (data.error || data.message)) || `HTTP ${resp.status}`
      const err = new Error(error)
      err.status = resp.status
      err.data = data
      throw err
    }

    return data
  }

  // ---- Auth ----
  async function login(user, password) {
    const data = await fetch(BASE + '/auth/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ user, password }),
    }).then((r) => {
      if (!r.ok) {
        return r.text().then((t) => {
          throw new Error(t ? JSON.parse(t).error || 'Login failed' : 'Login failed')
        })
      }
      return r.json()
    })
    setToken(data.token)
    saveUserInfo({ user, tenant: data.tenant, role: data.role })
    return data
  }

  async function logout() {
    try {
      await request('POST', '/auth/logout')
    } catch {
      /* ignore */
    }
    clearToken()
  }

  async function whoami() {
    return request('GET', '/auth/whoami')
  }

  async function changePassword(user, oldPassword, newPassword) {
    return fetch(BASE + '/auth/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ user, old_password: oldPassword, new_password: newPassword }),
    }).then((r) => r.json())
  }

  // ---- Collections ----
  async function listCollections() {
    return request('GET', '/collections')
  }

  async function createCollection(spec) {
    return request('POST', '/collections', spec)
  }

  async function dropCollection(name) {
    return request('DELETE', `/collections/${name}`, null, { confirm: name })
  }

  // ---- Documents ----
  async function listDocuments(collection, offset = 0, limit = 50) {
    return request('GET', `/${collection}/documents`, null, { offset, limit })
  }

  async function getDocument(collection, id) {
    return request('GET', `/${collection}/documents/${id}`)
  }

  async function upsertDocument(collection, id, doc) {
    return request('PUT', `/${collection}/documents/${id}`, doc)
  }

  async function deleteDocument(collection, id) {
    return request('DELETE', `/${collection}/documents/${id}`)
  }

  async function importMarkdown(collection, body) {
    return request('POST', `/${collection}/documents:import`, body)
  }

  // ---- Search ----
  async function search(collection, params) {
    return request('POST', `/${collection}/search`, params)
  }

  async function analyze(collection, text) {
    return request('POST', `/${collection}/queries:analyze`, { text })
  }

  // ---- Admin: Tenants ----
  async function listTenants() {
    return request('GET', '/admin/tenants')
  }

  async function createTenant(name) {
    return request('POST', '/admin/tenants', { name })
  }

  async function dropTenant(name) {
    return request('DELETE', `/admin/tenants/${name}`, null, { confirm: name })
  }

  // ---- Admin: Keys ----
  async function listKeys(tenant) {
    return request('GET', `/admin/tenants/${tenant}/keys`)
  }

  async function issueKey(tenant, role, collections) {
    const body = { role }
    if (collections && collections.length > 0) {
      body.collections = collections
    }
    return request('POST', `/admin/tenants/${tenant}/keys`, body)
  }

  async function revokeKey(tenant, keyId) {
    return request('DELETE', `/admin/tenants/${tenant}/keys/${keyId}`)
  }

  // ---- Admin: Stats ----
  async function getStats() {
    return request('GET', '/admin/stats')
  }

  return {
    login,
    logout,
    whoami,
    changePassword,
    listCollections,
    createCollection,
    dropCollection,
    listDocuments,
    getDocument,
    upsertDocument,
    deleteDocument,
    importMarkdown,
    search,
    analyze,
    listTenants,
    createTenant,
    dropTenant,
    listKeys,
    issueKey,
    revokeKey,
    getStats,
    isLoggedIn,
    getToken,
    getUserInfo,
    clearToken,
  }
})()
