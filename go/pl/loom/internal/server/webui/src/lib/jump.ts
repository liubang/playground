// jump.ts — cross-view anchor seeking. The destination view may still be
// mounting (or display:none) right after a tab switch, so poll until the
// target is rendered and visible, then scroll + flash-highlight it.

const SEEK_TRIES = 40
const SEEK_INTERVAL_MS = 100

export function seekToAnchor(getRoot: () => HTMLElement | null, selector: string) {
  let tries = 0
  const seek = () => {
    const root = getRoot()
    // Virtualized transcript views reveal the authoritative target before the
    // DOM lookup. Other destination views simply ignore this view-only event.
    root?.dispatchEvent(new CustomEvent('loom-reveal-anchor', { detail: selector }))
    let target: Element | null | undefined
    try {
      target = root?.querySelector(selector)
    } catch {
      // A malformed selector must not escape or schedule repeated failed seeks.
      return
    }
    // offsetParent is null while the owning view is hidden.
    if (target instanceof HTMLElement && target.offsetParent !== null) {
      target.scrollIntoView({ block: 'center', behavior: 'smooth' })
      target.animate(
        [{ boxShadow: '0 0 0 3px var(--primary)' }, { boxShadow: '0 0 0 3px transparent' }],
        { duration: 1800, easing: 'ease-out' },
      )
      return
    }
    if (++tries < SEEK_TRIES) setTimeout(seek, SEEK_INTERVAL_MS)
  }
  seek()
}
