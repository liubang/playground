// Prevents theme flash (FOUC): pre-sets data-theme before first paint. External script rather than inline,
// for compatibility with the server CSP (script-src 'self', inline scripts forbidden). Loaded synchronously (no
// async/defer) to guarantee execution before first render. Key name/values stay in sync with src/app/controller.ts
// THEME_KEY; the share page reuses the same preference for a consistent look.
try {
  var t = localStorage.getItem('loom_theme')
  document.documentElement.dataset.theme = t === 'light' ? 'light' : 'dark'
} catch (e) {}
