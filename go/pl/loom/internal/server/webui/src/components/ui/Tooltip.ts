// Tooltip.ts — Themed hover tooltip: takes over the title attribute globally and renders a theme-matched overlay.
// No changes needed at call sites: on mouseover it finds elements carrying a title (closest matches upward,
// same hit semantics as the native tooltip), stashes the text into data-tip and removes title — the native white tooltip
// thus no longer appears; the custom overlay shows after a short delay. Code that later re-sets the title via setAttribute("title")
// (e.g. the MCP badge's tool list) is taken over again on the next hover.
//
// The overlay is a shared singleton with pointer-events:none, centered above the target by default, flipping
// below when space is tight; click/scroll/keypress/window blur dismiss it immediately.
// Mirrors the legacy static/js/components/tooltip.js (framework-agnostic, initialized once at app startup).

const DELAY_MS = 350

let tipEl: HTMLDivElement | null = null // shared overlay
let timer: ReturnType<typeof setTimeout> | 0 = 0
let current: Element | null = null // current hover target

function ensureEl(): HTMLDivElement {
  if (!tipEl) {
    tipEl = document.createElement('div')
    tipEl.className = 'tip'
    tipEl.hidden = true
    document.body.appendChild(tipEl)
  }
  return tipEl
}

function hide() {
  clearTimeout(timer)
  timer = 0
  current = null
  if (tipEl) tipEl.hidden = true
}

function show(target: Element) {
  const text = (target as HTMLElement).dataset.tip
  if (!text || !(target as HTMLElement).isConnected) return
  const tip = ensureEl()
  tip.textContent = text
  tip.hidden = false
  const r = target.getBoundingClientRect()
  const w = tip.offsetWidth
  const h = tip.offsetHeight
  let left = r.left + r.width / 2 - w / 2
  left = Math.max(8, Math.min(left, innerWidth - w - 8))
  let top = r.top - h - 6
  if (top < 8) top = r.bottom + 6 // not enough room above; flip below
  tip.style.left = left + 'px'
  tip.style.top = top + 'px'
}

export function initTooltips() {
  document.addEventListener('mouseover', (e) => {
    const t = e.target instanceof Element ? e.target.closest('[title], [data-tip]') : null
    if (!t) return
    if (t.hasAttribute('title')) {
      // Take over: stash the text and remove the native title (the system tooltip only appears after hover
      // settles, so removing it on mouseover fully suppresses it)
      ;(t as HTMLElement).dataset.tip = t.getAttribute('title') || ''
      t.removeAttribute('title')
    }
    if (!(t as HTMLElement).dataset.tip || current === t) return
    clearTimeout(timer)
    current = t
    timer = setTimeout(() => show(t), DELAY_MS)
  })
  document.addEventListener('mouseout', (e) => {
    if (!current) return
    // Moving between child elements inside the target does not dismiss
    if (e.relatedTarget instanceof Node && current.contains(e.relatedTarget)) return
    hide()
  })
  document.addEventListener('mousedown', hide, true)
  document.addEventListener('scroll', hide, true)
  document.addEventListener('keydown', hide, true)
  window.addEventListener('blur', hide)
}
