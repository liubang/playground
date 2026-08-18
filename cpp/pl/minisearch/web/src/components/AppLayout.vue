<script setup>
import { h, computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import {
  NLayout,
  NLayoutSider,
  NLayoutHeader,
  NLayoutContent,
  NMenu,
  NTag,
  NButton,
  NPopconfirm,
  NDropdown,
  NAvatar,
  NIcon,
  useMessage,
} from 'naive-ui'
import {
  SearchOutline,
  FolderOpenOutline,
  DocumentTextOutline,
  BusinessOutline,
  KeyOutline,
  BarChartOutline,
  MoonOutline,
  SunnyOutline,
  LogOutOutline,
  SparklesOutline,
} from '@vicons/ionicons5'
import { session, toggleDark, clearSession, darkThemeState } from '../store/session'
import * as api from '../api/client'

const route = useRoute()
const router = useRouter()
const message = useMessage()
const collapsed = ref(false)

const activeKey = computed(() =>
  route.name && route.name !== 'login' ? String(route.name) : 'search',
)

const icon = (c) => () => h(c)

const menuOptions = computed(() => {
  const items = [
    { label: '搜索', key: 'search', icon: icon(SearchOutline) },
    { label: 'Collections', key: 'collections', icon: icon(FolderOpenOutline) },
    { label: '文档', key: 'documents', icon: icon(DocumentTextOutline) },
  ]
  if (session.role === 'admin' || session.role === 'tenant_admin') {
    items.push({ label: '密钥管理', key: 'keys', icon: icon(KeyOutline) })
  }
  if (session.role === 'admin') {
    items.push({ label: '租户管理', key: 'tenants', icon: icon(BusinessOutline) })
  }
  items.push({ label: '统计', key: 'stats', icon: icon(BarChartOutline) })
  return items
})

function onMenuSelect(key) {
  if (key !== route.name) router.push({ name: key })
}

async function doLogout() {
  try {
    await api.logout()
  } catch {
    /* ignore */
  }
  clearSession()
  message.success('已退出登录')
  router.push({ name: 'login' })
}

const userDropdownOptions = computed(() => [
  {
    label: () =>
      h('div', { style: 'padding:4px 2px' }, [
        h('div', { style: 'font-weight:600;font-size:13px' }, session.user || 'console'),
        h(
          'div',
          { style: 'font-size:12px;color:var(--mss-text-muted);margin-top:2px' },
          `${session.tenant || 'default'} · ${session.role || 'admin'}`,
        ),
      ]),
    key: 'whoami',
    disabled: true,
  },
  { type: 'divider', key: 'd1' },
  { label: '退出登录', key: 'logout' },
])

function onUserAction(key) {
  if (key === 'logout') doLogout()
}

const displayName = computed(() => session.user || session.tenant || 'console')
</script>

<template>
  <n-layout has-sider style="height: 100vh">
    <n-layout-sider
      bordered
      collapse-mode="width"
      :width="230"
      :collapsed-width="68"
      show-trigger="bar"
      :collapsed="collapsed"
      class="app-sider"
      @collapse="collapsed = true"
      @expand="collapsed = false"
    >
      <div class="brand">
        <div class="brand-mark">
          <n-icon :component="SparklesOutline" />
        </div>
        <div v-if="!collapsed" class="brand-text">
          <span class="brand-name">MiniSearch</span>
          <span class="brand-sub">Hybrid Search Console</span>
        </div>
      </div>
      <div class="sider-nav">
        <n-menu
          :value="activeKey"
          :options="menuOptions"
          :collapsed="collapsed"
          :collapsed-width="68"
          :indent="22"
          @update:value="onMenuSelect"
        />
      </div>
      <template #footer>
        <div class="sider-footer">
          <n-dropdown :options="userDropdownOptions" @select="onUserAction">
            <div class="user-chip">
              <n-avatar round size="small" class="user-avatar">{{
                displayName.slice(0, 1).toUpperCase()
              }}</n-avatar>
              <div v-if="!collapsed" class="user-meta">
                <span class="user-name">{{ displayName }}</span>
                <n-tag size="tiny" :bordered="false" type="info" class="user-role">{{
                  session.role
                }}</n-tag>
              </div>
            </div>
          </n-dropdown>
        </div>
      </template>
    </n-layout-sider>

    <n-layout>
      <n-layout-header bordered class="topbar">
        <div class="topbar-left">
          <span class="topbar-title">{{ route.meta?.title || '' }}</span>
        </div>
        <div class="topbar-right">
          <n-tag v-if="session.tenant" size="small" :bordered="false" type="primary" round
            >租户: {{ session.tenant }}</n-tag
          >
          <n-button quaternary circle class="icon-btn" @click="toggleDark">
            <template #icon>
              <n-icon :component="darkThemeState.dark ? SunnyOutline : MoonOutline" />
            </template>
          </n-button>
          <n-popconfirm @positive-click="doLogout">
            <template #trigger>
              <n-button quaternary circle class="icon-btn">
                <template #icon><n-icon :component="LogOutOutline" /></template>
              </n-button>
            </template>
            确认退出登录？
          </n-popconfirm>
        </div>
      </n-layout-header>

      <n-layout-content class="page-content" content-style="padding: 24px 28px 40px;">
        <router-view />
      </n-layout-content>
    </n-layout>
  </n-layout>
</template>

<style scoped>
/* 侧边栏跟随主题：亮色浅色 / 暗色深青，菜单配色由 naive 主题自然适配 */
.app-sider {
  background: var(--mss-sider);
}
.brand {
  display: flex;
  align-items: center;
  gap: 12px;
  height: 64px;
  padding: 0 16px;
  border-bottom: 1px solid var(--mss-border);
}
.brand-mark {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  height: 34px;
  border-radius: var(--mss-radius-m);
  background: var(--mss-brand-grad); /* 渐变仅保留在 logo mark */
  color: #fff;
  font-size: 18px;
  box-shadow: 0 4px 12px rgba(99, 102, 241, 0.4);
  flex-shrink: 0;
}
.brand-text {
  display: flex;
  flex-direction: column;
  min-width: 0;
}
.brand-name {
  font-size: 16px;
  font-weight: 700;
  color: var(--mss-text-strong);
  letter-spacing: 0.3px;
}
.brand-sub {
  font-size: 10px;
  color: var(--mss-text-muted);
  letter-spacing: 0.4px;
}
.sider-nav {
  flex: 1;
  padding: 12px 10px;
  overflow-y: auto;
}
:deep(.n-menu) {
  --n-item-height: 40px;
}
.sider-footer {
  padding: 12px;
  border-top: 1px solid var(--mss-border);
}
.user-chip {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 10px;
  border-radius: var(--mss-radius-m);
  cursor: pointer;
  transition: background 0.15s;
}
.user-chip:hover {
  background: var(--mss-brand-soft);
}
.user-avatar {
  background: var(--mss-brand);
  color: #fff;
  font-weight: 600;
}
.user-meta {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}
.user-name {
  font-size: 13px;
  color: var(--mss-text-strong);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  max-width: 92px;
}
.user-role {
  flex-shrink: 0;
}
.topbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  height: 64px;
  padding: 0 24px;
  background: rgba(255, 255, 255, 0.7);
  backdrop-filter: saturate(180%) blur(12px);
}
html.dark .topbar {
  background: rgba(11, 18, 32, 0.7);
}
.topbar-title {
  font-size: 16px;
  font-weight: 700;
  letter-spacing: 0.2px;
}
.topbar-right {
  display: flex;
  align-items: center;
  gap: 8px;
}
.icon-btn {
  color: inherit;
}
.page-content {
  height: calc(100vh - 64px);
  overflow-y: auto;
}
</style>
