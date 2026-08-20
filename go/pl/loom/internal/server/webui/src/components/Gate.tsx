// Gate.tsx — token 引导页（首次访问 / 401）。独立渲染，无应用外壳。

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
        <p>Enter the serve token to connect.</p>
        <input
          id="gate-token"
          type="password"
          autoComplete="off"
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
          Connect
        </button>
        <div className="hint">
          Stored in sessionStorage only.
          <br />
          The token is printed on first <code>loom serve</code> start, or see{' '}
          <code>serve.token</code> in the loom data directory.
        </div>
      </form>
    </div>
  )
})
