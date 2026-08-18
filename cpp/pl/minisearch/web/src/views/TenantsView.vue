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
  NIcon,
  useMessage,
  NEmpty,
} from 'naive-ui'
import { AddOutline, TrashOutline, BusinessOutline } from '@vicons/ionicons5'
import * as api from '../api/client'
import { session } from '../store/session'

const message = useMessage()
const loading = ref(false)
const rows = ref([])
const isAdmin = session.role === 'admin'

const columns = [
  {
    title: '租户',
    key: 'name',
    render: (row) =>
      h('div', { style: 'display:flex;align-items:center;gap:8px;font-weight:600' }, [
        h(NIcon, { component: BusinessOutline, color: 'var(--mss-brand)' }),
        row.name,
      ]),
  },
  {
    title: 'Collections',
    key: 'collections',
    width: 140,
    render: (row) =>
      h(
        NTag,
        { size: 'small', bordered: false, type: 'info' },
        { default: () => String(row.collections) },
      ),
  },
  {
    title: '操作',
    key: 'actions',
    width: 120,
    render: (row) =>
      h(
        NPopconfirm,
        { onPositiveClick: () => doDrop(row.name) },
        {
          trigger: () =>
            h(
              NButton,
              { size: 'small', type: 'error', tertiary: true, disabled: !isAdmin },
              { icon: () => h(NIcon, { component: TrashOutline }), default: () => '删除' },
            ),
          default: () => `确认删除租户 "${row.name}"？其下所有数据将丢失！`,
        },
      ),
  },
]

async function load() {
  loading.value = true
  try {
    const resp = await api.listTenants()
    rows.value = resp.tenants || []
  } catch (err) {
    if (err.status === 403) {
      rows.value = []
      message.warning('需要 admin 角色才能管理租户')
    } else {
      message.error('加载失败: ' + err.message)
    }
  } finally {
    loading.value = false
  }
}

async function doDrop(name) {
  try {
    await api.dropTenant(name)
    message.success('已删除 ' + name)
    load()
  } catch (err) {
    message.error('删除失败: ' + err.message)
  }
}

const modalOpen = ref(false)
const creating = ref(false)
const name = ref('')

async function doCreate() {
  if (!name.value.trim()) return message.warning('租户名称不能为空')
  creating.value = true
  try {
    await api.createTenant(name.value.trim())
    message.success('租户已创建')
    modalOpen.value = false
    name.value = ''
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
          <div class="page-title">租户管理</div>
          <n-text depth="3" style="font-size: 13px"
            >多租户隔离：每个租户拥有独立的 Collections 与 API Key（仅 admin）</n-text
          >
        </div>
        <n-button v-if="isAdmin" type="primary" @click="modalOpen = true">
          <template #icon><n-icon :component="AddOutline" /></template>
          新建租户
        </n-button>
      </div>
    </n-card>

    <n-card :bordered="true">
      <n-data-table
        :columns="columns"
        :data="rows"
        :loading="loading"
        :row-key="(r) => r.name"
        :bordered="false"
        :pagination="{ pageSize: 12 }"
      />
      <n-empty
        v-if="!loading && rows.length === 0"
        description="暂无租户"
        style="padding: 32px 0"
      />
    </n-card>

    <n-modal
      v-model:show="modalOpen"
      preset="card"
      title="新建租户"
      style="width: 460px; max-width: 92vw"
    >
      <n-form label-placement="top">
        <n-form-item label="租户名称">
          <n-input
            v-model:value="name"
            placeholder="my-team（[A-Za-z0-9_-]）"
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
.modal-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
</style>
