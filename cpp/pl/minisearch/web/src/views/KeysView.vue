<script setup>
import { ref, onMounted, h, computed } from 'vue'
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
  NText,
  NIcon,
  NAlert,
  useMessage,
  NEmpty,
} from 'naive-ui'
import { AddOutline, KeyOutline } from '@vicons/ionicons5'
import * as api from '../api/client'
import { session } from '../store/session'

const message = useMessage()

const tenants = ref([])
const tenant = ref('')
const loading = ref(false)
const rows = ref([])

const canManage = ['admin', 'tenant_admin'].includes(session.role)

async function loadTenants() {
  try {
    const resp = await api.listTenants()
    tenants.value = (resp.tenants || []).map((t) => ({ label: t.name, value: t.name }))
    if (!tenant.value && tenants.value.length > 0) {
      tenant.value = tenants.value[0].value
      loadKeys()
    } else if (tenant.value) {
      loadKeys()
    }
  } catch (err) {
    if (err.status === 403) message.warning('需要 admin / tenant_admin 角色才能管理密钥')
    else message.error('加载租户失败: ' + err.message)
  }
}

async function loadKeys() {
  if (!tenant.value) return
  loading.value = true
  try {
    const resp = await api.listKeys(tenant.value)
    rows.value = resp.keys || []
  } catch (err) {
    message.error('加载密钥失败: ' + err.message)
  } finally {
    loading.value = false
  }
}

function onTenantChange() {
  loadKeys()
}

const roleTagType = (role) =>
  role === 'tenant_admin' ? 'warning' : role === 'writer' ? 'info' : 'default'

const columns = [
  {
    title: 'Key ID',
    key: 'key_id',
    width: 180,
    render: (row) => h('span', { style: 'font-family:monospace;font-size:12px' }, row.key_id),
  },
  {
    title: '角色',
    key: 'role',
    width: 130,
    render: (row) =>
      h(
        NTag,
        { size: 'small', bordered: false, type: roleTagType(row.role) },
        { default: () => row.role },
      ),
  },
  {
    title: 'Collection 白名单',
    key: 'collections',
    render: (row) =>
      (row.collections || []).length
        ? h(
            'div',
            { style: 'display:flex;flex-wrap:wrap;gap:4px' },
            row.collections.map((c) =>
              h(NTag, { size: 'tiny', bordered: false, type: 'default' }, { default: () => c }),
            ),
          )
        : h(NText, { depth: 3 }, { default: () => '(全部)' }),
  },
  {
    title: '创建时间',
    key: 'created_at',
    width: 170,
    render: (row) =>
      h(
        NText,
        { depth: 3, style: 'font-size:13px' },
        {
          default: () => (row.created_at ? new Date(row.created_at * 1000).toLocaleString() : '-'),
        },
      ),
  },
  {
    title: '状态',
    key: 'status',
    width: 90,
    render: (row) =>
      row.revoked
        ? h(NTag, { size: 'small', bordered: false, type: 'error' }, { default: () => '已吊销' })
        : h(NTag, { size: 'small', bordered: false, type: 'success' }, { default: () => '有效' }),
  },
  {
    title: '操作',
    key: 'actions',
    width: 100,
    render: (row) =>
      row.revoked
        ? h(NText, { depth: 3 }, { default: () => '-' })
        : h(
            NPopconfirm,
            { onPositiveClick: () => doRevoke(row) },
            {
              trigger: () =>
                h(
                  NButton,
                  { size: 'small', type: 'error', tertiary: true },
                  { default: () => '吊销' },
                ),
              default: () => `确认吊销 Key "${row.key_id}"？`,
            },
          ),
  },
]

async function doRevoke(row) {
  try {
    await api.revokeKey(tenant.value, row.key_id)
    message.success('已吊销')
    loadKeys()
  } catch (err) {
    message.error('吊销失败: ' + err.message)
  }
}

// ---- 签发 ----
const issueOpen = ref(false)
const issuing = ref(false)
const issueForm = ref({ role: 'reader', collections: '' })
const issuedKey = ref('')
const issuedKeyOpen = ref(false)

async function doIssue() {
  const collections = issueForm.value.collections
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
  issuing.value = true
  try {
    const resp = await api.issueKey(tenant.value, issueForm.value.role, collections)
    issuedKey.value = resp.key || ''
    issueOpen.value = false
    issueForm.value.collections = ''
    if (issuedKey.value) {
      issuedKeyOpen.value = true
    }
    loadKeys()
  } catch (err) {
    message.error('签发失败: ' + err.message)
  } finally {
    issuing.value = false
  }
}

function copyIssued() {
  navigator.clipboard
    ?.writeText(issuedKey.value)
    .then(() => message.success('已复制'))
    .catch(() => message.error('复制失败'))
}

onMounted(loadTenants)

const noKeys = computed(() => !loading.value && rows.value.length === 0)
</script>

<template>
  <div class="mss-stack">
    <n-card :bordered="true">
      <div class="page-head">
        <n-select
          v-model:value="tenant"
          :options="tenants"
          placeholder="选择租户"
          style="width: 240px"
          filterable
          clearable
          @update:value="onTenantChange"
        />
        <n-button v-if="canManage" type="primary" :disabled="!tenant" @click="issueOpen = true">
          <template #icon><n-icon :component="AddOutline" /></template>
          签发 API Key
        </n-button>
      </div>
    </n-card>

    <n-card :bordered="true">
      <n-data-table
        :columns="columns"
        :data="rows"
        :loading="loading"
        :row-key="(r) => r.key_id"
        :bordered="false"
        :pagination="{ pageSize: 12 }"
      />
      <n-empty v-if="noKeys" description="该租户暂无 API Key" style="padding: 32px 0" />
    </n-card>

    <!-- 签发 Modal -->
    <n-modal
      v-model:show="issueOpen"
      preset="card"
      title="签发 API Key"
      style="width: 520px; max-width: 92vw"
    >
      <n-alert type="info" :bordered="false" style="margin-bottom: 16px">
        明文 Key 仅在签发时返回一次，请立即复制保存。
      </n-alert>
      <n-form label-placement="top">
        <n-form-item label="角色">
          <n-select
            v-model:value="issueForm.role"
            :options="[
              { label: 'tenant_admin — 租户管理', value: 'tenant_admin' },
              { label: 'writer — 读写', value: 'writer' },
              { label: 'reader — 只读', value: 'reader' },
            ]"
            :disabled="issuing"
          />
        </n-form-item>
        <n-form-item label="Collection 白名单（逗号分隔，空 = 本租户全部）">
          <n-input
            v-model:value="issueForm.collections"
            placeholder="loom-kb,code-search"
            :disabled="issuing"
          />
        </n-form-item>
        <div class="modal-actions">
          <n-button @click="issueOpen = false" :disabled="issuing">取消</n-button>
          <n-button type="primary" :loading="issuing" @click="doIssue">签发</n-button>
        </div>
      </n-form>
    </n-modal>

    <!-- 明文展示 Modal -->
    <n-modal
      v-model:show="issuedKeyOpen"
      preset="card"
      title="API Key 已签发"
      style="width: 620px; max-width: 94vw"
    >
      <n-text depth="3" style="font-size: 13px">明文仅此一次返回，请妥善保存：</n-text>
      <n-input :value="issuedKey" type="textarea" readonly :rows="3" class="key-output" />
      <div class="modal-actions">
        <n-button type="primary" @click="copyIssued">
          <template #icon><n-icon :component="KeyOutline" /></template>
          复制
        </n-button>
        <n-button @click="issuedKeyOpen = false">关闭</n-button>
      </div>
    </n-modal>
  </div>
</template>

<style scoped>
.page-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.modal-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.key-output {
  margin: 10px 0 14px;
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
  font-size: 13px;
}
</style>
