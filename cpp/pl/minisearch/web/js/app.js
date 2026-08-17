/**
 * app.js — MiniSearch Console 主应用
 *
 * 职责：
 * 1. 路由分发（hash-based SPA）
 * 2. 登录/退出流程
 * 3. 各功能页面的事件绑定与渲染
 */

;(function () {
  'use strict'

  // ---- 工具函数 ----
  function $(id) {
    return document.getElementById(id)
  }
  function el(tag, attrs, ...children) {
    const e = document.createElement(tag)
    if (attrs) {
      for (const [k, v] of Object.entries(attrs)) {
        if (k === 'class') e.className = v
        else if (k === 'text') e.textContent = v
        else if (k === 'html') e.innerHTML = v
        else if (k.startsWith('on')) e.addEventListener(k.slice(2), v)
        else e.setAttribute(k, v)
      }
    }
    for (const c of children) {
      if (c == null) continue
      if (typeof c === 'string' || typeof c === 'number') e.appendChild(document.createTextNode(c))
      else e.appendChild(c)
    }
    return e
  }

  let toastTimer = null
  function toast(msg, type = '') {
    const t = $('toast')
    t.textContent = msg
    t.className = 'toast' + (type ? ' ' + type : '')
    if (toastTimer) clearTimeout(toastTimer)
    toastTimer = setTimeout(() => {
      t.classList.add('hidden')
    }, 3500)
  }

  // 从 protobuf JSON 提取 FieldValue
  function fv(val) {
    if (val == null) return ''
    if (typeof val === 'string') return val
    if (typeof val === 'number') return String(val)
    if (val.s != null) return val.s
    if (val.n != null) return String(val.n)
    if (val.v != null) return '[' + (val.v.data ? val.v.data.length : 0) + ' floats]'
    return JSON.stringify(val)
  }

  // ---- 登录页面 ----
  function showLogin() {
    $('login-page').classList.remove('hidden')
    $('app').classList.add('hidden')
  }

  function showApp() {
    $('login-page').classList.add('hidden')
    $('app').classList.remove('hidden')
    const info = API.getUserInfo()
    $('user-tenant').textContent = info.tenant || 'default'
    $('user-role').textContent = info.role || 'admin'
  }

  async function doLogin(e) {
    e.preventDefault()
    const user = $('login-user').value.trim()
    const password = $('login-password').value
    const errEl = $('login-error')
    const btn = $('login-btn')
    btn.disabled = true
    btn.textContent = '登录中...'
    errEl.classList.add('hidden')
    try {
      await API.login(user, password)
      showApp()
      await navigateToRoute()
      toast('登录成功', 'success')
    } catch (err) {
      errEl.textContent = err.message || '登录失败'
      errEl.classList.remove('hidden')
    } finally {
      btn.disabled = false
      btn.textContent = '登录'
    }
  }

  async function doLogout() {
    await API.logout()
    showLogin()
    toast('已退出')
  }

  // ---- 路由 ----
  function getRoute() {
    const hash = window.location.hash.slice(1)
    if (!hash || hash === '/') return 'search'
    const parts = hash.split('/')
    return parts[1] || 'search'
  }

  function navigateToRoute() {
    const route = getRoute()
    document.querySelectorAll('.route-page').forEach((p) => p.classList.add('hidden'))
    document.querySelectorAll('.nav-item').forEach((a) => a.classList.remove('active'))
    const page = $('route-' + route)
    const nav = document.querySelector(`.nav-item[data-route="${route}"]`)
    if (page) page.classList.remove('hidden')
    if (nav) nav.classList.add('active')
    // 按路由触发数据加载
    switch (route) {
      case 'search':
        loadSearchPage()
        break
      case 'collections':
        loadCollectionsPage()
        break
      case 'documents':
        loadDocumentsPage()
        break
      case 'tenants':
        loadTenantsPage()
        break
      case 'keys':
        loadKeysPage()
        break
      case 'stats':
        loadStatsPage()
        break
    }
  }

  // ---- 搜索页 ----
  async function loadSearchPage() {
    try {
      const resp = await API.listCollections()
      const sel = $('search-collection')
      const current = sel.value
      sel.innerHTML = ''
      const cols = resp.collections || []
      if (cols.length === 0) {
        sel.appendChild(el('option', { value: '', text: '(无 Collection)' }))
      } else {
        for (const c of cols) {
          sel.appendChild(
            el('option', { value: c.name, text: `${c.name} (${c.active_documents || 0})` }),
          )
        }
      }
      if (current) sel.value = current
    } catch (err) {
      toast('加载 Collections 失败: ' + err.message, 'error')
    }
  }

  async function doSearch() {
    const collection = $('search-collection').value
    const text = $('search-text').value.trim()
    if (!collection) {
      toast('请先选择 Collection', 'error')
      return
    }
    if (!text) {
      toast('请输入查询文本', 'error')
      return
    }

    const params = {
      text,
      top_k: parseInt($('search-topk').value) || 10,
      rerank: $('search-rerank').checked,
    }
    const bm25w = parseFloat($('search-bm25-weight').value)
    const vecw = parseFloat($('search-vec-weight').value)
    if (!isNaN(bm25w) || !isNaN(vecw)) {
      params.weights = {
        bm25: isNaN(bm25w) ? 1.0 : bm25w,
        vector: isNaN(vecw) ? 1.0 : vecw,
      }
    }

    const container = $('search-results')
    container.innerHTML = ''
    container.appendChild(el('div', { class: 'took-tag', text: '搜索中...' }))

    try {
      const resp = await API.search(collection, params)
      container.innerHTML = ''
      const hits = resp.hits || []
      if (hits.length === 0) {
        container.appendChild(el('div', { class: 'result-item', text: '无结果' }))
        return
      }
      const header = el('div', { class: 'toolbar' })
      const took = el('span', {
        class: 'took-tag',
        text: `耗时 ${resp.took_ms || 0}ms · ${hits.length} 条结果`,
      })
      header.appendChild(took)
      if (resp.degraded && resp.degraded.length > 0) {
        for (const d of resp.degraded) {
          header.appendChild(el('span', { class: 'degraded-tag', text: `degraded: ${d}` }))
        }
      }
      container.appendChild(header)

      for (const hit of hits) {
        const doc = hit.document || {}
        const fields = doc.fields || {}
        const title = fv(fields.title) || fv(fields.Title) || '(untitled)'
        const body = fv(fields.content) || fv(fields.body) || fv(fields.text) || ''
        const item = el('div', { class: 'result-item', onclick: () => toggleResultBody(item) })
        item.appendChild(
          el(
            'div',
            { class: 'result-header' },
            el('span', { class: 'result-id', text: doc.id || hit.id || '' }),
            el('span', { class: 'result-score', text: `score: ${(hit.score || 0).toFixed(4)}` }),
          ),
        )
        item.appendChild(el('div', { class: 'result-title', text: title }))
        item.appendChild(
          el('div', {
            class: 'result-body',
            text: body.length > 300 ? body.slice(0, 300) + '...' : body,
          }),
        )
        // show all field names as tags
        const tags = el('div', { class: 'result-fields' })
        for (const fname of Object.keys(fields)) {
          tags.appendChild(el('span', { class: 'result-field-tag', text: fname }))
        }
        item.appendChild(tags)
        container.appendChild(item)
      }
    } catch (err) {
      container.innerHTML = ''
      toast('搜索失败: ' + err.message, 'error')
    }
  }

  function toggleResultBody(item) {
    const body = item.querySelector('.result-body')
    if (body) {
      if (body.style.maxHeight === 'none') {
        body.style.maxHeight = '120px'
        body.style.overflow = 'hidden'
      } else {
        body.style.maxHeight = 'none'
        body.style.overflow = 'visible'
      }
    }
  }

  // ---- Collections 页 ----
  async function loadCollectionsPage() {
    const container = $('collections-list')
    container.innerHTML = ''
    try {
      const resp = await API.listCollections()
      const cols = resp.collections || []
      if (cols.length === 0) {
        container.appendChild(
          el('div', { class: 'card', text: '暂无 Collection，点击"新建"创建。' }),
        )
        return
      }
      for (const c of cols) {
        const card = el('div', { class: 'card' })
        card.appendChild(el('div', { class: 'card-title', text: c.name }))
        card.appendChild(
          el('div', { class: 'card-meta', text: `活跃文档: ${c.active_documents || 0}` }),
        )
        const actions = el('div', { class: 'card-actions' })
        actions.appendChild(
          el('button', {
            class: 'btn btn-danger btn-sm',
            text: '删除',
            onclick: () => dropCollection(c.name),
          }),
        )
        card.appendChild(actions)
        container.appendChild(card)
      }
    } catch (err) {
      toast('加载列表失败: ' + err.message, 'error')
    }
  }

  async function dropCollection(name) {
    if (!confirm(`确认删除 Collection "${name}"？此操作不可恢复。`)) return
    try {
      await API.dropCollection(name)
      toast('已删除 ' + name, 'success')
      await loadCollectionsPage()
    } catch (err) {
      toast('删除失败: ' + err.message, 'error')
    }
  }

  async function doCreateCollection(e) {
    e.preventDefault()
    const name = $('cc-name').value.trim()
    const schemaText = $('cc-schema').value.trim()
    const analyzer = $('cc-analyzer').value.trim() || 'cjk_jieba'
    if (!name || !schemaText) {
      toast('名称和 Schema 不能为空', 'error')
      return
    }

    let fields
    try {
      const parsed = JSON.parse(schemaText)
      fields = parsed.fields || parsed
      if (!Array.isArray(fields)) throw new Error('Schema 需要 fields 数组')
    } catch {
      toast('Schema JSON 解析失败', 'error')
      return
    }

    const spec = { name, fields, default_analyzer: analyzer }
    try {
      const resp = await API.createCollection(spec)
      if (resp.ok) {
        toast('Collection 已创建', 'success')
        $('create-collection-modal').classList.add('hidden')
        $('create-collection-form').reset()
        await loadCollectionsPage()
      } else {
        toast('创建失败: ' + (resp.error || '未知错误'), 'error')
      }
    } catch (err) {
      toast('创建失败: ' + err.message, 'error')
    }
  }

  // ---- 文档管理页 ----
  async function loadDocumentsPage() {
    try {
      const resp = await API.listCollections()
      const sel = $('docs-collection')
      const current = sel.value
      sel.innerHTML = ''
      const cols = resp.collections || []
      if (cols.length === 0) {
        sel.appendChild(el('option', { value: '', text: '(无 Collection)' }))
      } else {
        for (const c of cols) {
          sel.appendChild(
            el('option', { value: c.name, text: `${c.name} (${c.active_documents || 0})` }),
          )
        }
      }
      if (current) sel.value = current
      if (sel.value) await refreshDocuments()
    } catch (err) {
      toast('加载列表失败: ' + err.message, 'error')
    }
  }

  let docsOffset = 0
  const docsLimit = 50

  async function refreshDocuments() {
    const collection = $('docs-collection').value
    if (!collection) return
    const container = $('documents-list')
    container.innerHTML = ''
    try {
      const resp = await API.listDocuments(collection, docsOffset, docsLimit)
      const docs = resp.documents || []
      if (docs.length === 0) {
        container.appendChild(el('div', { class: 'card', text: '该 Collection 暂无文档。' }))
      } else {
        const table = el('table')
        const thead = el('thead')
        const headRow = el('tr')
        headRow.appendChild(el('th', { text: 'ID' }))
        headRow.appendChild(el('th', { text: 'Fields' }))
        headRow.appendChild(el('th', { text: '操作' }))
        thead.appendChild(headRow)
        table.appendChild(thead)
        const tbody = el('tbody')
        for (const doc of docs) {
          const tr = el('tr')
          tr.appendChild(el('td', { text: doc.id }))
          const fields = doc.fields || {}
          const fieldStr = Object.entries(fields)
            .map(([k, v]) => `${k}: ${fv(v).slice(0, 40)}`)
            .join(', ')
          tr.appendChild(
            el('td', { text: fieldStr.slice(0, 80) + (fieldStr.length > 80 ? '...' : '') }),
          )
          const delBtn = el('button', {
            class: 'btn btn-danger btn-sm',
            text: '删除',
            onclick: async () => {
              if (!confirm(`删除文档 "${doc.id}"？`)) return
              try {
                await API.deleteDocument(collection, doc.id)
                toast('已删除', 'success')
                await refreshDocuments()
              } catch (err) {
                toast('删除失败: ' + err.message, 'error')
              }
            },
          })
          const actions = el('div', { class: 'card-actions' })
          actions.appendChild(
            el('button', {
              class: 'btn btn-sm',
              text: '查看',
              onclick: () => showDocDetail(collection, doc.id),
            }),
          )
          actions.appendChild(delBtn)
          tr.appendChild(el('td', {}, actions))
          tbody.appendChild(tr)
        }
        table.appendChild(tbody)
        container.appendChild(table)
      }
      // pagination
      const pagi = $('docs-pagination')
      pagi.innerHTML = ''
      const total = resp.total || 0
      pagi.appendChild(el('span', { class: 'took-tag', text: `共 ${total} 条` }))
      if (docsOffset > 0) {
        pagi.appendChild(
          el('button', {
            class: 'btn btn-sm',
            text: '上一页',
            onclick: () => {
              docsOffset = Math.max(0, docsOffset - docsLimit)
              refreshDocuments()
            },
          }),
        )
      }
      if (docsOffset + docsLimit < total) {
        pagi.appendChild(
          el('button', {
            class: 'btn btn-sm',
            text: '下一页',
            onclick: () => {
              docsOffset += docsLimit
              refreshDocuments()
            },
          }),
        )
      }
    } catch (err) {
      toast('加载文档失败: ' + err.message, 'error')
    }
  }

  async function doImportMarkdown(e) {
    e.preventDefault()
    const collection = $('docs-collection').value
    const name = $('import-name').value.trim()
    const content = $('import-content').value
    const strategy = $('import-strategy').value
    const chunk_size = parseInt($('import-chunk-size').value) || 1000
    const chunk_overlap = parseInt($('import-chunk-overlap').value) || 0
    if (!collection) {
      toast('请先选择 Collection', 'error')
      return
    }
    if (!name) {
      toast('文档名称不能为空', 'error')
      return
    }
    if (!content) {
      toast('内容不能为空', 'error')
      return
    }

    try {
      const resp = await API.importMarkdown(collection, {
        name,
        content,
        strategy,
        chunk_size,
        chunk_overlap,
      })
      if (resp.ok) {
        toast(`导入成功：${resp.chunks || 0} 个 chunk`, 'success')
        $('import-modal').classList.add('hidden')
        $('import-form').reset()
        await refreshDocuments()
      } else {
        toast('导入失败: ' + (resp.error || ''), 'error')
      }
    } catch (err) {
      toast('导入失败: ' + err.message, 'error')
    }
  }

  // ---- 文档详情 ----
  async function showDocDetail(collection, id) {
    const container = $('doc-detail-content')
    container.innerHTML = ''
    container.appendChild(el('div', { class: 'took-tag', text: '加载中...' }))
    $('doc-detail-modal').classList.remove('hidden')
    try {
      const resp = await API.getDocument(collection, id)
      container.innerHTML = ''
      if (!resp.found) {
        container.appendChild(el('div', { class: 'card', text: '文档不存在' }))
        return
      }
      const doc = resp.document || {}
      container.appendChild(
        el(
          'div',
          { class: 'form-group' },
          el('label', { text: 'ID' }),
          el('input', { value: doc.id || id, readonly: true }),
        ),
      )
      container.appendChild(
        el(
          'div',
          { class: 'form-group' },
          el('label', { text: 'Version' }),
          el('input', { value: String(doc.version || 0), readonly: true }),
        ),
      )
      const fields = doc.fields || {}
      const fieldList = Object.keys(fields)
      if (fieldList.length > 0) {
        container.appendChild(el('label', { text: 'Fields' }))
        for (const fname of fieldList) {
          const val = fv(fields[fname])
          container.appendChild(
            el(
              'div',
              { class: 'form-group' },
              el('label', { text: fname }),
              (() => {
                const ta = el('textarea', { class: 'code-input', rows: '4', readonly: true })
                ta.value = val
                return ta
              })(),
            ),
          )
        }
      }
    } catch (err) {
      container.innerHTML = ''
      container.appendChild(el('div', { class: 'card', text: '加载失败: ' + err.message }))
    }
  }

  // ---- 租户管理页 ----
  async function loadTenantsPage() {
    const container = $('tenants-list')
    container.innerHTML = ''
    try {
      const resp = await API.listTenants()
      const tenants = resp.tenants || []
      if (tenants.length === 0) {
        container.appendChild(el('div', { class: 'card', text: '暂无租户。' }))
        return
      }
      for (const t of tenants) {
        const card = el('div', { class: 'card' })
        card.appendChild(el('div', { class: 'card-title', text: t.name }))
        card.appendChild(
          el('div', { class: 'card-meta', text: `Collections: ${t.collections || 0}` }),
        )
        const actions = el('div', { class: 'card-actions' })
        actions.appendChild(
          el('button', {
            class: 'btn btn-danger btn-sm',
            text: '删除',
            onclick: async () => {
              if (!confirm(`确认删除租户 "${t.name}"？所有数据将丢失！`)) return
              try {
                await API.dropTenant(t.name)
                toast('已删除 ' + t.name, 'success')
                await loadTenantsPage()
              } catch (err) {
                toast('删除失败: ' + err.message, 'error')
              }
            },
          }),
        )
        card.appendChild(actions)
        container.appendChild(card)
      }
    } catch (err) {
      if (err.status === 403) {
        container.appendChild(el('div', { class: 'card', text: '需要 admin 角色才能管理租户。' }))
      } else {
        toast('加载失败: ' + err.message, 'error')
      }
    }
  }

  async function doCreateTenant(e) {
    e.preventDefault()
    const name = $('ct-name').value.trim()
    if (!name) {
      toast('名称不能为空', 'error')
      return
    }
    try {
      const resp = await API.createTenant(name)
      if (resp.ok) {
        toast('租户已创建', 'success')
        $('create-tenant-modal').classList.add('hidden')
        $('create-tenant-form').reset()
        await loadTenantsPage()
      } else {
        toast('创建失败: ' + (resp.error || ''), 'error')
      }
    } catch (err) {
      toast('创建失败: ' + err.message, 'error')
    }
  }

  // ---- 密钥管理页 ----
  async function loadKeysPage() {
    try {
      const resp = await API.listTenants()
      const sel = $('keys-tenant')
      const current = sel.value
      sel.innerHTML = ''
      const tenants = resp.tenants || []
      for (const t of tenants) {
        sel.appendChild(el('option', { value: t.name, text: t.name }))
      }
      if (current) sel.value = current
      if (sel.value) await refreshKeys()
    } catch (err) {
      if (err.status === 403) {
        $('keys-list').innerHTML = ''
        $('keys-list').appendChild(
          el('div', { class: 'card', text: '需要 admin/tenant_admin 角色才能管理密钥。' }),
        )
      } else {
        toast('加载失败: ' + err.message, 'error')
      }
    }
  }

  async function refreshKeys() {
    const tenant = $('keys-tenant').value
    if (!tenant) return
    const container = $('keys-list')
    container.innerHTML = ''
    try {
      const resp = await API.listKeys(tenant)
      const keys = resp.keys || []
      if (keys.length === 0) {
        container.appendChild(el('div', { class: 'card', text: '该租户暂无 API Key。' }))
        return
      }
      const table = el('table')
      const thead = el('thead')
      const headRow = el('tr')
      for (const h of ['Key ID', '角色', 'Collection 白名单', '创建时间', '状态', '操作']) {
        headRow.appendChild(el('th', { text: h }))
      }
      thead.appendChild(headRow)
      table.appendChild(thead)
      const tbody = el('tbody')
      for (const k of keys) {
        const tr = el('tr')
        tr.appendChild(el('td', { text: k.key_id, style: 'font-family:monospace;font-size:12px' }))
        tr.appendChild(el('td', { text: k.role }))
        tr.appendChild(el('td', { text: (k.collections || []).join(', ') || '(全部)' }))
        tr.appendChild(
          el('td', { text: k.created_at ? new Date(k.created_at * 1000).toLocaleString() : '-' }),
        )
        tr.appendChild(el('td', { text: k.revoked ? '已吊销' : '有效' }))
        const revokeBtn = k.revoked
          ? el('span', { text: '-' })
          : el('button', {
              class: 'btn btn-danger btn-sm',
              text: '吊销',
              onclick: async () => {
                if (!confirm(`确认吊销 Key "${k.key_id}"？`)) return
                try {
                  await API.revokeKey(tenant, k.key_id)
                  toast('已吊销', 'success')
                  await refreshKeys()
                } catch (err) {
                  toast('吊销失败: ' + err.message, 'error')
                }
              },
            })
        tr.appendChild(el('td', {}, revokeBtn))
        tbody.appendChild(tr)
      }
      table.appendChild(tbody)
      container.appendChild(table)
    } catch (err) {
      toast('加载密钥失败: ' + err.message, 'error')
    }
  }

  async function doIssueKey(e) {
    e.preventDefault()
    const tenant = $('keys-tenant').value
    const role = $('ik-role').value
    const colsText = $('ik-collections').value.trim()
    const collections = colsText
      ? colsText
          .split(',')
          .map((s) => s.trim())
          .filter(Boolean)
      : []
    try {
      const resp = await API.issueKey(tenant, role, collections)
      $('issue-key-modal').classList.add('hidden')
      $('issue-key-form').reset()
      // show the key once
      const keyStr = resp.key || ''
      if (keyStr) {
        const modal = el('div', { class: 'modal' })
        const content = el('div', { class: 'modal-content' })
        content.appendChild(el('h3', { text: 'API Key 已签发' }))
        content.appendChild(el('p', { text: '明文仅此一次返回，请妥善保存：' }))
        const ta = el('textarea', {
          class: 'code-input',
          readonly: true,
          style: 'width:100%;height:60px',
        })
        ta.value = keyStr
        content.appendChild(ta)
        const actions = el('div', { class: 'modal-actions' })
        actions.appendChild(
          el('button', {
            class: 'btn btn-primary',
            text: '复制',
            onclick: () => {
              navigator.clipboard.writeText(keyStr)
              toast('已复制', 'success')
            },
          }),
        )
        actions.appendChild(
          el('button', { class: 'btn', text: '关闭', onclick: () => modal.remove() }),
        )
        content.appendChild(actions)
        modal.appendChild(content)
        document.body.appendChild(modal)
      }
      await refreshKeys()
    } catch (err) {
      toast('签发失败: ' + err.message, 'error')
    }
  }

  // ---- 统计页 ----
  async function loadStatsPage() {
    const container = $('stats-content')
    container.innerHTML = ''
    try {
      const resp = await API.getStats()
      // summary cards
      const summary = el('div', { class: 'stats-container' })
      summary.appendChild(statCard('总 Collections', resp.total_collections || 0))
      summary.appendChild(statCard('总活跃文档', resp.total_active_documents || 0))
      container.appendChild(summary)
      // per-tenant
      const tenants = resp.tenants || []
      if (tenants.length > 0) {
        container.appendChild(el('h3', { text: '分租户', style: 'margin-top:20px' }))
        const grid = el('div', { class: 'stats-container' })
        for (const t of tenants) {
          const card = el('div', { class: 'stat-card' })
          card.appendChild(el('h3', { text: t.name }))
          card.appendChild(el('div', { class: 'stat-value', text: String(t.collections || 0) }))
          card.appendChild(
            el('p', { text: `${t.active_documents || 0} 文档`, style: 'color:var(--text-muted)' }),
          )
          grid.appendChild(card)
        }
        container.appendChild(grid)
      }
    } catch (err) {
      if (err.status === 403) {
        container.appendChild(el('div', { class: 'card', text: '需要 admin 角色才能查看统计。' }))
      } else {
        toast('加载统计失败: ' + err.message, 'error')
      }
    }
  }

  function statCard(label, value) {
    const card = el('div', { class: 'stat-card' })
    card.appendChild(el('h3', { text: label }))
    card.appendChild(el('div', { class: 'stat-value', text: String(value) }))
    return card
  }

  // ---- 初始化 ----
  function init() {
    // Login
    $('login-form').addEventListener('submit', doLogin)
    $('logout-btn').addEventListener('click', doLogout)

    // Search
    $('search-btn').addEventListener('click', doSearch)
    $('search-text').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') doSearch()
    })

    // Collections
    $('create-collection-btn').addEventListener('click', () =>
      $('create-collection-modal').classList.remove('hidden'),
    )
    $('cc-cancel').addEventListener('click', () =>
      $('create-collection-modal').classList.add('hidden'),
    )
    $('create-collection-form').addEventListener('submit', doCreateCollection)

    // Documents
    $('docs-collection').addEventListener('change', () => {
      docsOffset = 0
      refreshDocuments()
    })
    $('docs-refresh').addEventListener('click', () => {
      docsOffset = 0
      refreshDocuments()
    })
    $('docs-import').addEventListener('click', () => $('import-modal').classList.remove('hidden'))
    $('import-cancel').addEventListener('click', () => $('import-modal').classList.add('hidden'))
    $('import-form').addEventListener('submit', doImportMarkdown)
    $('doc-detail-close').addEventListener('click', () =>
      $('doc-detail-modal').classList.add('hidden'),
    )

    // Tenants
    $('create-tenant-btn').addEventListener('click', () =>
      $('create-tenant-modal').classList.remove('hidden'),
    )
    $('ct-cancel').addEventListener('click', () => $('create-tenant-modal').classList.add('hidden'))
    $('create-tenant-form').addEventListener('submit', doCreateTenant)

    // Keys
    $('keys-tenant').addEventListener('change', refreshKeys)
    $('issue-key-btn').addEventListener('click', () =>
      $('issue-key-modal').classList.remove('hidden'),
    )
    $('ik-cancel').addEventListener('click', () => $('issue-key-modal').classList.add('hidden'))
    $('issue-key-form').addEventListener('submit', doIssueKey)

    // Router
    window.addEventListener('hashchange', navigateToRoute)

    // Check auth
    if (API.isLoggedIn()) {
      showApp()
      navigateToRoute()
    } else {
      showLogin()
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init)
  } else {
    init()
  }
})()
