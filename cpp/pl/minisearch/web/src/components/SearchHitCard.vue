<script setup>
import { ref, computed, watch, onMounted, nextTick } from 'vue'
import { NCard, NTag, NIcon, NButton, NText, NTooltip } from 'naive-ui'
import { ChevronDownOutline, ChevronUpOutline } from '@vicons/ionicons5'
import { renderMarkdown } from '../utils/markdown'
import { highlightElement } from '../utils/highlight'
import { fv, pickTitle, pickBody } from '../utils/fields'

const props = defineProps({
  hit: { type: Object, required: true },
  terms: { type: Array, default: () => [] },
  rank: { type: Number, default: 0 },
})

const bodyEl = ref(null)
const expanded = ref(false)

function scoreType(score) {
  if (score >= 0.15) return 'success'
  if (score >= 0.03) return 'info'
  return 'default'
}

// RRF 分数很小（典型 0.01~0.1），映射为视觉强度条
const scorePct = computed(() => Math.min(100, Math.max(5, Number(props.hit.score || 0) * 900)))

async function render() {
  await nextTick()
  if (!bodyEl.value) return
  const doc = props.hit.document || {}
  const fields = doc.fields || {}
  const text = pickBody(fields)
  bodyEl.value.innerHTML = text ? renderMarkdown(text) : '<span style="opacity:.55">(无正文)</span>'
  highlightElement(bodyEl.value, props.terms)
}

watch(() => [props.hit, props.terms], render, { deep: true })
onMounted(render)

const title = () => {
  const doc = props.hit.document || {}
  return pickTitle(doc.fields || {}) || props.hit.id || '(untitled)'
}
const fieldNames = () => {
  const doc = props.hit.document || {}
  return Object.keys(doc.fields || {})
}
</script>

<template>
  <n-card :bordered="true" class="hit-card" :class="{ expanded }" size="small">
    <div class="hit-head">
      <div class="hit-head-left">
        <span class="rank-badge">{{ rank }}</span>
        <n-text depth="3" class="hit-id">{{ hit.id }}</n-text>
      </div>
      <n-tooltip trigger="hover">
        <template #trigger>
          <div class="score-wrap">
            <span class="score-text">score {{ Number(hit.score).toFixed(4) }}</span>
            <div class="score-bar">
              <div
                class="score-bar-fill"
                :class="'lv-' + scoreType(hit.score)"
                :style="{ width: scorePct + '%' }"
              />
            </div>
          </div>
        </template>
        混合检索融合得分（BM25 × 向量 RRF）
      </n-tooltip>
    </div>

    <div class="hit-title">{{ title() }}</div>

    <div ref="bodyEl" class="mss-markdown hit-body" :class="{ clamped: !expanded }" />

    <div class="hit-foot">
      <div class="field-tags">
        <n-tag v-for="f in fieldNames()" :key="f" size="tiny" :bordered="false" type="info" round>{{
          f
        }}</n-tag>
      </div>
      <n-button text size="tiny" class="expand-btn" @click="expanded = !expanded">
        <template #icon
          ><n-icon :component="expanded ? ChevronUpOutline : ChevronDownOutline"
        /></template>
        {{ expanded ? '收起' : '展开' }}
      </n-button>
    </div>
  </n-card>
</template>

<style scoped>
.hit-card {
  border-radius: 14px;
  transition:
    box-shadow 0.2s,
    transform 0.2s;
  animation: mss-fade-up 0.3s ease-out;
}
.hit-card:hover {
  box-shadow: 0 8px 28px rgba(16, 24, 40, 0.1);
  transform: translateY(-1px);
}
.hit-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  margin-bottom: 6px;
}
.hit-head-left {
  display: flex;
  align-items: center;
  gap: 8px;
  overflow: hidden;
}
.rank-badge {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-width: 22px;
  height: 22px;
  padding: 0 6px;
  border-radius: 7px;
  background: linear-gradient(135deg, #6366f1, #8b5cf6);
  color: #fff;
  font-size: 11px;
  font-weight: 700;
  flex-shrink: 0;
}
.hit-id {
  font-size: 12px;
  font-family: var(--font-family-mono, monospace);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.score-wrap {
  display: flex;
  align-items: center;
  gap: 8px;
}
.score-text {
  font-size: 12px;
  font-weight: 600;
  color: #6366f1;
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}
.score-bar {
  width: 64px;
  height: 4px;
  border-radius: 2px;
  background: rgba(127, 127, 127, 0.18);
  overflow: hidden;
}
.score-bar-fill {
  height: 100%;
  border-radius: 2px;
  transition: width 0.4s ease;
}
.score-bar-fill.lv-success {
  background: #10b981;
}
.score-bar-fill.lv-info {
  background: #0ea5e9;
}
.score-bar-fill.lv-default {
  background: #94a3b8;
}
.hit-title {
  font-size: 15px;
  font-weight: 600;
  margin: 4px 0 6px;
}
.hit-body {
  font-size: 13px;
}
.hit-body.clamped {
  max-height: 180px;
  overflow: hidden;
  position: relative;
}
.hit-body.clamped::after {
  content: '';
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  height: 48px;
  background: linear-gradient(transparent, var(--n-card-color, #fff));
  pointer-events: none;
}
.hit-foot {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 10px;
  padding-top: 8px;
  border-top: 1px solid rgba(127, 127, 127, 0.12);
}
.field-tags {
  flex: 1;
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}
.expand-btn {
  color: #94a3b8;
}
</style>
