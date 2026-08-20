// controls.tsx — spec 驱动的字段控件（受控组件）。
// 密钥控件展示服务端的脱敏占位符，未修改时原样回传由服务端还原；点击眼睛
// 按钮经 POST /v1/config/reveal 按需取回明文（明文不随整体配置下发）。

import { useState } from 'react'
import type { FieldSpec } from './spec'
import { SECRET_MASK } from './spec'
import type { ControlState } from './convert'
import { Select } from '../ui/Select'
import { confirmDialog } from '../ui/Confirm'
import { Icon } from '../../lib/icons'

export interface FieldRowProps {
  spec: FieldSpec
  value: ControlState
  onChange: (v: ControlState) => void
  // onReveal: 可选，async () => 明文 | null（失败已自行提示）；仅 password
  // 控件且当前值是掩码时参与——用户已输入新值时直接切换可见性即可。
  onReveal?: (() => Promise<string | null>) | null
  // invalid: 校验失败标红（开始编辑后由面板摘除）
  invalid?: boolean
}

export function FieldRow({ spec, value, onChange, onReveal, invalid }: FieldRowProps) {
  const t = spec.type || 'text'

  const change = (v: ControlState) => {
    onChange(v)
  }

  let ctl: React.ReactNode
  if (t === 'select' || t === 'tristate') {
    const opts: [string, string][] =
      t === 'tristate'
        ? [
            ['', `默认（${spec.def || '开'}）`],
            ['true', '开'],
            ['false', '关'],
          ]
        : spec.options || []
    ctl = (
      <Select
        className="set-input"
        options={opts.map(([v, l]) => ({ value: v, label: l }))}
        value={String(value)}
        onChange={(v) => change(v)}
      />
    )
  } else if (t === 'bool' || t === 'flag-list') {
    ctl = (
      <input
        type="checkbox"
        className="set-check"
        checked={value === true}
        onChange={(e) => change(e.target.checked)}
      />
    )
  } else if (t === 'textarea' || t === 'list-text' || t === 'kv-text' || t === 'pair-list') {
    ctl = (
      <textarea
        className={'set-input mono' + (invalid ? ' is-invalid' : '')}
        rows={spec.rows || 3}
        spellCheck={false}
        placeholder={spec.ph}
        value={String(value)}
        onChange={(e) => change(e.target.value)}
      />
    )
  } else {
    ctl = (
      <input
        className={'set-input' + (invalid ? ' is-invalid' : '')}
        type={t === 'password' ? 'password' : t === 'number' ? 'number' : 'text'}
        step={spec.step ? String(spec.step) : undefined}
        spellCheck={false}
        autoComplete="off"
        placeholder={spec.ph}
        value={String(value)}
        onChange={(e) => change(e.target.value)}
      />
    )
  }

  // optionHints：随 select 当前值切换的解释文案
  let hint: string | undefined
  if (spec.optionHints) {
    hint = spec.optionHints[String(value)] ?? spec.hint ?? ''
  } else {
    hint = spec.hint
  }

  return (
    <div className="set-row">
      <label className="set-label">
        {spec.label}
        {spec.required && (
          <span className="set-req" title="必填">
            {' *'}
          </span>
        )}
      </label>
      <div className="set-field">
        {t === 'password' ? (
          <SecretField
            value={String(value)}
            onChange={change}
            onReveal={onReveal || null}
            invalid={!!invalid}
            ph={spec.ph}
          />
        ) : (
          ctl
        )}
        {hint && <div className="set-hint">{hint}</div>}
      </div>
    </div>
  )
}

// SecretField — 密钥输入 + 眼睛按钮（掩码值先经 reveal 接口换回明文）。
function SecretField({
  value,
  onChange,
  onReveal,
  invalid,
  ph,
}: {
  value: string
  onChange: (v: string) => void
  onReveal: (() => Promise<string | null>) | null
  invalid: boolean
  ph?: string
}) {
  const [visible, setVisible] = useState(false)
  const [revealing, setRevealing] = useState(false)

  const toggle = async () => {
    if (visible) {
      setVisible(false)
      return
    }
    if (value === SECRET_MASK && onReveal) {
      setRevealing(true)
      const plaintext = await onReveal()
      setRevealing(false)
      if (plaintext == null) return
      onChange(plaintext)
    }
    setVisible(true)
  }

  return (
    <div className="set-secret">
      <input
        className={'set-input' + (invalid ? ' is-invalid' : '')}
        type={visible ? 'text' : 'password'}
        spellCheck={false}
        autoComplete="off"
        placeholder={ph}
        value={value}
        onChange={(e) => onChange(e.target.value)}
      />
      <button
        type="button"
        className="icon-btn set-eye"
        title="显示/隐藏"
        disabled={revealing}
        onClick={() => void toggle()}
      >
        <Icon name={visible ? 'eye-slash' : 'eye'} />
      </button>
    </div>
  )
}

// SetSection — 卡片式小节容器（标题 + 字段行）。
export function SetSection({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="set-sec set-sec-card">
      <h3 className="set-sec-title">{title}</h3>
      {children}
    </section>
  )
}

// SetTip — 长说明文本：默认一行截断，点击展开/收起（与 skill-desc 同一交互语言）。
export function SetTip({ text }: { text: string }) {
  const [clamp, setClamp] = useState(true)
  return (
    <div
      className={'set-hint set-tip' + (clamp ? ' is-clamp' : '')}
      title="点击展开/收起"
      onClick={() => setClamp((v) => !v)}
    >
      {text}
    </div>
  )
}

// SetLoading — 居中加载/错误占位（面板打开与手动重新加载期间替代空白内容区）。
export function SetLoading({ text, isError }: { text: string; isError?: boolean }) {
  return (
    <div className={'set-loading' + (isError ? ' is-error' : '')}>
      <Icon name={isError ? 'xmark' : 'rotate-left'} />
      {text}
    </div>
  )
}

// SetNavBar — 层级导航共享：面包屑条（返回按钮 + 路径文本），详情态显示。
export function SetNavBar({
  hidden,
  crumb,
  onBack,
}: {
  hidden: boolean
  crumb: string
  onBack: () => void
}) {
  return (
    <div className="set-nav" hidden={hidden}>
      <button type="button" className="btn btn-secondary btn-sm set-nav-back" onClick={onBack}>
        <Icon name="arrow-left" />
        返回
      </button>
      <span className="set-nav-crumb">{crumb}</span>
    </div>
  )
}

// SetCardSummary — 概览行摘要：名称 + 元信息 + 展开箭头；点击进入详情。
export function SetCardSummary({
  name,
  nameEmpty,
  meta,
  onOpen,
}: {
  name: string
  nameEmpty?: boolean
  meta: string
  onOpen: () => void
}) {
  return (
    <button type="button" className="set-card-summary" onClick={onOpen}>
      <span className={'set-card-name' + (nameEmpty ? ' is-empty' : '')}>{name}</span>
      <span className="set-card-meta">{meta}</span>
      <Icon name="caret-right" className="set-card-caret" />
    </button>
  )
}

// CardDelBtn — 卡片删除按钮（高危删除先做二次确认）。
export function CardDelBtn({
  title,
  getConfirm,
  onDelete,
}: {
  title: string
  getConfirm?: () => { title: string; body: string; okLabel: string }
  onDelete: () => void
}) {
  return (
    <button
      type="button"
      className="icon-btn set-card-del"
      title={title}
      onClick={async () => {
        if (getConfirm) {
          const ok = await confirmDialog(getConfirm())
          if (!ok) return
        }
        onDelete()
      }}
    >
      <Icon name="trash" />
    </button>
  )
}
