import { reactive, computed } from 'vue'

const TOKEN_KEY = 'mss_token'
const USER_KEY = 'mss_user'
const TENANT_KEY = 'mss_tenant'
const ROLE_KEY = 'mss_role'
const DARK_KEY = 'mss_dark'

function load(key) {
  try {
    return sessionStorage.getItem(key) || ''
  } catch {
    return ''
  }
}

export const session = reactive({
  token: load(TOKEN_KEY),
  user: load(USER_KEY),
  tenant: load(TENANT_KEY),
  role: load(ROLE_KEY),
})

export const isLoggedIn = computed(() => !!session.token)

export function saveSession(data) {
  session.token = data.token || ''
  session.user = data.user || ''
  session.tenant = data.tenant || 'default'
  session.role = data.role || 'admin'
  try {
    sessionStorage.setItem(TOKEN_KEY, session.token)
    sessionStorage.setItem(USER_KEY, session.user)
    sessionStorage.setItem(TENANT_KEY, session.tenant)
    sessionStorage.setItem(ROLE_KEY, session.role)
  } catch {
    /* ignore */
  }
}

export function clearSession() {
  session.token = ''
  session.user = ''
  session.tenant = ''
  session.role = ''
  try {
    for (const k of [TOKEN_KEY, USER_KEY, TENANT_KEY, ROLE_KEY]) sessionStorage.removeItem(k)
  } catch {
    /* ignore */
  }
}

// ---- 暗色模式（localStorage 持久化，不随会话清空） ----
const DARK_STORAGE_KEY = 'mss_dark_theme'

export const darkThemeState = reactive({
  dark: (() => {
    try {
      const saved = localStorage.getItem(DARK_STORAGE_KEY)
      if (saved !== null) return saved === '1'
      return window.matchMedia?.('(prefers-color-scheme: dark)').matches ?? false
    } catch {
      return false
    }
  })(),
})

export function toggleDark() {
  darkThemeState.dark = !darkThemeState.dark
  try {
    localStorage.setItem(DARK_STORAGE_KEY, darkThemeState.dark ? '1' : '0')
  } catch {
    /* ignore */
  }
}
