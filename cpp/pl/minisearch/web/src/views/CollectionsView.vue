<script setup>
import { ref, onMounted, h } from 'vue'
import {
  NCard,
  NSpace,
  NButton,
  NDataTable,
  NTag,
  NPopconfirm,
  NModal,
  NForm,
  NFormItem,
  NInput,
  NText,
  useMessage,
  NEmpty,
} from 'naive-ui'
import { AddOutline, TrashOutline, FolderOpenOutline } from '@vicons/ionicons5'
import * as api from '../api/client'
import { session } from '../store/session'

const message = useMessage()
const loading = ref(false)
const rows = ref([])

const canManage = ['admin', 'tenant_admin'].includes(session.role)

const columns = [
  {
    title: '名称',
    key: 'name',
    render: (row) =>
      h('div', { style: 'display:flex;align-items:center;gap:8px;font-weight:600' }, [
        h(NIcon, { component: FolderOpenOutline, color: '#6366f1' }),
        row.name,
      ]),
  },
  {
    title: '活跃文档',
    key: 'active_documents',
    width: 140,
    render: (row) =>
      h(
        NTag,
        { size: 'small', bordered: false, type: 'info' },
        { default: () => String(row.active_documents) },
      ),
  },
  {
    title: '操作',
    key: 'actions',
    width: 120,
    render: (row) =>
      h(
        NPopconfirm,
        {
          onPositiveClick: () => doDrop(row.name),
        },
        {
          trigger: () =>
            h(
              NButton,
              { size: 'small', type: 'error', tertiary: true, disabled: !canManage },
              { icon: () => h(NIcon, { component: TrashOutline }), default: () => '删除' },
            ),
          default: () => `确认删除 Collection "${row.name}"？该操作不可恢复。`,
        },
      ),
  },
]

async function load() {
  loading.value = true
  try {
    const resp = await api.listCollections()
    rows.value = resp.collections || []
  } catch (err) {
    message.error('加载失败: ' + err.message)
  } finally {
    loading.value = false
  }
}

async function doDrop(name) {
  try {
    await api.dropCollection(name)
    message.success('已删除 ' + name)
    load()
  } catch (err) {
    message.error('删除失败: ' + err.message)
  }
}

// ---- 新建 Collection ----
const modalOpen = ref(false)
const creating = ref(false)
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

async function doCreate() {
  if (!form.value.name.trim()) {
    message.warning('名称不能为空')
    return
  }
  let fields
  try {
    const parsed = JSON.parse(form.value.schema)
    fields = parsed.fields || parsed
    if (!Array.isArray(fields)) throw new Error('schema 需要 fields 数组')
  } catch (err) {
    message.error('Schema JSON 解析失败: ' + err.message)
    return
  }
  creating.value = true
  try {
    await api.createCollection({
      name: form.value.name.trim(),
      default_analyzer: form.value.default_analyzer.trim() || 'cjk_jieba',
      fields,
    })
    message.success('Collection 已创建')
    modalOpen.value = false
    form.value.name = ''
    load()
  } catch (err) {
    message.error('创建失败: ' + err.message)
  } finally {
    creating.value = false
  }
}

onMounted(load)
</script>

<template>
  <div class="mss-stack">
    <n-card :bordered="true">
      <div class="page-head">
        <div>
          <div class="page-title">Collections</div>
          <n-text depth="3" style="font-size: 13px"
            >管理当前租户的索引集合（新建需 schema 定义）</n-text
          >
        </div>
        <n-button v-if="canManage" type="primary" @click="modalOpen = true">
          <template #icon><n-icon :component="AddOutline" /></template>
          新建 Collection
        </n-button>
      </div>
    </n-card>

    <n-card :bordered="true">
      <n-data-table
        :columns="columns"
        :data="rows"
        :loading="loading"
        :row-key="(r) => r.name"
        :pagination="{ pageSize: 12 }"
        :bordered="false"
      />
      <n-empty
        v-if="!loading && rows.length === 0"
        description="暂无 Collection"
        style="padding: 32px 0"
      />
    </n-card>

    <n-modal
      v-model:show="modalOpen"
      preset="card"
      title="新建 Collection"
      style="width: 680px; max-width: 94vw"
    >
      <n-form label-placement="top">
        <n-form-item label="名称">
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
          <n-button @click="modalOpen = false" :disabled="creating">取消</n-button>
          <n-button type="primary" :loading="creating" @click="doCreate">创建</n-button>
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
</style>
