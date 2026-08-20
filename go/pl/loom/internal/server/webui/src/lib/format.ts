// format.ts — 展示格式化小工具（与旧 static/js/format.js 一一对应）。

export function fmtTokens(n: number | null | undefined): string {
  if (n == null || isNaN(n)) return ''
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(1) + 'M'
  if (n >= 1000) return (n / 1000).toFixed(1) + 'k'
  return String(n)
}

export function fmtBytes(n: number | null | undefined): string {
  if (n == null || isNaN(n)) return ''
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + ' MB'
  if (n >= 1 << 10) return (n / (1 << 10)).toFixed(1) + ' KB'
  return n + ' B'
}

export function relTime(iso?: string): string {
  const t = new Date(iso || '')
  if (isNaN(t.getTime())) return ''
  const s = Math.max(0, (Date.now() - t.getTime()) / 1000)
  if (s < 60) return 'just now'
  if (s < 3600) return Math.floor(s / 60) + 'm ago'
  if (s < 86400) return Math.floor(s / 3600) + 'h ago'
  if (s < 86400 * 7) return Math.floor(s / 86400) + 'd ago'
  return t.toLocaleDateString()
}

// 消息时间展示：与截图风格一致（如 "8月6日 14:34"）。
export function fmtMsgTime(iso?: string): string {
  const t = new Date(iso || '')
  if (isNaN(t.getTime())) return ''
  const m = t.getMonth() + 1
  const d = t.getDate()
  const hh = String(t.getHours()).padStart(2, '0')
  const mm = String(t.getMinutes()).padStart(2, '0')
  return `${m}月${d}日 ${hh}:${mm}`
}

// 悬浮提示用的完整时间（含年份与秒），便于精确定位。
export function fmtMsgTimeTitle(iso?: string): string {
  const t = new Date(iso || '')
  if (isNaN(t.getTime())) return ''
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${t.getFullYear()}-${pad(t.getMonth() + 1)}-${pad(t.getDate())} ${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())}`
}

export function shortId(id?: string): string {
  if (!id) return ''
  return id.length > 12 ? id.slice(0, 12) + '…' : id
}

// randomId 生成 UUIDv4。crypto.randomUUID 是 Secure-Context-Only API，
// 内网 IP（http://192.168.x.x）访问时不存在；getRandomValues 不受此限，
// 用它手工拼 UUIDv4 作降级（WebUI 需支持非回环地址访问）。
export function randomId(): string {
  if (crypto.randomUUID) return crypto.randomUUID()
  const bytes = crypto.getRandomValues(new Uint8Array(16))
  bytes[6] = (bytes[6] & 0x0f) | 0x40 // version 4
  bytes[8] = (bytes[8] & 0x3f) | 0x80 // variant 1
  const hex = [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

// copyText 复制到剪贴板，返回是否成功。navigator.clipboard 同样是
// Secure-Context-Only：内网 IP 访问时为 undefined，降级到 execCommand；
// 两者都失败返回 false，由调用方决定兜底（不允许静默假成功）。
export async function copyText(text: string): Promise<boolean> {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text)
      return true
    } catch {
      /* 权限拒绝等，走降级 */
    }
  }
  const ta = document.createElement('textarea')
  ta.value = text
  ta.style.cssText = 'position:fixed;top:0;left:0;opacity:0'
  document.body.appendChild(ta)
  ta.focus()
  ta.select()
  let ok = false
  try {
    ok = document.execCommand('copy')
  } catch {
    /* 忽略 */
  }
  ta.remove()
  return ok
}

// fmtDuration 工具耗时展示。
export function fmtDuration(ms: number): string {
  if (ms < 1000) return ms + 'ms'
  return (ms / 1000).toFixed(1) + 's'
}
