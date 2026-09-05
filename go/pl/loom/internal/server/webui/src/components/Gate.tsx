// Gate.tsx — token onboarding page (first visit / 401). Rendered standalone, without the app shell.

import { memo, useState } from 'react'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'

export const Gate = memo(function Gate({ controller }: { controller: AppController }) {
  const gateError = useStore(controller.store, (s) => s.gateError)
  const gateLocked = useStore(controller.store, (s) => s.gateLocked)
  const [token, setToken] = useState('')

  return (
    <div id="gate" className="gate-wrap">
      <form
        id="gate-form"
        className="gate"
        onSubmit={(e) => {
          e.preventDefault()
          const t = token.trim()
          if (!t) return
          controller.submitGateToken(t)
        }}
      >
        <div className="brand">◆ loom</div>
        <p>输入 serve token 以连接。</p>
        <input
          id="gate-token"
          type="password"
          autoComplete="off"
          autoFocus
          placeholder="token"
          required
          hidden={gateLocked}
          value={token}
          onChange={(e) => setToken(e.target.value)}
        />
        <div id="gate-error" className="gate-error" hidden={!gateError}>
          {gateError}
        </div>
        <button
          className="btn btn-primary"
          type="submit"
          style={{ width: '100%' }}
          hidden={gateLocked}
        >
          连接
        </button>
        <div className="hint">
          仅保存在 sessionStorage 中。
          <br />
          首次启动 <code>loom serve</code> 时会打印 token，也可在 loom 数据目录查看{' '}
          <code>serve.token</code>。
        </div>
      </form>
    </div>
  )
})
