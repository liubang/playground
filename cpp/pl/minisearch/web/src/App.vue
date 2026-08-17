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
import { computed } from 'vue'
import { darkThemeState } from './store/session'
import { resolveTheme, lightThemeOverrides, darkThemeOverrides } from './theme'

const theme = computed(() => resolveTheme(darkThemeState.dark))
const themeOverrides = computed(() =>
  darkThemeState.dark ? darkThemeOverrides : lightThemeOverrides,
)
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
