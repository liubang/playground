<script setup>
import {
  NConfigProvider,
  NGlobalStyle,
  NMessageProvider,
  NDialogProvider,
  NNotificationProvider,
  zhCN,
  dateZhCN,
} from 'naive-ui'
import { computed, watchEffect } from 'vue'
import { darkThemeState } from './store/session'
import { resolveTheme, lightThemeOverrides, darkThemeOverrides } from './theme'

const theme = computed(() => resolveTheme(darkThemeState.dark))
const themeOverrides = computed(() =>
  darkThemeState.dark ? darkThemeOverrides : lightThemeOverrides,
)

// 自主维护 html.dark class：组件内的暗色微调选择器（.dark .foo）以此为锚，
// 不依赖 naive-ui 内部是否渲染 .n-dark。
watchEffect(() => {
  document.documentElement.classList.toggle('dark', darkThemeState.dark)
})
</script>

<template>
  <n-config-provider
    :theme="theme"
    :theme-overrides="themeOverrides"
    :locale="zhCN"
    :date-locale="dateZhCN"
  >
    <n-global-style />
    <n-message-provider placement="top">
      <n-dialog-provider>
        <n-notification-provider>
          <router-view />
        </n-notification-provider>
      </n-dialog-provider>
    </n-message-provider>
  </n-config-provider>
</template>
