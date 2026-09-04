// Confirm.tsx — 确认弹窗（替代原生 confirm）。
// confirmDialog({title, body, okLabel}) → Promise<boolean>
// 所有调用点都是破坏性/有代价操作（删除会话/工作区/skill、撤销分享、
// 写入规则包、放弃未保存修改），故默认焦点在「取消」上：Esc / 点遮罩 /
// Enter（命中取消按钮）= 取消；确认必须主动点击或 Tab 过去——避免弹窗
// 叠加在输入焦点上时一次 Enter 就执行不可恢复操作。<ConfirmHost /> 挂在 App 根部。

import { useEffect } from 'react'
import { Store, useStore } from '../../store/store'

export interface ConfirmRequest {
  title: string
  body: string
  okLabel?: string
}

interface ConfirmState {
  req: (ConfirmRequest & { resolve: (v: boolean) => void }) | null
}

const confirmStore = new Store<ConfirmState>({ req: null })

export function confirmDialog(req: ConfirmRequest): Promise<boolean> {
  return new Promise((resolve) => {
    confirmStore.set({ req: { ...req, resolve } })
  })
}

// isConfirmOpen 供其他 Esc 处理方（设置面板）避让：弹窗开着时 Esc 由它自己消费。
export function isConfirmOpen(): boolean {
  return confirmStore.get().req !== null
}

export function ConfirmHost() {
  const req = useStore(confirmStore, (s) => s.req)

  useEffect(() => {
    if (!req) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation()
        done(false)
        return
      }
      if (e.key === 'Tab') {
        // Focus trap：Tab/Shift+Tab 在弹窗内部循环，不泄入背后的页面
        const modal = document.querySelector<HTMLElement>('#confirm-modal .modal')
        if (!modal) return
        const focusables = [...modal.querySelectorAll<HTMLElement>('button:not([disabled])')]
        if (focusables.length === 0) return
        const first = focusables[0]
        const last = focusables[focusables.length - 1]
        const active = document.activeElement as HTMLElement | null
        if (e.shiftKey && (active === first || !modal.contains(active))) {
          e.preventDefault()
          last.focus()
        } else if (!e.shiftKey && (active === last || !modal.contains(active))) {
          e.preventDefault()
          first.focus()
        }
      }
      // 不在 document 捕获阶段消费 Enter：弹窗出现时用户焦点可能还停留在
      // 背后的输入框里，全局 Enter=确认 会把一次普通的输入回车变成危险操作。
      // Enter/Space 的确认留给聚焦按钮的浏览器原生行为。
    }
    const done = (v: boolean) => {
      document.removeEventListener('keydown', onKey, true)
      confirmStore.set({ req: null })
      req.resolve(v)
    }
    document.addEventListener('keydown', onKey, true)
    return () => document.removeEventListener('keydown', onKey, true)
  }, [req])

  if (!req) return <div id="confirm-modal" className="modal-wrap" hidden />

  const done = (v: boolean) => {
    confirmStore.set({ req: null })
    req.resolve(v)
  }

  return (
    // eslint-disable-next-line jsx-a11y/no-static-element-interactions
    <div
      id="confirm-modal"
      className="modal-wrap"
      onClick={(e) => {
        if (e.target === e.currentTarget) done(false)
      }}
    >
      <div className="modal" role="dialog" aria-modal="true" aria-labelledby="confirm-title">
        <div className="modal-title" id="confirm-title">
          {req.title}
        </div>
        <div className="modal-body" id="confirm-body">
          {req.body}
        </div>
        <div className="modal-actions">
          <button
            id="confirm-cancel"
            className="btn btn-secondary"
            type="button"
            autoFocus
            onClick={() => done(false)}
          >
            取消
          </button>
          <button
            id="confirm-ok"
            className="btn btn-danger"
            type="button"
            onClick={() => done(true)}
          >
            {req.okLabel || '确认'}
          </button>
        </div>
      </div>
    </div>
  )
}
