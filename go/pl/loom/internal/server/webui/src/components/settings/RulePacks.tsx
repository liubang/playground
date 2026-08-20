// RulePacks.tsx — 规则包（权限 tab 附加小节）：列出内置包与安装状态；
// 安装/卸载写入用户规则目录并热重载。与旧 settings.js 的规则包部分对应。

import { useCallback, useEffect, useState } from 'react'
import type { AppController } from '../../app/controller'
import type { RulePack } from '../../protocol/types'
import { ApiError } from '../../protocol/api'
import { SetTip, SetLoading } from './controls'
import { confirmDialog } from '../ui/Confirm'
import { toast } from '../ui/Toast'
import { Icon } from '../../lib/icons'

export function RulePacks({ controller, active }: { controller: AppController; active: boolean }) {
  const [packs, setPacks] = useState<RulePack[] | null>(null)
  const [loaded, setLoaded] = useState(false)
  const [loading, setLoading] = useState(false)
  const [err, setErr] = useState('')
  const [refreshing, setRefreshing] = useState(false)

  const load = useCallback(
    async (force = false) => {
      if (loading || (loaded && !force)) return
      setLoading(true)
      setErr('')
      if (force) setRefreshing(true)
      try {
        const r = await controller.api.listRulePacks()
        setPacks(r.packs || [])
        setLoaded(true) // 成功才置位：失败后下次切入 tab 允许自动重试
        if (force) toast(`共 ${(r.packs || []).length} 个规则包`, true)
      } catch (e) {
        const err = e as ApiError
        if (err.status !== 401) {
          setErr('加载失败: ' + err.message)
          if (force) toast('规则包加载失败: ' + err.message)
        }
      } finally {
        setLoading(false)
        setRefreshing(false)
      }
    },
    [controller, loading, loaded],
  )

  useEffect(() => {
    if (active) void load()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [active])

  const toggle = async (p: RulePack) => {
    if (!p.installed) {
      const ok = await confirmDialog({
        title: '启用规则包 ' + p.name,
        body:
          `将把 ${(p.commands || []).length} 条命令的 allow 规则写入用户规则目录（pack-${p.id}.json），` +
          '这些命令将在沙箱外运行，可读取你的凭证与 keychain。' +
          (p.risk === 'high' ? '\n\n高风险：该包涉及云 CLI，可能访问云凭证，请确认信任。' : '') +
          '\n\n可在规则文件中随时修改或删除以停用。',
        okLabel: '启用',
      })
      if (!ok) return
    }
    try {
      if (p.installed) {
        await controller.api.uninstallRulePack(p.id)
        toast(`已停用 ${p.name}（立即生效）`, true)
      } else {
        await controller.api.installRulePack(p.id)
        toast(`已启用 ${p.name}（立即生效）`, true)
      }
      // 就地更新这一张卡片（徽标 + 按钮切换），不整表重建
      setPacks((prev) =>
        (prev || []).map((x) => (x.id === p.id ? { ...x, installed: !p.installed } : x)),
      )
    } catch (e) {
      if ((e as ApiError).status !== 401) {
        toast((p.installed ? '停用失败: ' : '启用失败: ') + (e as Error).message)
      }
    }
  }

  return (
    <section className="set-sec set-sec-card">
      <div className="set-skills-bar">
        <h3 className="set-sec-title">规则包（预授权命令）</h3>
        <button
          type="button"
          className={
            'btn btn-secondary btn-sm set-skills-refresh' + (refreshing ? ' is-spinning' : '')
          }
          title="重新读取规则包状态"
          disabled={refreshing}
          onClick={() => void load(true)}
        >
          <Icon name="rotate-left" />
          刷新
        </button>
      </div>
      <SetTip text="部分已知安全的命令（Go 工具链、pip、云 CLI）在 macOS 沙箱内因 TLS 验证必然失败。开启对应规则包后，这些命令直接在沙箱外运行，不再失败、不再逐次审批。开启会把这些命令的 allow 规则写入用户规则目录（pack-*.json），可随时在规则文件中查看、修改或删除；风险等级高的包可能读取云凭证，请按需开启。" />
      <div className="set-skills">
        {loading && !loaded ? (
          <SetLoading text="读取规则包中…" />
        ) : err ? (
          <SetLoading text={err} isError />
        ) : packs && packs.length === 0 ? (
          <div className="set-hint">（无可用规则包）</div>
        ) : (
          (packs || []).map((p) => <RulePackCard key={p.id} p={p} onToggle={toggle} />)
        )}
      </div>
    </section>
  )
}

function RulePackCard({ p, onToggle }: { p: RulePack; onToggle: (p: RulePack) => void }) {
  const [expanded, setExpanded] = useState(false)
  const [busy, setBusy] = useState(false)
  const riskLabel = { low: '低风险', medium: '中风险', high: '高风险' }[p.risk as string] || p.risk
  return (
    <section className="set-sec set-sec-card pack-card">
      <div className="skill-head">
        <span className="skill-name mono">{p.name}</span>
        <span className={'skill-scope' + (p.risk === 'high' ? ' is-repo' : '')}>{riskLabel}</span>
        {p.installed && <span className="skill-scope is-off">已启用</span>}
        <div className="skill-actions">
          <button
            type="button"
            className={p.installed ? 'btn btn-secondary btn-sm' : 'btn btn-primary btn-sm'}
            disabled={busy}
            onClick={async () => {
              setBusy(true)
              await onToggle(p)
              setBusy(false)
            }}
          >
            {p.installed ? '停用' : '启用'}
          </button>
        </div>
      </div>
      {p.description && (
        <div
          className={'set-hint pack-desc' + (expanded ? ' is-expanded' : '')}
          title="点击展开/收起"
          onClick={() => setExpanded((v) => !v)}
        >
          {p.description}
        </div>
      )}
      {p.reason && <div className="set-hint set-tip">{'信任边界：' + p.reason}</div>}
      <div className="pack-cmds">
        {(p.commands || []).map((c) => (
          <code className="pack-cmd mono" key={c}>
            {c}
          </code>
        ))}
      </div>
    </section>
  )
}
