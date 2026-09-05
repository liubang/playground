// format.ts — display-formatting helpers (one-to-one with the legacy static/js/format.js).

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

// now is injectable: a caller holding a minute-level ticking time source (like the
// Sidebar's ticker) renders the whole column with the same now, preventing individual timestamps from drifting out of sync.
export function relTime(iso?: string, now: number = Date.now()): string {
  const t = new Date(iso || '')
  if (isNaN(t.getTime())) return ''
  const s = Math.max(0, (now - t.getTime()) / 1000)
  if (s < 60) return 'just now'
  if (s < 3600) return Math.floor(s / 60) + 'm ago'
  if (s < 86400) return Math.floor(s / 3600) + 'h ago'
  if (s < 86400 * 7) return Math.floor(s / 86400) + 'd ago'
  return t.toLocaleDateString()
}

// Message time display: matches the screenshot style (e.g. "8月6日 14:34").
export function fmtMsgTime(iso?: string): string {
  const t = new Date(iso || '')
  if (isNaN(t.getTime())) return ''
  const m = t.getMonth() + 1
  const d = t.getDate()
  const hh = String(t.getHours()).padStart(2, '0')
  const mm = String(t.getMinutes()).padStart(2, '0')
  return `${m}月${d}日 ${hh}:${mm}`
}

// Full timestamp (with year and seconds) for the hover tooltip, for precise pinpointing.
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

// randomId generates a UUIDv4. crypto.randomUUID is a Secure-Context-Only API
// and doesn't exist when accessed over an intranet IP (http://192.168.x.x);
// getRandomValues is unrestricted, so hand-assemble a UUIDv4 with it as the fallback (the WebUI must support non-loopback access).
export function randomId(): string {
  if (crypto.randomUUID) return crypto.randomUUID()
  const bytes = crypto.getRandomValues(new Uint8Array(16))
  bytes[6] = (bytes[6] & 0x0f) | 0x40 // version 4
  bytes[8] = (bytes[8] & 0x3f) | 0x80 // variant 1
  const hex = [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
}

// copyText copies to the clipboard, returning whether it succeeded. navigator.clipboard
// is likewise Secure-Context-Only: undefined over intranet IPs, so it degrades to
// execCommand; if both fail, return false and let the caller pick a fallback (no silent fake success).
export async function copyText(text: string): Promise<boolean> {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text)
      return true
    } catch {
      /* permission denied etc.: degrade */
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
    /* ignore */
  }
  ta.remove()
  return ok
}

// fmtDuration displays tool durations.
export function fmtDuration(ms: number): string {
  if (ms < 1000) return ms + 'ms'
  return (ms / 1000).toFixed(1) + 's'
}
