<script setup>
import { ref, computed, onMounted, onBeforeUnmount, nextTick, watch } from 'vue'
import {
  NCard,
  NSelect,
  NInput,
  NButton,
  NIcon,
  NSpin,
  NEmpty,
  NTag,
  NText,
  NAlert,
  NCollapse,
  NCollapseItem,
  NInputNumber,
  NSwitch,
  NModal,
  NDataTable,
  NDivider,
  useMessage,
} from 'naive-ui'
import { SearchOutline, CutOutline, FlashOutline } from '@vicons/ionicons5'
import * as api from '../api/client'
import { session } from '../store/session'
import SearchHitCard from '../components/SearchHitCard.vue'
import { extractTerms } from '../utils/highlight'

const message = useMessage()

const isAdmin = session.role === 'admin'
const tenants = ref([])
const tenant = ref('')
const collections = ref([])
const collection = ref('')
const query = ref('')
const searchInput = ref(null)

const topK = ref(10)
const rerank = ref(false)
const bm25Weight = ref(1.0)
const vecWeight = ref(1.0)

const loading = ref(false)
const hits = ref([])
const tookMs = ref(0)
const degraded = ref([])
const terms = ref([])
const searched = ref(false)
const totalHits = ref(0)

// 结果集内最高分，传给结果卡片做 score 条归一化
const maxScore = computed(() => hits.value.reduce((m, h) => Math.max(m, Number(h.score || 0)), 0))

// 分词预览
const analyzeOpen = ref(false)
const analyzeLoading = ref(false)
const analyzeTokens = ref([])
const analyzeColumns = [
  { title: 'term', key: 'term' },
  { title: 'pos', key: 'pos', width: 70 },
  { title: 'begin', key: 'begin', width: 70 },
  { title: 'end', key: 'end', width: 70 },
]

async function loadTenants() {
  if (isAdmin) {
    try {
      const resp = await api.listTenants()
      tenants.value = (resp.tenants || []).map((t) => ({
        label: t.name,
        value: t.name,
      }))
      if (!tenant.value && tenants.value.length > 0) {
        tenant.value = tenants.value[0].value
      }
    } catch (err) {
      message.error('加载租户失败: ' + err.message)
    }
  } else {
    tenant.value = session.tenant
    tenants.value = [{ label: session.tenant, value: session.tenant }]
  }
  if (tenant.value) await loadCollections()
}

async function loadCollections() {
  if (!tenant.value) return
  try {
    const resp = await api.listCollections(tenant.value)
    collections.value = (resp.collections || []).map((c) => ({
      label: c.name,
      value: c.name,
    }))
    if (!collection.value && collections.value.length > 0) {
      collection.value = collections.value[0].value
    }
    for (const c of resp.collections || []) {
      api
        .listTopLevelDocuments(c.name, tenant.value)
        .then((docs) => {
          const opt = collections.value.find((o) => o.value === c.name)
          if (opt) opt.label = `${c.name} (${docs.length} 篇)`
        })
        .catch(() => {})
    }
  } catch (err) {
    message.error('加载 Collections 失败: ' + err.message)
  }
}

watch(tenant, () => {
  collection.value = ''
  collections.value = []
  hits.value = []
  searched.value = false
  if (tenant.value) loadCollections()
})

async function doSearch() {
  if (!collection.value) {
    message.warning('请先选择 Collection')
    return
  }
  const text = query.value.trim()
  if (!text) {
    message.warning('请输入查询文本')
    return
  }
  loading.value = true
  searched.value = true
  try {
    // 并行获取分词结果，用于命中高亮
    const analyzePromise = api
      .analyze(collection.value, text, tenant.value)
      .then((r) => {
        terms.value = extractTerms(text, r.tokens || [])
      })
      .catch(() => {
        terms.value = extractTerms(text)
      })

    const params = {
      text,
      top_k: topK.value,
      rerank: rerank.value,
    }
    if (bm25Weight.value !== 1.0 || vecWeight.value !== 1.0) {
      params.weights = { bm25: bm25Weight.value, vector: vecWeight.value }
    }
    const resp = await api.search(collection.value, params, tenant.value)
    hits.value = resp.hits || []
    totalHits.value = hits.value.length
    tookMs.value = resp.took_ms || 0
    degraded.value = resp.degraded || []
    await analyzePromise
  } catch (err) {
    hits.value = []
    message.error('搜索失败: ' + err.message)
  } finally {
    loading.value = false
  }
}

async function openAnalyze() {
  const text = query.value.trim()
  if (!collection.value) {
    message.warning('请先选择 Collection')
    return
  }
  if (!text) {
    message.warning('请输入要分词的文本')
    return
  }
  analyzeOpen.value = true
  analyzeLoading.value = true
  analyzeTokens.value = []
  try {
    const resp = await api.analyze(collection.value, text, tenant.value)
    analyzeTokens.value = resp.tokens || []
  } catch (err) {
    message.error('分词失败: ' + err.message)
    analyzeOpen.value = false
  } finally {
    analyzeLoading.value = false
  }
}

// ---- 快捷键：/ 聚焦搜索框，Esc 清空 ----
function onKeydown(e) {
  const tag = e.target?.tagName
  const inInput = tag === 'INPUT' || tag === 'TEXTAREA' || e.target?.isContentEditable
  if (e.key === '/' && !inInput) {
    e.preventDefault()
    searchInput.value?.focus()
  } else if (e.key === 'Escape' && inInput) {
    query.value = ''
    searchInput.value?.blur()
  }
}

onMounted(() => {
  loadTenants()
  window.addEventListener('keydown', onKeydown)
})
onBeforeUnmount(() => window.removeEventListener('keydown', onKeydown))

const degradedLabel = (d) =>
  d === 'vector' ? '向量检索已降级' : d === 'rerank' ? 'Rerank 已降级' : `降级: ${d}`
</script>

<template>
  <div class="mss-stack">
    <n-card :bordered="true" class="search-panel">
      <div class="search-row">
        <n-select
          v-if="isAdmin"
          v-model:value="tenant"
          :options="tenants"
          placeholder="租户"
          style="width: 160px"
          filterable
        />
        <n-select
          v-model:value="collection"
          :options="collections"
          placeholder="选择 Collection"
          style="width: 240px"
          filterable
          clearable
        />
        <n-input
          ref="searchInput"
          v-model:value="query"
          size="large"
          round
          class="search-input"
          placeholder="输入查询文本，回车搜索；/ 聚焦搜索框，Esc 清空"
          clearable
          @keydown.enter="doSearch"
        >
          <template #prefix><n-icon :component="SearchOutline" /></template>
        </n-input>
        <n-button
          type="primary"
          size="large"
          round
          class="search-btn"
          :loading="loading"
          @click="doSearch"
        >
          搜索
        </n-button>
        <n-button size="large" round @click="openAnalyze">
          <template #icon><n-icon :component="CutOutline" /></template>
          分词
        </n-button>
      </div>

      <n-collapse class="opts-collapse">
        <n-collapse-item title="检索参数" name="opts">
          <div class="opts-row">
            <div class="opt-item">
              <n-text depth="3">Top K</n-text>
              <n-input-number v-model:value="topK" :min="1" :max="100" style="width: 90px" />
            </div>
            <div class="opt-item">
              <n-text depth="3">Rerank</n-text>
              <n-switch v-model:value="rerank" />
              <n-text depth="3" style="font-size: 12px">cross-encoder 重排</n-text>
            </div>
            <div class="opt-item">
              <n-text depth="3">BM25 权重</n-text>
              <n-input-number
                v-model:value="bm25Weight"
                :min="0"
                :max="10"
                :step="0.1"
                style="width: 100px"
              />
            </div>
            <div class="opt-item">
              <n-text depth="3">向量权重</n-text>
              <n-input-number
                v-model:value="vecWeight"
                :min="0"
                :max="10"
                :step="0.1"
                style="width: 100px"
              />
            </div>
          </div>
        </n-collapse-item>
      </n-collapse>
    </n-card>

    <div v-if="searched && !loading" class="result-meta">
      <div class="meta-tags">
        <n-tag :bordered="false" type="info" size="small">
          <template #icon><n-icon :component="FlashOutline" /></template>
          {{ tookMs }} ms · {{ totalHits }} 条结果
        </n-tag>
        <n-tag v-for="d in degraded" :key="d" :bordered="false" type="warning" size="small">
          {{ degradedLabel(d) }}
        </n-tag>
      </div>
    </div>

    <n-spin :show="loading">
      <div v-if="searched && !loading && hits.length === 0" class="empty-wrap">
        <n-empty description="无匹配结果">
          <template #extra>
            <n-button size="small" @click="doSearch">换个关键词试试</n-button>
          </template>
        </n-empty>
      </div>
      <div v-else-if="!searched" class="empty-wrap">
        <n-empty description="输入查询文本开始搜索" />
      </div>
      <div class="mss-stack result-list">
        <SearchHitCard
          v-for="(hit, i) in hits"
          :key="hit.id"
          :hit="hit"
          :terms="terms"
          :rank="i + 1"
          :max-score="maxScore"
        />
      </div>
    </n-spin>

    <n-modal
      v-model:show="analyzeOpen"
      preset="card"
      title="分词预览（Analyze）"
      style="width: 640px; max-width: 92vw"
    >
      <n-text depth="3" style="font-size: 13px">
        使用当前 Collection 的查询分析器对查询文本分词，命中高亮即基于这些 token。
      </n-text>
      <n-divider style="margin: 12px 0" />
      <n-spin :show="analyzeLoading">
        <n-data-table
          :columns="analyzeColumns"
          :data="analyzeTokens"
          :max-height="360"
          :scroll-x="560"
          size="small"
        />
      </n-spin>
    </n-modal>
  </div>
</template>

<style scoped>
.search-panel {
  background: var(--mss-card);
  border-radius: var(--mss-radius-l);
}
.search-row {
  display: flex;
  gap: 12px;
  align-items: center;
}
.search-row .search-input {
  flex: 1;
  transition: box-shadow 0.2s;
}
.search-row .search-input:focus-within {
  box-shadow: 0 0 0 4px var(--mss-brand-soft);
}
.search-btn {
  padding: 0 28px;
  transition: transform 0.15s;
}
.search-btn:hover {
  transform: translateY(-1px);
}
.opts-collapse {
  margin-top: 12px;
}
.opts-row {
  display: flex;
  flex-wrap: wrap;
  gap: 24px;
  align-items: center;
}
.opt-item {
  display: flex;
  align-items: center;
  gap: 8px;
}
.result-meta {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.meta-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.empty-wrap {
  padding: 48px 0;
}
.result-list {
  gap: 12px;
}
</style>
