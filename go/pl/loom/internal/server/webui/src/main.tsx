// main.tsx — SPA entry: desktop-shell detection (Wails webview) → global tooltip →
// AppController boot orchestration (token gate → meta health check → session list → live SSE).

import { createRoot } from 'react-dom/client'
import './app.css'
import { App } from './App'
import { AppController } from './app/controller'
import { initTooltips } from './components/ui/Tooltip'

const controller = new AppController()

// Debug hook: even without a DevTools shortcut in the desktop shell, state can be inspected via the WKWebView console/script bridge
;(window as unknown as { __loom?: AppController }).__loom = controller

initTooltips() // globally take over title attributes → themed hover tooltips

// The desktop app hides the native title bar (mac.TitleBarHidden); the traffic lights float above the content;
// body.is-desktop enables the yielding/drag-area styles (see styles/desktop.css).
if (controller.isDesktopShell) {
  document.body.classList.add('is-desktop')
  // Window dragging: Wails' --wails-draggable is implemented in its frontend runtime (/wails/runtime.js),
  // but the desktop SPA runs on a loopback origin (SSE needs real HTTP, see
  // DESKTOP_DESIGN.md §2.3) and cannot load that file, so an equivalent logic is implemented here.
  // Native chain: the external message handler is registered on WKUserContentController
  // (origin-independent); receiving "drag" triggers performWindowDragWithEvent:, and
  // the mouseEvent is tracked by an NSEvent local monitor (WailsContext.m).
  const wailsDrag = (
    window as unknown as {
      webkit?: { messageHandlers?: { external?: { postMessage: (m: string) => void } } }
    }
  ).webkit?.messageHandlers?.external
  if (wailsDrag) {
    let dragArmed = false
    window.addEventListener('mousedown', (e) => {
      // Arm only when a left single-click hits a drag region; double clicks stay with the system (window zoom)
      dragArmed =
        e.buttons === 1 &&
        e.detail === 1 &&
        getComputedStyle(e.target as Element)
          .getPropertyValue('--wails-draggable')
          .trim() === 'drag'
    })
    window.addEventListener('mousemove', (e) => {
      if (dragArmed && e.buttons === 1) {
        dragArmed = false
        wailsDrag.postMessage('drag')
      }
    })
    window.addEventListener('mouseup', () => {
      dragArmed = false
    })
  }
  // On blur, macOS draws the traffic lights in their inactive state (they appear dark under a transparent title bar,
  // drawn by AppKit — the web layer can't change the buttons themselves); correspondingly add a fade to the top row
  // for a consistent native inactive appearance.
  window.addEventListener('blur', () => document.body.classList.add('is-inactive'))
  window.addEventListener('focus', () => document.body.classList.remove('is-inactive'))
}

createRoot(document.getElementById('root')!).render(<App controller={controller} />)
void controller.boot()
