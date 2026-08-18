<script setup>
import { ref, reactive, onMounted } from 'vue'
import {
  NCard,
  NButton,
  NSelect,
  NTag,
  NPopconfirm,
  NModal,
  NForm,
  NFormItem,
  NInput,
  NText,
  NIcon,
  NAlert,
  NEmpty,
  NSpin,
  useMessage,
} from 'naive-ui'
import {
  ChevronForwardOutline,
  KeyOutline,
  BusinessOutline,
  AddOutline,
  TrashOutline,
  SwapHorizontalOutline,
  RefreshOutline,
} from '@vicons/ionicons5'
import * as api from '../api/client'
import { session } from '../store/session'

const message = useMessage()
const isAdmin = session.role === 'admin'
const canManage = ['admin', 'tenant_admin'].includes(session.role)

// ---- hierarchical expandable list ----
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
        keys: null,
        active: 0,
        total: 0,
      })
      tenants.push(t)
      if (t.expanded) await loadKeys(t)
    }
  } catch (err) {
    if (err.status === 403) message.warning('需要 admin / tenant_admin 角色才能管理密钥')
    else message.error('加载租户失败: ' + err.message)
  }
}

async function loadKeys(t) {
  t.loading = true
  try {
    const resp = await api.listKeys(t.name)
    const keys = resp.keys || []
    t.keys = keys.map((k) => ({ ...k }))
    t.active = keys.filter((k) => !k.revoked).length
    t.total = keys.length
  } catch (err) {
    message.error(`加载 ${t.name} 密钥失败: ` + err.message)
  } finally {
    t.loading = false
  }
}

async function reloadKeys(t) {
  if (t.expanded) await loadKeys(t)
}

function toggleTenant(t) {
  t.expanded = !t.expanded
  if (t.expanded && !t.keys) loadKeys(t)
}

function findTenant(name) {
  return tenants.find((t) => t.name === name)
}

function roleType(role) {
  if (role === 'tenant_admin') return 'warning'
  if (role === 'writer') return 'info'
  return 'default'
}

async function doRevoke(tenant, keyId) {
  try {
    await api.revokeKey(tenant, keyId)
    message.success('已吊销')
    await reloadKeys(findTenant(tenant))
  } catch (err) {
    message.error('吊销失败: ' + err.message)
  }
}

// ---- 签发 ----
const issueOpen = ref(false)
const issuing = ref(false)
const issueTenant = ref('')
const issueForm = ref({ role: 'reader', collections: '' })
const issuedKey = ref('')
const issuedKeyOpen = ref(false)

function openIssue(tenant) {
  issueTenant.value = tenant
  issueForm.value = { role: 'reader', collections: '' }
  issueOpen.value = true
}

async function doIssue() {
  const collections = issueForm.value.collections
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
  issuing.value = true
  try {
    const resp = await api.issueKey(issueTenant.value, issueForm.value.role, collections)
    issuedKey.value = resp.key || ''
    issueOpen.value = false
    if (issuedKey.value) issuedKeyOpen.value = true
    const t = findTenant(issueTenant.value)
    if (t) {
      t.expanded = true
      await reloadKeys(t)
    }
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

// ---- 迁移 ----
const moveOpen = ref(false)
const moveTenant = ref('')
const moveKeyId = ref('')
const moveTarget = ref('')
const moveOptions = ref([])

async function openMove(tenant, keyId) {
  moveTenant.value = tenant
  moveKeyId.value = keyId
  moveTarget.value = ''
  moveOpen.value = true
  try {
    const resp = await api.listTenants()
    moveOptions.value = (resp.tenants || [])
      .map((t) => t.name)
      .filter((t) => t !== tenant)
      .map((t) => ({ label: t, value: t }))
  } catch (err) {
    message.error('加载租户列表失败: ' + err.message)
  }
}

async function doMove() {
  if (!moveTarget.value) return message.warning('请选择目标租户')
  try {
    await api.moveKey(moveTenant.value, moveKeyId.value, moveTarget.value)
    message.success(`Key 已迁移到 ${moveTarget.value}`)
    moveOpen.value = false
    const src = findTenant(moveTenant.value)
    if (src) await reloadKeys(src)
    const dst = findTenant(moveTarget.value)
    if (dst && dst.expanded) await reloadKeys(dst)
  } catch (err) {
    message.error('迁移失败: ' + err.message)
  }
}

onMounted(loadTenants)
</script>

<template>
  <div class="mss-stack">
    <n-card :bordered="true">
      <div class="page-head">
        <div>
          <div class="page-title">密钥管理</div>
          <n-text depth="3" style="font-size: 13px"> 按租户分组展开，每个 Key 一行可操作。 </n-text>
        </div>
        <n-button quaternary @click="loadTenants">
          <template #icon><n-icon :component="RefreshOutline" /></template>
          刷新
        </n-button>
      </div>
    </n-card>

    <n-card :bordered="true" class="data-card">
      <n-empty v-if="tenants.length === 0" description="暂无租户数据" style="padding: 32px 0" />

      <div v-for="t in tenants" :key="t.name" class="tree-node">
        <!-- 租户行 -->
        <div class="tree-row level-0" @click="toggleTenant(t)">
          <n-icon
            class="chevron"
            :class="{ open: t.expanded }"
            :component="ChevronForwardOutline"
          />
          <n-icon :component="BusinessOutline" color="var(--mss-brand)" class="row-icon" />
          <span class="row-label">{{ t.name }}</span>
          <n-tag v-if="t.keys" size="tiny" :bordered="false" type="info" class="row-meta">
            {{ t.active }} 有效 / {{ t.total }} 总
          </n-tag>
          <div class="row-actions" @click.stop>
            <n-button
              v-if="canManage"
              size="tiny"
              quaternary
              type="primary"
              @click="openIssue(t.name)"
            >
              <template #icon><n-icon :component="AddOutline" /></template>
              签发
            </n-button>
          </div>
        </div>

        <!-- 密钥行 -->
        <div v-show="t.expanded" class="tree-children">
          <n-spin :show="t.loading" size="small">
            <template v-if="t.keys && t.keys.length > 0">
              <div v-for="k in t.keys" :key="k.key_id" class="tree-row level-1">
                <span class="chevron-placeholder" />
                <n-icon
                  :component="KeyOutline"
                  :color="k.revoked ? '#999' : '#2d8f5f'"
                  class="row-icon"
                />
                <span class="row-label mono">{{ k.key_id }}</span>
                <n-tag size="tiny" :bordered="false" :type="roleType(k.role)" class="row-meta">
                  {{ k.role }}
                </n-tag>
                <n-tag
                  size="tiny"
                  :bordered="false"
                  :type="k.revoked ? 'error' : 'success'"
                  class="row-meta"
                >
                  {{ k.revoked ? '已吊销' : '有效' }}
                </n-tag>
                <div class="row-actions">
                  <n-button
                    v-if="isAdmin"
                    size="tiny"
                    quaternary
                    type="info"
                    @click="openMove(t.name, k.key_id)"
                  >
                    <template #icon><n-icon :component="SwapHorizontalOutline" /></template>
                    迁移
                  </n-button>
                  <n-popconfirm
                    v-if="canManage && !k.revoked"
                    @positive-click="doRevoke(t.name, k.key_id)"
                  >
                    <template #trigger>
                      <n-button size="tiny" quaternary type="error">
                        <template #icon><n-icon :component="TrashOutline" /></template>
                        吊销
                      </n-button>
                    </template>
                    确认吊销 Key "{{ k.key_id }}"？
                  </n-popconfirm>
                </div>
              </div>
            </template>
            <n-empty
              v-else-if="t.keys && t.keys.length === 0"
              description="暂无密钥"
              size="small"
              style="padding: 12px 0"
            />
          </n-spin>
        </div>
      </div>
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
        <n-text depth="3" style="font-size: 12px">将签发到租户：{{ issueTenant }}</n-text>
        <n-form-item label="角色" style="margin-top: 12px">
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

    <!-- 迁移 Modal -->
    <n-modal
      v-model:show="moveOpen"
      preset="card"
      title="迁移 API Key"
      style="width: 460px; max-width: 92vw"
    >
      <n-form label-placement="top">
        <n-text depth="3" style="font-size: 12px">
          将 Key「{{ moveKeyId }}」从租户 {{ moveTenant }} 迁移到目标租户，立即生效。
        </n-text>
        <n-form-item label="目标租户" style="margin-top: 12px">
          <n-select v-model:value="moveTarget" :options="moveOptions" placeholder="选择目标租户" />
        </n-form-item>
        <div class="modal-actions">
          <n-button @click="moveOpen = false">取消</n-button>
          <n-button type="primary" @click="doMove">迁移</n-button>
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
.key-output {
  margin: 10px 0 14px;
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
  font-size: 13px;
}
.mono {
  font-family: 'SF Mono', 'Fira Code', Consolas, monospace;
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

.level-1 .row-label {
  font-weight: 500;
  font-size: 13px;
}
</style>
