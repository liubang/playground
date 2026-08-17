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
      <div class="orb orb-1" />
      <div class="orb orb-2" />
      <div class="grid" />
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
  background: #f6f7fb;
}
.n-dark .login-wrap {
  background: #0f172a;
}
.login-bg {
  position: absolute;
  inset: 0;
  z-index: 0;
}
.orb {
  position: absolute;
  border-radius: 50%;
  filter: blur(90px);
  opacity: 0.55;
  animation: drift 14s ease-in-out infinite alternate;
}
.orb-1 {
  width: 480px;
  height: 480px;
  top: -120px;
  left: -80px;
  background: radial-gradient(circle, rgba(99, 102, 241, 0.55), transparent 70%);
}
.orb-2 {
  width: 420px;
  height: 420px;
  bottom: -140px;
  right: -60px;
  background: radial-gradient(circle, rgba(139, 92, 246, 0.5), transparent 70%);
  animation-delay: -7s;
}
@keyframes drift {
  from {
    transform: translate(0, 0) scale(1);
  }
  to {
    transform: translate(40px, 24px) scale(1.08);
  }
}
.grid {
  position: absolute;
  inset: 0;
  background-image:
    linear-gradient(rgba(100, 116, 139, 0.07) 1px, transparent 1px),
    linear-gradient(90deg, rgba(100, 116, 139, 0.07) 1px, transparent 1px);
  background-size: 44px 44px;
  mask-image: radial-gradient(ellipse 70% 60% at 50% 45%, #000 30%, transparent 75%);
  -webkit-mask-image: radial-gradient(ellipse 70% 60% at 50% 45%, #000 30%, transparent 75%);
}
.login-card {
  position: relative;
  z-index: 1;
  width: 400px;
  max-width: 94vw;
  border-radius: 20px;
  box-shadow: 0 24px 80px rgba(31, 41, 55, 0.18);
  background: rgba(255, 255, 255, 0.82);
  backdrop-filter: blur(16px) saturate(160%);
}
.n-dark .login-card {
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
  border-radius: 16px;
  background: linear-gradient(135deg, #6366f1 0%, #8b5cf6 100%);
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
  background: linear-gradient(90deg, #6366f1, #8b5cf6);
  margin-top: 8px;
}
.login-error {
  margin-bottom: 14px;
}
.login-btn {
  margin-top: 6px;
  letter-spacing: 2px;
  background: linear-gradient(135deg, #6366f1, #8b5cf6);
  border: none;
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
