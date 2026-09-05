// controls.tsx — spec-driven field controls (controlled components).
// Secret controls display the server's masked placeholder; if unmodified it is sent back as-is for the
// server to restore; the eye button fetches the plaintext on demand via POST /v1/config/reveal (the plaintext is not delivered with the whole config).

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
  // onReveal: optional, async () => plaintext | null (failures are already self-reported); only
  // involved for a password control whose current value is the mask — if the user typed a new value, just toggle visibility.
  onReveal?: (() => Promise<string | null>) | null
  // invalid: validation-failure highlight (removed by the panel once editing starts)
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

  // optionHints: explanatory text that switches with the select's current value
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

// SecretField — secret input + eye button (a masked value is first exchanged for plaintext via the reveal endpoint).
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

// SetSection — card-style section container (title + field rows).
export function SetSection({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section className="set-sec set-sec-card">
      <h3 className="set-sec-title">{title}</h3>
      {children}
    </section>
  )
}

// SetTip — long explanatory text: truncated to one line by default, click to expand/collapse (same interaction language as skill-desc).
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

// SetLoading — centered loading/error placeholder (replaces the empty content area while the panel opens and during manual reloads).
export function SetLoading({ text, isError }: { text: string; isError?: boolean }) {
  return (
    <div className={'set-loading' + (isError ? ' is-error' : '')}>
      <Icon name={isError ? 'xmark' : 'rotate-left'} />
      {text}
    </div>
  )
}

// SetNavBar — shared hierarchical navigation: breadcrumb bar (back button + path text), shown in detail state.
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

// SetCardSummary — overview-row summary: name + meta + expand arrow; click to enter detail.
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

// CardDelBtn — card delete button (high-risk deletes require a confirmation first).
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
