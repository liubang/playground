<script setup>
import { ref, onMounted, h } from 'vue'
import {
  NCard,
  NSpace,
  NButton,
  NSelect,
  NDataTable,
  NTag,
  NPopconfirm,
  NModal,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NIcon,
  NText,
  NSpin,
  NDivider,
  useMessage,
  NEmpty,
} from 'naive-ui'
import {
  RefreshOutline,
  DownloadOutline,
  EyeOutline,
  CreateOutline,
  TrashOutline,
} from '@vicons/ionicons5'
import * as api from '../api/client'
import { fv, fvKind, pickBody } from '../utils/fields'
import { renderMarkdown } from '../utils/markdown'

const message = useMessage()

const collections = ref([])
const collection = ref('')
const docsLoading = ref(false)
const rows = ref([])
const total = ref(0)
const offset = ref(0)
const page = ref(1)
const pageSize = 50

async function loadCollections() {
  try {
    const resp = await api.listCollections()
    collections.value = (resp.collections || []).map((c) => ({
      label: `${c.name} (${c.active_documents})`,
      value: c.name,
    }))
    if (!collection.value && collections.value.length > 0) {
      collection.value = collections.value[0].value
      loadDocs()
    } else if (collection.value) {
      loadDocs()
    }
  } catch (err) {
    message.error('加载 Collections 失败: ' + err.message)
  }
}

function fieldSummary(doc) {
  const fields = doc.fields || {}
  const parts = Object.entries(fields)
    .map(([k, v]) => `${k}: ${String(fv(v)).slice(0, 60)}`)
    .slice(0, 3)
  const s = parts.join(' · ')
  return s.length > 120 ? s.slice(0, 120) + '…' : s
}

async function loadDocs() {
  if (!collection.value) return
  docsLoading.value = true
  try {
    const resp = await api.listDocuments(collection.value, (page.value - 1) * pageSize, pageSize)
    rows.value = resp.documents || []
    total.value = resp.total || 0
  } catch (err) {
    message.error('加载文档失败: ' + err.message)
  } finally {
    docsLoading.value = false
  }
}

function onCollectionChange() {
  page.value = 1
  offset.value = 0
  loadDocs()
}

function onPageChange(p) {
  page.value = p
  loadDocs()
}

const columns = [
  {
    title: 'ID',
    key: 'id',
    width: 220,
    render: (row) =>
      h('span', { style: 'font-family:monospace;font-size:12px;word-break:break-all' }, row.id),
  },
  {
    title: 'Fields 摘要',
    key: 'fields',
    render: (row) =>
      h(NText, { depth: 3, style: 'font-size:13px' }, { default: () => fieldSummary(row) }),
  },
  {
    title: 'Version',
    key: 'version',
    width: 90,
    render: (row) =>
      h(
        NTag,
        { size: 'small', bordered: false, type: 'default' },
        { default: () => String(row.version) },
      ),
  },
  {
    title: '操作',
    key: 'actions',
    width: 210,
    render: (row) =>
      h(NSpace, { size: 4 }, () => [
        h(
          NButton,
          { size: 'small', quaternary: true, onClick: () => openDetail(row) },
          { icon: () => h(NIcon, { component: EyeOutline }), default: () => '查看' },
        ),
        h(
          NButton,
          { size: 'small', quaternary: true, type: 'primary', onClick: () => openEdit(row) },
          { icon: () => h(NIcon, { component: CreateOutline }), default: () => '编辑' },
        ),
        h(
          NPopconfirm,
          { onPositiveClick: () => doDelete(row) },
          {
            trigger: () =>
              h(
                NButton,
                { size: 'small', quaternary: true, type: 'error' },
                { icon: () => h(NIcon, { component: TrashOutline }), default: () => '删除' },
              ),
            default: () => `确认删除文档 "${row.id}"？`,
          },
        ),
      ]),
  },
]

async function doDelete(row) {
  try {
    await api.deleteDocument(collection.value, row.id)
    message.success('已删除')
    loadDocs()
  } catch (err) {
    message.error('删除失败: ' + err.message)
  }
}

// ---- 导入 Markdown ----
const importOpen = ref(false)
const importing = ref(false)
const importFileName = ref('')
const fileInput = ref(null)
const importForm = ref({
  name: '',
  content: '',
  strategy: 'markdown',
  chunk_size: 1000,
  chunk_overlap: 100,
})

// 从本地文件读入 Markdown 全文；文件名（去扩展名）在名称为空时回填
async function onFilePick(e) {
  const file = e.target.files?.[0]
  e.target.value = '' // 允许重复选择同一文件
  if (!file) return
  if (file.size > 10 * 1024 * 1024) return message.warning('文件过大（上限 10MB）')
  importForm.value.content = await file.text()
  importFileName.value = file.name
  if (!importForm.value.name.trim()) {
    importForm.value.name = file.name.replace(/\.(md|markdown|txt)$/i, '')
  }
}

async function doImport() {
  if (!importForm.value.name.trim()) return message.warning('文档名称不能为空')
  if (!importForm.value.content.trim()) return message.warning('内容不能为空')
  importing.value = true
  try {
    const resp = await api.importMarkdown(collection.value, {
      name: importForm.value.name.trim(),
      content: importForm.value.content,
      strategy: importForm.value.strategy,
      chunk_size: importForm.value.chunk_size,
      chunk_overlap: importForm.value.chunk_overlap,
    })
    message.success(`导入成功：${resp.chunks || 0} 个 chunk`)
    importOpen.value = false
    importForm.value.name = ''
    importForm.value.content = ''
    importFileName.value = ''
    loadDocs()
  } catch (err) {
    message.error('导入失败: ' + err.message)
  } finally {
    importing.value = false
  }
}

// ---- 详情 ----
const detailOpen = ref(false)
const detailLoading = ref(false)
const detail = ref(null)
const detailFields = ref([])

async function openDetail(row) {
  detailOpen.value = true
  detailLoading.value = true
  detail.value = null
  detailFields.value = []
  try {
    const resp = await api.getDocument(collection.value, row.id)
    if (!resp.found) {
      message.warning('文档不存在')
      detailOpen.value = false
      return
    }
    detail.value = resp.document
    detailFields.value = Object.entries(resp.document.fields || {})
  } catch (err) {
    message.error('加载详情失败: ' + err.message)
    detailOpen.value = false
  } finally {
    detailLoading.value = false
  }
}

const isTextKey = (k) => ['content', 'body', 'text', 'markdown', 'content_md'].includes(k)

// ---- 编辑 ----
const editOpen = ref(false)
const editLoading = ref(false)
const editFields = ref([]) // [{ name, kind, value }]
const editVersion = ref(0)
const editId = ref('')

async function openEdit(row) {
  editOpen.value = true
  editLoading.value = true
  editId.value = row.id
  editVersion.value = row.version || 0
  editFields.value = []
  try {
    const resp = await api.getDocument(collection.value, row.id)
    const doc = resp.document || {}
    editVersion.value = doc.version || 0
    editFields.value = Object.entries(doc.fields || {})
      .filter(([k, v]) => {
        const { kind } = fvKind(v)
        return kind !== 'v' // 向量字段由服务端维护，不在此编辑
      })
      .map(([k, v]) => ({ name: k, ...fvKind(v) }))
  } catch (err) {
    message.error('加载文档失败: ' + err.message)
    editOpen.value = false
  } finally {
    editLoading.value = false
  }
}

async function doSave() {
  const fields = {}
  for (const f of editFields.value) {
    if (f.kind === 'n') {
      const num = Number(f.value)
      if (f.value.trim() !== '' && !Number.isNaN(num)) fields[f.name] = { n: num }
    } else {
      fields[f.name] = { s: f.value }
    }
  }
  try {
    await api.upsertDocument(collection.value, editId.value, { version: editVersion.value, fields })
    message.success('已保存')
    editOpen.value = false
    loadDocs()
  } catch (err) {
    message.error('保存失败: ' + err.message)
  }
}

onMounted(loadCollections)
</script>

<template>
  <div class="mss-stack">
    <n-card :bordered="true">
      <div class="page-head">
        <div class="head-left">
          <n-select
            v-model:value="collection"
            :options="collections"
            placeholder="选择 Collection"
            style="width: 260px"
            filterable
            clearable
            @update:value="onCollectionChange"
          />
          <n-button quaternary @click="loadDocs">
            <template #icon><n-icon :component="RefreshOutline" /></template>
            刷新
          </n-button>
        </div>
        <n-button type="primary" @click="importOpen = true" :disabled="!collection">
          <template #icon><n-icon :component="DownloadOutline" /></template>
          导入 Markdown
        </n-button>
      </div>
    </n-card>

    <n-card :bordered="true">
      <n-data-table
        :columns="columns"
        :data="rows"
        :loading="docsLoading"
        :row-key="(r) => r.id"
        :bordered="false"
        :pagination="{
          page: page,
          pageCount: Math.max(1, Math.ceil(total / pageSize)),
          itemCount: total,
          pageSize,
          showSizePicker: false,
          prefix: ({ itemCount }) => `共 ${itemCount} 条`,
          onUpdatePage: onPageChange,
        }"
      />
      <n-empty
        v-if="!docsLoading && rows.length === 0"
        description="该 Collection 暂无文档"
        style="padding: 32px 0"
      />
    </n-card>

    <!-- 导入 Modal -->
    <n-modal
      v-model:show="importOpen"
      preset="card"
      title="导入 Markdown"
      style="width: 720px; max-width: 94vw"
    >
      <n-form label-placement="top">
        <n-form-item label="文档名称（chunk id 前缀）">
          <n-input v-model:value="importForm.name" placeholder="my-doc" :disabled="importing" />
        </n-form-item>
        <n-form-item>
          <template #label>
            <div class="content-label">
              <span>Markdown 内容</span>
              <n-button size="tiny" tertiary @click="fileInput?.click()" :disabled="importing">
                选择 .md 文件…
              </n-button>
              <n-text v-if="importFileName" depth="3" style="font-size: 12px">
                {{ importFileName }}
              </n-text>
            </div>
          </template>
          <n-input
            v-model:value="importForm.content"
            type="textarea"
            :rows="14"
            class="code-input"
            placeholder="# Title&#10;Content..."
            :disabled="importing"
          />
          <input
            ref="fileInput"
            type="file"
            accept=".md,.markdown,.txt"
            style="display: none"
            @change="onFilePick"
          />
        </n-form-item>
        <div class="form-row">
          <n-form-item label="切分策略" style="flex: 1">
            <n-select
              v-model:value="importForm.strategy"
              :options="[
                { label: 'Markdown（标题感知）', value: 'markdown' },
                { label: '定长', value: 'fixed' },
              ]"
              :disabled="importing"
            />
          </n-form-item>
          <n-form-item label="Chunk Size">
            <n-input-number
              v-model:value="importForm.chunk_size"
              :min="100"
              :step="100"
              style="width: 140px"
              :disabled="importing"
            />
          </n-form-item>
          <n-form-item label="Chunk Overlap">
            <n-input-number
              v-model:value="importForm.chunk_overlap"
              :min="0"
              :step="10"
              style="width: 140px"
              :disabled="importing"
            />
          </n-form-item>
        </div>
        <div class="modal-actions">
          <n-button @click="importOpen = false" :disabled="importing">取消</n-button>
          <n-button type="primary" :loading="importing" @click="doImport">导入</n-button>
        </div>
      </n-form>
    </n-modal>

    <!-- 详情 Modal -->
    <n-modal
      v-model:show="detailOpen"
      preset="card"
      title="文档详情"
      style="width: 760px; max-width: 94vw"
    >
      <n-spin :show="detailLoading">
        <template v-if="detail">
          <n-tag size="small" :bordered="false" type="info">version {{ detail.version }}</n-tag>
          <n-divider style="margin: 10px 0" />
          <div v-for="[k, v] in detailFields" :key="k" class="detail-field">
            <n-text strong style="font-size: 13px">{{ k }}</n-text>
            <div
              v-if="isTextKey(k) && fv(v)"
              class="mss-markdown detail-md"
              v-html="renderMarkdown(fv(v))"
            />
            <n-text v-else depth="2" style="font-size: 13px; word-break: break-all">{{
              fv(v)
            }}</n-text>
          </div>
        </template>
      </n-spin>
    </n-modal>

    <!-- 编辑 Modal -->
    <n-modal
      v-model:show="editOpen"
      preset="card"
      title="编辑文档"
      style="width: 680px; max-width: 94vw"
    >
      <n-spin :show="editLoading">
        <n-text depth="3" style="font-size: 12px"
          >ID: {{ editId }} · 保存将按当前 version 乐观锁写入</n-text
        >
        <n-divider style="margin: 10px 0" />
        <n-form label-placement="top">
          <n-form-item v-for="f in editFields" :key="f.name" :label="f.name">
            <n-input-number
              v-if="f.kind === 'n'"
              v-model:value="f.value"
              style="width: 100%"
              placeholder="number"
            />
            <n-input
              v-else
              v-model:value="f.value"
              type="textarea"
              :rows="f.value.length > 200 ? 8 : 3"
              class="code-input"
            />
          </n-form-item>
        </n-form>
        <div class="modal-actions">
          <n-button @click="editOpen = false">取消</n-button>
          <n-button type="primary" @click="doSave">保存</n-button>
        </div>
      </n-spin>
    </n-modal>
  </div>
</template>

<style scoped>
.content-label {
  display: flex;
  align-items: center;
  gap: 10px;
  width: 100%;
}
.page-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}
.head-left {
  display: flex;
  align-items: center;
  gap: 12px;
}
.form-row {
  display: flex;
  gap: 12px;
}
.code-input {
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
  font-size: 13px;
}
.modal-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 4px;
}
.detail-field {
  margin-bottom: 10px;
}
.detail-md {
  font-size: 13px;
  max-height: 320px;
  overflow-y: auto;
  padding: 10px 12px;
  border-radius: 8px;
  background: rgba(127, 127, 127, 0.08);
  margin-top: 4px;
}
</style>
