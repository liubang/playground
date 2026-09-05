// Confirm.tsx — Confirmation dialog (replaces the native confirm).
// confirmDialog({title, body, okLabel}) → Promise<boolean>
// Every call site is a destructive or costly operation (delete session/workspace/skill, unshare,
// install rule packs, discard unsaved changes), so focus defaults to Cancel: Esc / clicking the overlay /
// Enter (while the cancel button is focused) = cancel; confirming requires an explicit click or Tab — so that a
// single Enter does not run an irreversible action while the dialog overlays a focused input. <ConfirmHost /> lives at the App root.

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

// Concurrency queue: only one confirm dialog is open at a time; concurrent/nested confirmDialog calls queue
// up, with the next one taking over once the current settles (previously the latter overwrote the former, whose
// Promise never resolved, leaving the caller hanging on await).
type Pending = ConfirmRequest & { resolve: (v: boolean) => void }
const queue: Pending[] = []
// Holder of the focus before the dialog opened: restored once the last one settles (see settle).
let returnFocusTo: HTMLElement | null = null

export function confirmDialog(req: ConfirmRequest): Promise<boolean> {
  return new Promise((resolve) => {
    queue.push({ ...req, resolve })
    if (!confirmStore.get().req) next()
  })
}

function next() {
  const req = queue.shift()
  if (!req) return
  // The dialog is about to take over focus (cancel button autoFocus): record the current one first
  returnFocusTo = document.activeElement as HTMLElement | null
  confirmStore.set({ req })
}

// Settle the current dialog: resolve the caller's Promise; if the queue is non-empty hand over to the next, else return the focus
// to the triggering element (same semantics as the lightbox; skipped if the element is unmounted).
function settle(v: boolean) {
  const req = confirmStore.get().req
  if (!req) return
  confirmStore.set({ req: null })
  req.resolve(v)
  if (queue.length > 0) {
    next()
    return
  }
  const focusBack = returnFocusTo
  returnFocusTo = null
  if (focusBack?.isConnected) focusBack.focus()
}

// isConfirmOpen lets other Esc handlers (e.g. the settings panel) yield: while a dialog is open, Esc is consumed here.
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
        settle(false)
        return
      }
      if (e.key === 'Tab') {
        // Focus trap: Tab/Shift+Tab cycle within the dialog without leaking into the page behind
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
      // Do not consume Enter in the document capture phase: when the dialog appears the user's focus may still rest
      // on an input behind it, where a global Enter=confirm would turn an ordinary input newline into a dangerous action.
      // Enter/Space confirmation is left to the native browser behavior of the focused button.
    }
    document.addEventListener('keydown', onKey, true)
    return () => document.removeEventListener('keydown', onKey, true)
  }, [req])

  if (!req) return <div id="confirm-modal" className="modal-wrap" hidden />

  return (
    // eslint-disable-next-line jsx-a11y/no-static-element-interactions
    <div
      id="confirm-modal"
      className="modal-wrap"
      onClick={(e) => {
        if (e.target === e.currentTarget) settle(false)
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
            onClick={() => settle(false)}
          >
            取消
          </button>
          <button
            id="confirm-ok"
            className="btn btn-danger"
            type="button"
            onClick={() => settle(true)}
          >
            {req.okLabel || '确认'}
          </button>
        </div>
      </div>
    </div>
  )
}
