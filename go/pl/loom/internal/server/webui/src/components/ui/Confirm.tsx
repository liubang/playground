// Confirm.tsx — 确认弹窗（替代原生 confirm）。
// confirmDialog({title, body, okLabel}) → Promise<boolean>
// Esc / 点遮罩 = 取消；Enter = 确认。<ConfirmHost /> 挂在 App 根部。

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
      } else if (e.key === 'Enter') {
        e.stopPropagation()
        done(true)
      }
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
            onClick={() => done(false)}
          >
            取消
          </button>
          <button
            id="confirm-ok"
            className="btn btn-danger"
            type="button"
            autoFocus
            onClick={() => done(true)}
          >
            {req.okLabel || '确认'}
          </button>
        </div>
      </div>
    </div>
  )
}
