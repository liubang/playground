<script setup>
import { ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import {
  NCard,
  NForm,
  NFormItem,
  NInput,
  NButton,
  NIcon,
  NAlert,
  NText,
  NGradientText,
  useMessage,
} from 'naive-ui'
import {
  SparklesOutline,
  LockClosedOutline,
  PersonOutline,
  MoonOutline,
  SunnyOutline,
} from '@vicons/ionicons5'
import * as api from '../api/client'
import { saveSession, darkThemeState, toggleDark } from '../store/session'

const route = useRoute()
const router = useRouter()
const message = useMessage()

const user = ref('')
const password = ref('')
const loading = ref(false)
const errorMsg = ref('')

async function onSubmit() {
  if (!user.value || !password.value) {
    errorMsg.value = '请输入用户名和密码'
    return
  }
  loading.value = true
  errorMsg.value = ''
  try {
    const data = await api.login(user.value, password.value)
    saveSession({ ...data, user: user.value })
    message.success('登录成功')
    const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : '/search'
    router.push(redirect)
  } catch (err) {
    errorMsg.value = err.message || '登录失败'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="login-wrap">
    <div class="login-bg">
      <div class="orb" />
    </div>

    <n-card class="login-card" :bordered="false">
      <div class="login-head">
        <div class="logo-mark">
          <n-icon :component="SparklesOutline" />
        </div>
        <h1 class="login-title">
          <n-gradient-text type="primary">MiniSearch</n-gradient-text>
        </h1>
        <n-text depth="3">混合检索服务控制台 · Hybrid Search Console</n-text>
        <div class="divider" />
      </div>

      <n-form size="large" @submit.prevent="onSubmit">
        <n-form-item label="用户名">
          <n-input v-model:value="user" placeholder="console admin user" autocomplete="username">
            <template #prefix><n-icon :component="PersonOutline" /></template>
          </n-input>
        </n-form-item>
        <n-form-item label="密码">
          <n-input
            v-model:value="password"
            type="password"
            show-password-on="click"
            placeholder="••••••••"
            autocomplete="current-password"
            @keydown.enter="onSubmit"
          >
            <template #prefix><n-icon :component="LockClosedOutline" /></template>
          </n-input>
        </n-form-item>

        <n-alert v-if="errorMsg" type="error" :show-icon="false" class="login-error">{{
          errorMsg
        }}</n-alert>

        <n-button
          type="primary"
          block
          size="large"
          :loading="loading"
          attr-type="submit"
          class="login-btn"
        >
          登 录
        </n-button>
      </n-form>

      <div class="login-foot">
        <n-button quaternary circle size="small" class="foot-icon" @click="toggleDark">
          <template #icon
            ><n-icon :component="darkThemeState.dark ? SunnyOutline : MoonOutline"
          /></template>
        </n-button>
        <n-text depth="3" class="foot-tag">BM25 × 向量 × Rerank</n-text>
      </div>
    </n-card>
  </div>
</template>

<style scoped>
.login-wrap {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 100vh;
  padding: 24px;
  overflow: hidden;
  background: var(--mss-bg);
}
.login-bg {
  position: absolute;
  inset: 0;
  z-index: 0;
}
/* 单个静态光斑：登录页长期驻留，不做无限动画 */
.orb {
  position: absolute;
  border-radius: 50%;
  filter: blur(90px);
  width: 520px;
  height: 520px;
  top: -140px;
  left: -100px;
  background: radial-gradient(circle, rgba(99, 102, 241, 0.35), transparent 70%);
}
html.dark .orb {
  background: radial-gradient(circle, rgba(99, 102, 241, 0.22), transparent 70%);
}
.login-card {
  position: relative;
  z-index: 1;
  width: 400px;
  max-width: 94vw;
  border-radius: var(--mss-radius-l);
  box-shadow: var(--mss-shadow-2);
  background: rgba(255, 255, 255, 0.82);
  backdrop-filter: blur(16px) saturate(160%);
}
html.dark .login-card {
  background: rgba(30, 41, 59, 0.8);
}
.login-head {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  margin-bottom: 26px;
  text-align: center;
}
.logo-mark {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 56px;
  height: 56px;
  border-radius: var(--mss-radius-l);
  background: var(--mss-brand-grad); /* 渐变仅保留在 logo mark */
  color: #fff;
  font-size: 28px;
  margin-bottom: 6px;
  box-shadow: 0 10px 28px rgba(99, 102, 241, 0.4);
}
.login-title {
  margin: 0;
  font-size: 26px;
  letter-spacing: 0.5px;
}
.divider {
  width: 56px;
  height: 3px;
  border-radius: 2px;
  background: var(--mss-brand);
  margin-top: 8px;
}
.login-error {
  margin-bottom: 14px;
}
.login-btn {
  margin-top: 6px;
  letter-spacing: 2px;
}
.login-foot {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-top: 22px;
}
.foot-icon {
  color: inherit;
}
.foot-tag {
  font-size: 12px;
}
</style>
