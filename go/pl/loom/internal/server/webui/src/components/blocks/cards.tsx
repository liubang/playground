// cards.tsx — approval card and question card (correspond to the old blocks.js approvalCard/questionCard).

import { memo, useState } from 'react'
import type { ApprovalRequestedPayload, QuestionPayload } from '../../protocol/events'
import { Icon } from '../../lib/icons'
import { DiffView } from './DiffView'

// --- approval card ---
// diff (optional): the diff text moved in from the tool block by the caller (dedup: during approval the tool block no
// longer shows it again; it is restored once the approval is resolved).

export const ApprovalCard = memo(function ApprovalCard({
  payload,
  diff,
  resolving,
  onResolve,
}: {
  payload: ApprovalRequestedPayload
  diff?: string
  resolving?: boolean
  onResolve: (decision: 'allow' | 'deny', always: boolean, trust?: string) => void
}) {
  // An empty rule_preview means this call cannot be remembered (backend ApprovalRulePreview);
  // hide Allow always in that case to avoid offering a silently ineffective option.
  const preview = payload.rule_preview || ''
  const trustPreview = payload.trust_preview || ''
  return (
    <div className="block card-approval">
      <div className="card-title">
        <span className="card-title-label">
          <Icon name="circle-question" /> 需要审批
        </span>
        <span className="risk">{'R' + (payload.risk ?? '?')}</span>
        <span className="mono">{payload.tool_name || ''}</span>
      </div>
      {payload.description && <div className="desc">{payload.description}</div>}
      {/* Consequence row: what this operation "will do" (the derived effect), not the command text itself */}
      {payload.consequence && <div className="consequence">{payload.consequence}</div>}
      {/* Show cmd only when target differs from description, avoiding rendering the same text twice */}
      {payload.target && payload.target !== payload.description && (
        <div className="cmd">
          <code>{payload.target}</code>
        </div>
      )}
      {diff && <DiffView diffText={diff} />}
      <div className="actions">
        <button
          type="button"
          className="btn btn-primary"
          disabled={resolving}
          onClick={() => onResolve('allow', false)}
        >
          允许
        </button>
        {preview && (
          <button
            type="button"
            className="btn btn-secondary"
            disabled={resolving}
            onClick={() => onResolve('allow', true)}
          >
            总是允许
          </button>
        )}
        {trustPreview && (
          <button
            type="button"
            className="btn btn-danger"
            disabled={resolving}
            onClick={() => onResolve('allow', true, 'unsandboxed')}
          >
            信任（无沙箱）
          </button>
        )}
        <button
          type="button"
          className="btn btn-danger"
          disabled={resolving}
          onClick={() => onResolve('deny', false)}
        >
          拒绝
        </button>
        {preview && (
          <span className="memo">{`“总是允许”会把 "${preview}" 记入此工作区的规则`}</span>
        )}
        {trustPreview && (
          <span className="memo">{`“信任”会以完整用户权限记住 "${trustPreview}"`}</span>
        )}
      </div>
    </div>
  )
})

// --- question card ---

export const QuestionCard = memo(function QuestionCard({
  payload,
  resolving,
  onAnswer,
}: {
  payload: QuestionPayload
  resolving?: boolean
  onAnswer: (answer: { selected: string[]; custom_text: string; skipped: boolean }) => void
}) {
  // On snapshot rebuild the payload is PendingRequest.Question (wrapped in a question field)
  const q = (payload as { question?: QuestionPayload }).question || payload
  const inputType = q.allow_multiple ? 'checkbox' : 'radio'
  const [name] = useState(() => 'q_' + Math.random().toString(36).slice(2, 8))
  const [custom, setCustom] = useState('')
  // Controlled selection state: previously read the DOM directly via querySelectorAll(':checked'); when the component is
  // reused by React / the payload changes / resolving toggles a re-render, the DOM's checked state can
  // drift from the component's intent. Unified into state.
  const [selected, setSelected] = useState<string[]>([])
  const toggle = (label: string) =>
    q.allow_multiple
      ? setSelected((prev) =>
          prev.includes(label) ? prev.filter((x) => x !== label) : [...prev, label],
        )
      : setSelected([label])

  return (
    <div className="block card-question">
      <div className="card-title">
        <span>? Loom 提问</span>
        {q.allow_multiple && <span className="multi">（可多选）</span>}
      </div>
      <div className="q-text">{q.text || ''}</div>
      {(q.options || []).map((opt) => (
        <label className="opt" key={opt.label}>
          <input
            type={inputType}
            name={name}
            value={opt.label}
            checked={selected.includes(opt.label)}
            onChange={() => toggle(opt.label)}
            disabled={resolving}
          />
          <span>
            {opt.label}
            {opt.description && <span className="desc">{' — ' + opt.description}</span>}
          </span>
        </label>
      ))}
      <input
        type="text"
        placeholder="自定义回答…（可选）"
        value={custom}
        disabled={resolving}
        onChange={(e) => setCustom(e.target.value)}
      />
      <div className="actions">
        <button
          type="button"
          className="btn btn-primary"
          disabled={resolving}
          onClick={() => onAnswer({ selected, custom_text: custom.trim(), skipped: false })}
        >
          提交
        </button>
        <button
          type="button"
          className="btn btn-secondary"
          disabled={resolving}
          onClick={() => onAnswer({ selected: [], custom_text: '', skipped: true })}
        >
          跳过
        </button>
      </div>
    </div>
  )
})
