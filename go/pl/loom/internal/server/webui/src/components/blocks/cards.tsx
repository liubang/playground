// cards.tsx — 审批卡片与问答卡片（与旧 blocks.js approvalCard/questionCard 对应）。

import { memo, useState } from 'react'
import type { ApprovalRequestedPayload, QuestionPayload } from '../../protocol/events'
import { Icon } from '../../lib/icons'
import { DiffView } from './DiffView'

// --- approval 卡片 ---
// diff（可选）：调用方从工具块移入的 diff 文本（去重：审批期间工具块不
// 再重复展示，收编时恢复）。

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
  // rule_preview 为空表示该调用不可记忆（后端 ApprovalRulePreview），
  // 此时隐藏 Allow always，避免提供一个静默无效的选项。
  const preview = payload.rule_preview || ''
  const trustPreview = payload.trust_preview || ''
  return (
    <div className="block card-approval">
      <div className="card-title">
        <span className="card-title-label">
          <Icon name="circle-question" /> Approval required
        </span>
        <span className="risk">{'R' + (payload.risk ?? '?')}</span>
        <span className="mono">{payload.tool_name || ''}</span>
      </div>
      {payload.description && <div className="desc">{payload.description}</div>}
      {/* 后果行：这个操作「会做什么」（推导出的效果），而非命令文本本身 */}
      {payload.consequence && <div className="consequence">{payload.consequence}</div>}
      {/* cmd 仅在 target 与 description 不同时展示，避免同一段话渲染两遍 */}
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
          Allow
        </button>
        {preview && (
          <button
            type="button"
            className="btn btn-secondary"
            disabled={resolving}
            onClick={() => onResolve('allow', true)}
          >
            Allow always
          </button>
        )}
        {trustPreview && (
          <button
            type="button"
            className="btn btn-danger"
            disabled={resolving}
            onClick={() => onResolve('allow', true, 'unsandboxed')}
          >
            Trust (no sandbox)
          </button>
        )}
        <button
          type="button"
          className="btn btn-danger"
          disabled={resolving}
          onClick={() => onResolve('deny', false)}
        >
          Deny
        </button>
        {preview && (
          <span className="memo">{`allow always remembers "${preview}" for the workspace`}</span>
        )}
        {trustPreview && (
          <span className="memo">{`trust remembers "${trustPreview}" with FULL user privileges`}</span>
        )}
      </div>
    </div>
  )
})

// --- question 卡片 ---

export const QuestionCard = memo(function QuestionCard({
  payload,
  resolving,
  onAnswer,
}: {
  payload: QuestionPayload
  resolving?: boolean
  onAnswer: (answer: { selected: string[]; custom_text: string; skipped: boolean }) => void
}) {
  // snapshot 重建时 payload 为 PendingRequest.Question（包一层 question）
  const q = (payload as { question?: QuestionPayload }).question || payload
  const inputType = q.allow_multiple ? 'checkbox' : 'radio'
  const [name] = useState(() => 'q_' + Math.random().toString(36).slice(2, 8))
  const [custom, setCustom] = useState('')
  // 受控选择状态：此前用 querySelectorAll(':checked') 直接读 DOM，组件被
  // React 复用 / payload 变更 / resolving 切换重渲时，DOM 的 checked 与
  // 组件意图可能脱节。统一收进 state。
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
        <span>? Loom asks</span>
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
        placeholder="custom answer… (optional)"
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
          Submit
        </button>
        <button
          type="button"
          className="btn btn-secondary"
          disabled={resolving}
          onClick={() => onAnswer({ selected: [], custom_text: '', skipped: true })}
        >
          Skip
        </button>
      </div>
    </div>
  )
})
