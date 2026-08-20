// main.tsx — SPA 入口：桌面壳识别（Wails webview）→ 全局 tooltip →
// AppController 启动编排（token gate → meta 验活 → 会话列表 → SSE 直播）。

import { createRoot } from 'react-dom/client'
import './app.css'
import { App } from './App'
import { AppController } from './app/controller'
import { initTooltips } from './components/ui/Tooltip'

const controller = new AppController()

// 调试钩子：桌面壳无 DevTools 快捷键时也可经 WKWebView 控制台/脚本桥检查状态
;(window as unknown as { __loom?: AppController }).__loom = controller

initTooltips() // 全局接管 title 属性 → 主题化悬浮提示

// 桌面端隐藏了原生标题栏（mac.TitleBarHidden），红绿灯悬浮在内容之上；
// body.is-desktop 用于开启让位/拖动区样式（见 app.css 末尾）。
if (controller.isDesktopShell) {
  document.body.classList.add('is-desktop')
  // 窗口拖动：Wails 的 --wails-draggable 在其前端 runtime（/wails/runtime.js）
  // 里实现，但桌面端 SPA 跑在 loopback origin（SSE 需要真实 HTTP，见
  // DESKTOP_DESIGN.md §2.3），加载不到该文件，这里自行实现等价逻辑。
  // 原生链路：external message handler 注册在 WKUserContentController 上
  // （与 origin 无关），收到 "drag" 即 performWindowDragWithEvent:，
  // mouseEvent 由 NSEvent 本地监听器跟踪（WailsContext.m）。
  const wailsDrag = (
    window as unknown as {
      webkit?: { messageHandlers?: { external?: { postMessage: (m: string) => void } } }
    }
  ).webkit?.messageHandlers?.external
  if (wailsDrag) {
    let dragArmed = false
    window.addEventListener('mousedown', (e) => {
      // 仅左键单击命中 drag 区域时武装；双击保留给系统（窗口缩放）
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
  // 失焦时 macOS 会把红绿灯绘成非激活态（透明标题栏下呈黑色，由 AppKit
  // 绘制，web 层改不了按钮本身）；同步给顶行加淡化样式，呈现统一的原生
  // 非激活观感。
  window.addEventListener('blur', () => document.body.classList.add('is-inactive'))
  window.addEventListener('focus', () => document.body.classList.remove('is-inactive'))
}

createRoot(document.getElementById('root')!).render(<App controller={controller} />)
void controller.boot()
