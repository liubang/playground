<script setup>
import { ref, onMounted } from 'vue'
import { NCard, NStatistic, NTag, NText, NIcon, NSpin, useMessage, NEmpty } from 'naive-ui'
import { FolderOpenOutline, DocumentTextOutline, BusinessOutline } from '@vicons/ionicons5'
import * as api from '../api/client'

const message = useMessage()
const loading = ref(false)
const stats = ref(null)

async function load() {
  loading.value = true
  try {
    const resp = await api.getStats()
    stats.value = resp
  } catch (err) {
    if (err.status === 403) message.warning('需要 admin 角色才能查看统计')
    else message.error('加载统计失败: ' + err.message)
  } finally {
    loading.value = false
  }
}

onMounted(load)
</script>

<template>
  <n-spin :show="loading">
    <div class="mss-stack">
      <div class="stat-row">
        <n-card :bordered="true" class="stat-card">
          <n-statistic label="总 Collections">
            <template #prefix><n-icon :component="FolderOpenOutline" /></template>
            {{ stats?.total_collections ?? 0 }}
          </n-statistic>
        </n-card>
        <n-card :bordered="true" class="stat-card">
          <n-statistic label="总活跃文档">
            <template #prefix><n-icon :component="DocumentTextOutline" /></template>
            {{ stats?.total_active_documents ?? 0 }}
          </n-statistic>
        </n-card>
      </div>

      <n-card :bordered="true">
        <template #header>
          <div class="header-inline">
            <n-icon :component="BusinessOutline" color="var(--mss-brand)" />
            <span>分租户统计</span>
          </div>
        </template>
        <div v-if="stats?.tenants?.length" class="tenant-row">
          <n-card v-for="t in stats.tenants" :key="t.name" size="small" class="tenant-stat">
            <div class="tenant-head">
              <n-tag :bordered="false" type="primary" size="small">{{ t.name }}</n-tag>
            </div>
            <div class="tenant-numbers">
              <n-statistic label="Collections" :value="t.collections ?? 0" />
              <n-statistic label="活跃文档" :value="t.active_documents ?? 0" />
            </div>
          </n-card>
        </div>
        <n-empty v-else description="暂无租户数据" style="padding: 24px 0" />
      </n-card>
    </div>
  </n-spin>
</template>

<style scoped>
.stat-row {
  display: flex;
  flex-wrap: wrap;
  gap: 16px;
}
.stat-card {
  min-width: 260px;
  border-radius: 12px;
}
.header-inline {
  display: flex;
  align-items: center;
  gap: 8px;
}
.tenant-row {
  display: flex;
  flex-wrap: wrap;
  gap: 16px;
}
.tenant-stat {
  min-width: 280px;
  border-radius: 12px;
}
.tenant-head {
  display: flex;
  align-items: center;
  margin-bottom: 8px;
}
.tenant-numbers {
  display: flex;
  align-items: baseline;
  gap: 24px;
}
</style>
