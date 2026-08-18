<script setup>
import { ref, reactive, computed } from 'vue'
import {
  NCard,
  NButton,
  NTag,
  NPopconfirm,
  NModal,
  NForm,
  NFormItem,
  NInput,
  NText,
  NIcon,
  NDrawer,
  NDrawerContent,
  NDataTable,
  NSpin,
  NSelect,
  NEmpty,
  NDivider,
  NTooltip,
  useMessage,
} from 'naive-ui'
import {
  ChevronForwardOutline,
  FolderOpenOutline,
  BusinessOutline,
  DocumentTextOutline,
  AddOutline,
  TrashOutline,
  CloudUploadOutline,
  EyeOutline,
  SwapHorizontalOutline,
  RefreshOutline,
} from '@vicons/ionicons5'
import * as api from '../api/client'
import { session } from '../store/session'
import { fv, fvKind, pickTitle } from '../utils/fields'

const message = useMessage()
const isAdmin = session.role === 'admin'
const canManage = ['admin', 'tenant_admin'].includes(session.role)

// ---- tenant tree (hierarchical expandable list) ----
const tenants = reactive([])

async function loadTenants() {
  try {
    const resp = isAdmin ? await api.listTenants() : { tenants: [{ name: session.tenant }] }
    const names = (resp.tenants || []).map((t) => t.name)
    tenants.splice(0, tenants.length)
    for (const name of names) {
      const t = reactive({
        name,
        expanded: names.length === 1,
        loading: false,
        cols: null,
        active: 0,
        total: 0,
      })
      tenants.push(t)
      if (t.expanded) await loadCollections(t)
    }
  } catch (err) {
    message.error('加载租户失败: ' + err.message)
  }
}

async function loadCollections(t) {
  t.loading = true
  try {
    const resp = await api.listCollections(t.name)
    const cols = (resp.collections || []).map((c) =>
      reactive({
        name: c.name,
        active_documents: c.active_documents,
        expanded: false,
        loading: false,
        docs: null,
      }),
    )
    t.cols = cols
    t.active = cols.filter((c) => c.active_documents > 0).length
    t.total = cols.length
  } catch (err) {
    message.error(`加载 ${t.name} 的 collections 失败: ` + err.message)
  } finally {
    t.loading = false
  }
}

async function loadDocuments(t, c) {
  c.loading = true
  try {
    const docs = await api.listTopLevelDocuments(c.name, t.name)
    c.docs = docs.map((d) => {
      const fullTitle = pickTitle(d.docs?.[0]?.fields) || ''
      return {
        name: d.name,
        chunks: d.chunks,
        title: shortTitle(fullTitle),
        fullTitle,
        chunkDocs: d.docs || [],
      }
    })
    // update active_documents for display
    c.active_documents =
      (c.docs || []).reduce((s, d) => s + (d.chunks || 0), 0) || c.active_documents
  } catch (err) {
    message.error(`加载 ${c.name} 文档列表失败: ` + err.message)
  } finally {
    c.loading = false
  }
}

// chunk 的 title 形如「文档标题 > 章节标题」，文档名取第一段（上传文档的标题）
function shortTitle(full) {
  if (!full) return ''
  const parts = full
    .split('>')
    .map((s) => s.trim())
    .filter(Boolean)
  return parts.length ? parts[0] : full
}

async function reloadCollections(t) {
  await loadCollections(t)
  if (t.expanded) {
    for (const c of t.cols || []) {
      if (c.expanded) await loadDocuments(t, c)
    }
  }
}

async function reloadDocs(t, c) {
  if (c.expanded) await loadDocuments(t, c)
}

// ---- collection CRUD ----
async function doDrop(tenant, name) {
  try {
    await api.dropCollection(name, tenant)
    message.success('已删除 ' + name)
    await reloadCollections(findTenant(tenant))
  } catch (err) {
    message.error('删除失败: ' + err.message)
  }
}

async function doDeleteDoc(tenant, col, docId) {
  try {
    const all = await api.listDocuments(col, 0, 500, tenant)
    const chunks = (all.documents || []).filter((d) => d.id.startsWith(docId))
    for (const c of chunks) await api.deleteDocument(col, c.id, tenant)
    message.success(`已删除 ${docId}（${chunks.length} chunks）`)
    const t = findTenant(tenant)
    const c = findCol(t, col)
    if (t && c) await reloadDocs(t, c)
  } catch (err) {
    message.error('删除失败: ' + err.message)
  }
}

function findTenant(name) {
  return tenants.find((t) => t.name === name)
}
function findCol(t, name) {
  return (t?.cols || []).find((c) => c.name === name)
}

// ---- 新建 Collection modal ----
const createOpen = ref(false)
const creating = ref(false)
const createTenant = ref('')
const form = ref({
  name: '',
  default_analyzer: 'cjk_jieba',
  schema: JSON.stringify(
    {
      fields: [
        { name: 'title', type: 'text', indexed: true, stored: true },
        { name: 'content', type: 'text', indexed: true, stored: true, analyzer: 'cjk_jieba' },
        {
          name: 'content_vec',
          type: 'vector',
          dims: 1024,
          metric: 'cosine',
          source_field: 'content',
          mode: 'server',
        },
      ],
    },
    null,
    2,
  ),
})

function openCreate(tenant) {
  createTenant.value = tenant
  createOpen.value = true
}

async function doCreate() {
  if (!form.value.name.trim()) return message.warning('名称不能为空')
  let fields
  try {
    const parsed = JSON.parse(form.value.schema)
    fields = parsed.fields || parsed
    if (!Array.isArray(fields)) throw new Error('schema 需要 fields 数组')
  } catch (err) {
    return message.error('Schema JSON 解析失败: ' + err.message)
  }
  creating.value = true
  try {
    await api.createCollection(
      {
        name: form.value.name.trim(),
        default_analyzer: form.value.default_analyzer.trim() || 'cjk_jieba',
        fields,
      },
      createTenant.value,
    )
    message.success('Collection 已创建')
    createOpen.value = false
    form.value.name = ''
    const t = findTenant(createTenant.value)
    if (t) {
      t.expanded = true
      await reloadCollections(t)
    }
  } catch (err) {
    message.error('创建失败: ' + err.message)
  } finally {
    creating.value = false
  }
}

// ---- 导入 Markdown modal ----
const importOpen = ref(false)
const importing = ref(false)
const importTenant = ref('')
const importCol = ref('')
const importDocId = ref('')
const importForm = ref({ name: '', content: '', text_field: 'content' })

function openImport(tenant, col, docId) {
  importTenant.value = tenant
  importCol.value = col
  importDocId.value = docId || ''
  importForm.value = { name: docId || '', content: '', text_field: 'content' }
  importOpen.value = true
}

async function doImport() {
  if (!importForm.value.name.trim() || !importForm.value.content.trim())
    return message.warning('名称和内容不能为空')
  importing.value = true
  try {
    const resp = await api.importMarkdown(
      importCol.value,
      {
        name: importForm.value.name.trim(),
        content: importForm.value.content,
        text_field: importForm.value.text_field || 'content',
      },
      importTenant.value,
    )
    message.success(`已导入 ${resp.chunks ?? 0} chunks`)
    importOpen.value = false
    const t = findTenant(importTenant.value)
    const c = findCol(t, importCol.value)
    if (t && c) await reloadDocs(t, c)
  } catch (err) {
    message.error('导入失败: ' + err.message)
  } finally {
    importing.value = false
  }
}

// ---- 文档详情 drawer ----
const docDrawerOpen = ref(false)
const docDetail = ref({ tenant: '', col: '', docId: '', chunks: [], loading: false })

const docColumns = [
  {
    title: 'Chunk ID',
    key: 'id',
    render: (row) => row.id,
  },
  { title: '版本', key: 'version', width: 100, render: (row) => String(row.version ?? '') },
  {
    title: '操作',
    key: 'actions',
    width: 120,
    render: (row) => null,
  },
]

// 直接复用文档列表已聚合好的 chunk 数据，避免重新拉取时因分页上限漏数据
function openDocDetail(tenant, col, d) {
  const chunks = (d.chunkDocs || [])
    .filter((x) => api.docNameOf(x.id) === d.name)
    .slice()
    .sort((a, b) => (api.chunkIndexOf(a.id) ?? 0) - (api.chunkIndexOf(b.id) ?? 0))
  docDetail.value = { tenant, col, docId: d.name, title: d.title, chunks, loading: false }
  docDrawerOpen.value = true
}

async function doDeleteChunk(row) {
  try {
    await api.deleteDocument(docDetail.value.col, row.id, docDetail.value.tenant)
    message.success('已删除 ' + row.id)
    docDetail.value.chunks = docDetail.value.chunks.filter((x) => x.id !== row.id)
    const t = findTenant(docDetail.value.tenant)
    const c = findCol(t, docDetail.value.col)
    if (t && c) await reloadDocs(t, c)
  } catch (err) {
    message.error('删除失败: ' + err.message)
  }
}

// ---- chunk 查看 modal ----
const chunkViewOpen = ref(false)
const chunkView = ref({ loading: false, doc: null, fields: [], raw: '', id: '', version: 0 })

async function viewChunk(row) {
  chunkView.value = { loading: true, doc: null, fields: [], raw: '', id: row.id, version: 0 }
  chunkViewOpen.value = true
  try {
    const resp = await api.getDocument(docDetail.value.col, row.id, docDetail.value.tenant)
    chunkView.value.doc = resp.document || resp
    chunkView.value.version = chunkView.value.doc?.version ?? 0
    const fields = chunkView.value.doc?.fields || {}
    chunkView.value.fields = Object.entries(fields).map(([name, val]) => {
      const info = fvKind(val)
      const sval = info.kind === 'v' ? `[${info.value} floats]` : info.value
      return { name, kind: info.kind, val: sval }
    })
  } catch (err) {
    message.error('加载失败: ' + err.message)
  } finally {
    chunkView.value.loading = false
  }
}

// ---- 迁移 modal ----
const moveOpen = ref(false)
const moveKind = ref('collection')
const moveTenant = ref('')
const moveName = ref('')
const moveTarget = ref('')
const moveTenantOptions = ref([])

async function openMigrate(tenant, name, kind) {
  moveKind.value = kind
  moveTenant.value = tenant
  moveName.value = name
  moveTarget.value = ''
  moveOpen.value = true
  try {
    const resp = await api.listTenants()
    moveTenantOptions.value = (resp.tenants || [])
      .map((t) => t.name)
      .filter((t) => t !== tenant)
      .map((t) => ({ label: t, value: t }))
  } catch (err) {
    message.error('加载租户列表失败: ' + err.message)
  }
}

async function doMigrate() {
  if (!moveTarget.value) return message.warning('请选择目标租户')
  try {
    if (moveKind.value === 'collection') {
      const resp = await api.moveCollection(moveTenant.value, moveName.value, moveTarget.value)
      message.success(`Collection 已迁移到 ${moveTarget.value}（${resp.documents ?? 0} 文档）`)
    } else {
      await api.moveKey(moveTenant.value, moveName.value, moveTarget.value)
      message.success(`Key 已迁移到 ${moveTarget.value}`)
    }
    moveOpen.value = false
    const src = findTenant(moveTenant.value)
    if (src) await reloadCollections(src)
    const dst = findTenant(moveTarget.value)
    if (dst && dst.expanded) await reloadCollections(dst)
  } catch (err) {
    message.error('迁移失败: ' + err.message)
  }
}

function toggleTenant(t) {
  t.expanded = !t.expanded
  if (t.expanded && !t.cols) loadCollections(t)
}
function toggleCol(t, c) {
  c.expanded = !c.expanded
  if (c.expanded && !c.docs) loadDocuments(t, c)
}

loadTenants()
</script>

<template>
  <div class="mss-stack">
    <n-card :bordered="true">
      <div class="page-head">
        <div>
          <div class="page-title">数据管理</div>
          <n-text depth="3" style="font-size: 13px">
            租户 → Collection → 文档，逐级展开查看与操作。
          </n-text>
        </div>
        <n-button quaternary @click="loadTenants">
          <template #icon><n-icon :component="RefreshOutline" /></template>
          刷新
        </n-button>
      </div>
    </n-card>

    <n-card :bordered="true" class="data-card">
      <n-empty v-if="tenants.length === 0" description="暂无租户数据" style="padding: 32px 0" />

      <!-- 租户层 -->
      <div v-for="t in tenants" :key="t.name" class="tree-node">
        <div class="tree-row level-0" @click="toggleTenant(t)">
          <n-icon
            class="chevron"
            :class="{ open: t.expanded }"
            :component="ChevronForwardOutline"
          />
          <n-icon :component="BusinessOutline" color="var(--mss-brand)" class="row-icon" />
          <span class="row-label">{{ t.name }}</span>
          <n-tag v-if="t.cols" size="tiny" :bordered="false" type="info" class="row-meta">
            {{ t.total }} collections
          </n-tag>
          <div class="row-actions" @click.stop>
            <n-button
              v-if="isAdmin"
              size="tiny"
              quaternary
              type="primary"
              @click="openCreate(t.name)"
            >
              <template #icon><n-icon :component="AddOutline" /></template>
              新建 Collection
            </n-button>
          </div>
        </div>

        <!-- Collection 层 -->
        <div v-show="t.expanded" class="tree-children">
          <n-spin :show="t.loading" size="small">
            <template v-if="t.cols && t.cols.length > 0">
              <div v-for="c in t.cols" :key="c.name" class="tree-node">
                <div class="tree-row level-1" @click="toggleCol(t, c)">
                  <n-icon
                    class="chevron"
                    :class="{ open: c.expanded }"
                    :component="ChevronForwardOutline"
                  />
                  <n-icon :component="FolderOpenOutline" color="#f0a020" class="row-icon" />
                  <span class="row-label">{{ c.name }}</span>
                  <n-tag size="tiny" :bordered="false" type="info" class="row-meta">
                    {{ c.active_documents }} chunks
                  </n-tag>
                  <div class="row-actions" @click.stop>
                    <n-button
                      v-if="canManage"
                      size="tiny"
                      quaternary
                      type="primary"
                      @click="openImport(t.name, c.name)"
                    >
                      <template #icon><n-icon :component="CloudUploadOutline" /></template>
                      上传文档
                    </n-button>
                    <n-button
                      v-if="isAdmin"
                      size="tiny"
                      quaternary
                      type="info"
                      @click="openMigrate(t.name, c.name, 'collection')"
                    >
                      <template #icon><n-icon :component="SwapHorizontalOutline" /></template>
                      迁移
                    </n-button>
                    <n-popconfirm v-if="canManage" @positive-click="doDrop(t.name, c.name)">
                      <template #trigger>
                        <n-button size="tiny" quaternary type="error">
                          <template #icon><n-icon :component="TrashOutline" /></template>
                          删除
                        </n-button>
                      </template>
                      删除 Collection "{{ c.name }}"？不可恢复。
                    </n-popconfirm>
                  </div>
                </div>

                <!-- 文档层 -->
                <div v-show="c.expanded" class="tree-children">
                  <n-spin :show="c.loading" size="small">
                    <template v-if="c.docs && c.docs.length > 0">
                      <div v-for="d in c.docs" :key="d.name" class="tree-row level-2">
                        <span class="chevron-placeholder" />
                        <n-icon :component="DocumentTextOutline" color="#888" class="row-icon" />
                        <span class="row-label" :title="d.fullTitle || d.name">
                          {{ d.title || d.name }}
                        </span>
                        <span v-if="d.title" class="doc-id mono">{{ d.name }}</span>
                        <n-tag size="tiny" :bordered="false" type="info" class="row-meta">
                          {{ d.chunks }} chunks
                        </n-tag>
                        <div class="row-actions">
                          <n-button
                            size="tiny"
                            quaternary
                            @click="openDocDetail(t.name, c.name, d)"
                          >
                            <template #icon><n-icon :component="EyeOutline" /></template>
                            查看
                          </n-button>
                          <n-button
                            v-if="canManage"
                            size="tiny"
                            quaternary
                            type="primary"
                            @click="openImport(t.name, c.name, d.name)"
                          >
                            <template #icon><n-icon :component="CloudUploadOutline" /></template>
                            替换
                          </n-button>
                          <n-popconfirm
                            v-if="canManage"
                            @positive-click="doDeleteDoc(t.name, c.name, d.name)"
                          >
                            <template #trigger>
                              <n-button size="tiny" quaternary type="error">
                                <template #icon><n-icon :component="TrashOutline" /></template>
                                删除
                              </n-button>
                            </template>
                            删除文档 "{{ d.name }}"（含 {{ d.chunks }} chunks）？
                          </n-popconfirm>
                        </div>
                      </div>
                    </template>
                    <n-empty
                      v-else-if="c.docs && c.docs.length === 0"
                      description="暂无文档"
                      size="small"
                      style="padding: 12px 0"
                    />
                  </n-spin>
                </div>
              </div>
            </template>
            <n-empty
              v-else-if="t.cols && t.cols.length === 0"
              description="暂无 Collection"
              size="small"
              style="padding: 12px 0"
            />
          </n-spin>
        </div>
      </div>
    </n-card>

    <!-- 新建 Collection Modal -->
    <n-modal
      v-model:show="createOpen"
      preset="card"
      title="新建 Collection"
      style="width: 680px; max-width: 94vw"
    >
      <n-form label-placement="top">
        <n-text depth="3" style="font-size: 12px">将创建到租户：{{ createTenant }}</n-text>
        <n-form-item label="名称" style="margin-top: 12px">
          <n-input v-model:value="form.name" placeholder="loom-kb" :disabled="creating" />
        </n-form-item>
        <n-form-item label="默认 Analyzer">
          <n-input
            v-model:value="form.default_analyzer"
            placeholder="cjk_jieba"
            :disabled="creating"
          />
        </n-form-item>
        <n-form-item label="Schema（JSON，fields 数组）">
          <n-input
            v-model:value="form.schema"
            type="textarea"
            :rows="14"
            class="schema-input"
            spellcheck="false"
            placeholder='{"fields":[...]}'
            :disabled="creating"
          />
        </n-form-item>
        <div class="modal-actions">
          <n-button @click="createOpen = false" :disabled="creating">取消</n-button>
          <n-button type="primary" :loading="creating" @click="doCreate">创建</n-button>
        </div>
      </n-form>
    </n-modal>

    <!-- 导入 Markdown Modal -->
    <n-modal
      v-model:show="importOpen"
      preset="card"
      :title="importDocId ? '替换文档：' + importDocId : '上传 Markdown 文档'"
      style="width: 720px; max-width: 94vw"
    >
      <n-form label-placement="top">
        <n-form-item label="文档名称（顶层 id 前缀）">
          <n-input v-model:value="importForm.name" :disabled="importing" />
        </n-form-item>
        <n-form-item label="用于分块的文本字段">
          <n-input v-model:value="importForm.text_field" :disabled="importing" />
        </n-form-item>
        <n-form-item label="Markdown 内容">
          <n-input
            v-model:value="importForm.content"
            type="textarea"
            :rows="12"
            class="schema-input"
            spellcheck="false"
            placeholder="# 标题\n\n正文..."
            :disabled="importing"
          />
        </n-form-item>
        <n-text depth="3" style="font-size: 12px">
          导入到 Collection「{{ importCol }}」· 租户「{{ importTenant }}」，同名文档会被替换。
        </n-text>
        <div class="modal-actions" style="margin-top: 12px">
          <n-button @click="importOpen = false" :disabled="importing">取消</n-button>
          <n-button type="primary" :loading="importing" @click="doImport">导入</n-button>
        </div>
      </n-form>
    </n-modal>

    <!-- 文档详情 Drawer -->
    <n-drawer v-model:show="docDrawerOpen" :width="620" placement="right">
      <n-drawer-content
        :title="`${docDetail.title || docDetail.docId} 的 Chunks`"
        :native-scrollbar="false"
      >
        <n-spin :show="docDetail.loading">
          <div v-for="row in docDetail.chunks" :key="row.id" class="chunk-row">
            <span class="chunk-id">{{ row.id }}</span>
            <span class="chunk-ver">v{{ row.version ?? '' }}</span>
            <div class="chunk-actions">
              <n-button size="tiny" quaternary @click="viewChunk(row)">
                <template #icon><n-icon :component="EyeOutline" /></template>
                查看
              </n-button>
              <n-popconfirm v-if="canManage" @positive-click="doDeleteChunk(row)">
                <template #trigger>
                  <n-button size="tiny" quaternary type="error">
                    <template #icon><n-icon :component="TrashOutline" /></template>
                    删除
                  </n-button>
                </template>
                删除 chunk "{{ row.id }}"？
              </n-popconfirm>
            </div>
          </div>
          <n-empty
            v-if="!docDetail.loading && docDetail.chunks.length === 0"
            description="暂无 chunk"
            style="padding: 24px 0"
          />
        </n-spin>
        <n-divider />
        <n-text depth="3" style="font-size: 12px"> 点击查看 chunk 字段；删除不可恢复。 </n-text>
      </n-drawer-content>
    </n-drawer>

    <!-- chunk 查看 Modal -->
    <n-modal
      v-model:show="chunkViewOpen"
      preset="card"
      :title="chunkView.id"
      style="width: 680px; max-width: 94vw"
    >
      <n-spin :show="chunkView.loading">
        <n-text depth="3" style="font-size: 12px">version: {{ chunkView.version }}</n-text>
        <div style="margin-top: 12px">
          <div v-for="f in chunkView.fields" :key="f.name" style="margin-bottom: 12px">
            <n-text depth="2" style="font-weight: 600">{{ f.name }}</n-text>
            <n-tag size="tiny" :bordered="false" style="margin-left: 6px">{{ f.kind }}</n-tag>
            <n-input
              type="textarea"
              :value="f.val"
              readonly
              :rows="Math.min(10, Math.max(2, f.val.split('\n').length))"
              class="schema-input"
              style="margin-top: 4px"
            />
          </div>
        </div>
      </n-spin>
      <div class="modal-actions">
        <n-button @click="chunkViewOpen = false">关闭</n-button>
      </div>
    </n-modal>

    <!-- 迁移 Modal -->
    <n-modal
      v-model:show="moveOpen"
      preset="card"
      :title="moveKind === 'collection' ? '迁移 Collection' : '迁移 API Key'"
      style="width: 460px; max-width: 92vw"
    >
      <n-form label-placement="top">
        <n-text depth="3" style="font-size: 12px">
          将「{{ moveName }}」从租户 {{ moveTenant }} 迁移到目标租户。{{
            moveKind === 'collection'
              ? 'Schema 原样重建，全部文档复制完成后才删除源；目标已存在同名时不覆盖。'
              : '改绑 key 的租户归属，立即生效。'
          }}
        </n-text>
        <n-form-item label="目标租户" style="margin-top: 12px">
          <n-select
            v-model:value="moveTarget"
            :options="moveTenantOptions"
            placeholder="选择目标租户"
          />
        </n-form-item>
        <div class="modal-actions">
          <n-button @click="moveOpen = false">取消</n-button>
          <n-button type="primary" @click="doMigrate">迁移</n-button>
        </div>
      </n-form>
    </n-modal>
  </div>
</template>

<style scoped>
.page-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.page-title {
  font-size: 16px;
  font-weight: 700;
  margin-bottom: 2px;
}
.schema-input {
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
  font-size: 13px;
}
.modal-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.mono {
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
}
.doc-id {
  font-size: 12px;
  color: #999;
  flex-shrink: 0;
}

.data-card :deep(.n-card__content) {
  padding: 4px 0;
}

.tree-node {
  margin-bottom: 1px;
}

.tree-row {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 8px;
  border-radius: 4px;
  cursor: pointer;
  transition: background 0.12s;
  min-height: 36px;
}
.tree-row:hover {
  background: rgba(99, 102, 241, 0.06);
}

.chevron {
  flex-shrink: 0;
  transition: transform 0.18s ease;
  font-size: 16px;
  opacity: 0.6;
}
.chevron.open {
  transform: rotate(90deg);
}
.chevron-placeholder {
  display: inline-block;
  width: 16px;
  flex-shrink: 0;
}

.row-icon {
  flex-shrink: 0;
  font-size: 18px;
}

.row-label {
  font-size: 14px;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  flex: 0 1 auto;
  min-width: 60px;
}

.row-meta {
  flex-shrink: 0;
}

.row-actions {
  margin-left: auto;
  display: flex;
  align-items: center;
  gap: 2px;
  opacity: 0.6;
  transition: opacity 0.15s;
}
.tree-row:hover .row-actions {
  opacity: 1;
}

.tree-children {
  margin-left: 24px;
  border-left: 1px solid rgba(128, 128, 128, 0.12);
  padding-left: 4px;
}

.level-1 .row-label,
.level-2 .row-label {
  font-weight: 500;
}
.level-2 .row-label {
  font-size: 13px;
}

.chunk-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 0;
  border-bottom: 1px solid rgba(128, 128, 128, 0.08);
}
.chunk-id {
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
  font-size: 12px;
  flex: 1;
}
.chunk-ver {
  font-size: 12px;
  color: var(--mss-text-3, #999);
}
.chunk-actions {
  display: flex;
  gap: 4px;
}
</style>
