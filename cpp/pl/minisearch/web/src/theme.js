import { darkTheme } from 'naive-ui'

// MiniSearch 品牌主色：靛蓝紫
export const brand = {
  primary: '#6366f1',
  primaryHover: '#4f46e5',
  primaryPressed: '#4338ca',
  primarySuppl: '#818cf8',
  info: '#0ea5e9',
  success: '#10b981',
  warning: '#f59e0b',
  error: '#ef4444',
}

// 亮色主题覆盖
export const lightThemeOverrides = {
  common: {
    primaryColor: brand.primary,
    primaryColorHover: brand.primaryHover,
    primaryColorPressed: brand.primaryPressed,
    primaryColorSuppl: brand.primarySuppl,
    infoColor: brand.info,
    successColor: brand.success,
    warningColor: brand.warning,
    errorColor: brand.error,
    borderRadius: '10px',
    borderRadiusSmall: '6px',
    fontFamily:
      "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans SC', 'PingFang SC', 'Microsoft YaHei', sans-serif",
    fontFamilyMono:
      "'SF Mono', 'Fira Code', 'Cascadia Code', 'JetBrains Mono', Consolas, monospace",
    cardColor: '#ffffff',
    modalColor: '#ffffff',
    popoverColor: '#ffffff',
    bodyColor: '#f6f7fb',
    boxShadow1: '0 1px 3px rgba(16, 24, 40, 0.06), 0 1px 2px rgba(16, 24, 40, 0.04)',
    boxShadow2: '0 4px 16px rgba(16, 24, 40, 0.08), 0 2px 4px rgba(16, 24, 40, 0.04)',
    boxShadow3: '0 12px 40px rgba(16, 24, 40, 0.12)',
  },
  Card: {
    borderRadius: '12px',
    color: '#ffffff',
    borderColor: 'rgba(15, 23, 42, 0.08)',
    boxShadow: '0 1px 3px rgba(16, 24, 40, 0.04)',
  },
  Button: {
    borderRadiusMedium: '8px',
    borderRadiusLarge: '10px',
    fontWeight: '600',
  },
  Input: {
    borderRadius: '10px',
  },
  Select: {
    peers: {
      InternalSelection: { borderRadius: '10px' },
    },
  },
  Menu: {
    itemBorderRadius: '8px',
    itemColorActive: 'rgba(99, 102, 241, 0.12)',
    itemTextColorActive: '#4f46e5',
    itemTextColorActiveHover: '#4f46e5',
    itemIconColorActive: '#6366f1',
    itemIconColorActiveHover: '#6366f1',
  },
  DataTable: {
    borderRadius: '12px',
    thColor: '#f8fafc',
    tdColorHover: 'rgba(99, 102, 241, 0.04)',
  },
  Modal: {
    borderRadius: '16px',
  },
  Tag: {
    borderRadius: '6px',
  },
  Collapse: {
    titleFontWeight: '600',
  },
}

// 暗色主题覆盖（叠加在 darkTheme 上）
export const darkThemeOverrides = {
  common: {
    primaryColor: brand.primary,
    primaryColorHover: brand.primaryHover,
    primaryColorPressed: brand.primaryPressed,
    primaryColorSuppl: brand.primarySuppl,
    infoColor: brand.info,
    successColor: brand.success,
    warningColor: brand.warning,
    errorColor: brand.error,
    borderRadius: '10px',
    borderRadiusSmall: '6px',
    fontFamily: lightThemeOverrides.common.fontFamily,
    fontFamilyMono: lightThemeOverrides.common.fontFamilyMono,
    bodyColor: '#0f172a',
    cardColor: '#1e293b',
    modalColor: '#1e293b',
    popoverColor: '#1e293b',
    boxShadow1: '0 1px 3px rgba(0, 0, 0, 0.3)',
    boxShadow2: '0 4px 16px rgba(0, 0, 0, 0.35)',
    boxShadow3: '0 12px 40px rgba(0, 0, 0, 0.45)',
  },
  Card: {
    borderRadius: '12px',
    color: '#1e293b',
    borderColor: 'rgba(148, 163, 184, 0.14)',
    boxShadow: '0 1px 3px rgba(0, 0, 0, 0.2)',
  },
  Button: {
    borderRadiusMedium: '8px',
    borderRadiusLarge: '10px',
    fontWeight: '600',
  },
  Input: {
    borderRadius: '10px',
  },
  Select: {
    peers: {
      InternalSelection: { borderRadius: '10px' },
    },
  },
  Menu: {
    itemBorderRadius: '8px',
    itemColorActive: 'rgba(99, 102, 241, 0.25)',
    itemTextColorActive: '#a5b4fc',
    itemTextColorActiveHover: '#c7d2fe',
    itemIconColorActive: '#818cf8',
    itemIconColorActiveHover: '#a5b4fc',
  },
  DataTable: {
    borderRadius: '12px',
    thColor: 'rgba(148, 163, 184, 0.08)',
    tdColorHover: 'rgba(99, 102, 241, 0.08)',
  },
  Modal: {
    borderRadius: '16px',
  },
  Tag: {
    borderRadius: '6px',
  },
  Collapse: {
    titleFontWeight: '600',
  },
}

export function resolveTheme(dark) {
  return dark ? darkTheme : null
}
