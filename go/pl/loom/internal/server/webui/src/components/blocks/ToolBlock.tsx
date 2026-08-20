// ToolBlock.tsx — 工具块渲染器（与旧 blocks.js toolBlock 一一对应）。
// 实时事件的 preview 是有界摘要，完整内容经 fetchToolOutput 按需取；
// snapshot 重建路径有 full_text，用不到。

import { memo, useState } from 'react'
import type { ToolCompletion } from '../../app/transcript'
import { fmtDuration, copyText } from '../../lib/format'
import { Icon } from '../../lib/icons'
import { DiffView } from './DiffView'
import { ArtifactBlock, InlineImage } from './images'

// st → [图标, 文案]；注意 className 用 err（CSS 类）而 status 文本用 error
const TOOL_STATUS: Record<string, ['check' | 'xmark' | 'ban', string]> = {
  ok: ['check', 'ok'],
  err: ['xmark', 'error'],
  canceled: ['ban', 'canceled'],
}

export interface ToolBlockProps {
  toolName: string
  target?: string
  diff?: string
  diffSuppressed?: boolean // 审批期间 diff 移入审批卡片展示
  completion?: ToolCompletion
  // fetchToolOutput: 复制完整输出用（实时路径 preview 有界）
  fetchToolOutput?: () => Promise<string>
}

export const ToolBlock = memo(function ToolBlock({
  toolName,
  target,
  diff,
  diffSuppressed,
  completion,
  fetchToolOutput,
}: ToolBlockProps) {
  const [targetExpanded, setTargetExpanded] = useState(false)

  let statusEl
  if (!completion) {
    statusEl = <span className="tool-status running">running</span>
  } else {
    const st =
      completion.status === 'success' ? 'ok' : completion.status === 'error' ? 'err' : 'canceled'
    const meta = TOOL_STATUS[st]
    statusEl = (
      <span className={'tool-status ' + st}>
        {meta ? (
          <>
            <Icon name={meta[0]} /> {meta[1]}
          </>
        ) : (
          completion.status || 'done'
        )}
      </span>
    )
  }

  const getFullText = async (): Promise<string> => {
    if (completion?.full_text) return completion.full_text
    if (fetchToolOutput) return fetchToolOutput()
    return completion?.preview || '' // server 取不到时兜底复制摘要
  }

  return (
    <div className="block block-tool">
      <div className="tool-head">
        <span className="tool-name mono">{toolName || 'tool'}</span>
        {target && (
          // CSS 省略号截断后，原生 tooltip 与点击展开（换行显示）都能看完整内容。
          <span
            className={'tool-target mono' + (targetExpanded ? ' expanded' : '')}
            title={target}
            onClick={(e) => {
              e.stopPropagation()
              setTargetExpanded((v) => !v)
            }}
          >
            {target}
          </span>
        )}
        {statusEl}
        <span className="tool-dur mono">
          {completion?.duration_ms != null ? fmtDuration(completion.duration_ms) : ''}
        </span>
      </div>
      {completion && (completion.error_message || completion.error) && (
        <div className="tool-error">{completion.error_message || completion.error}</div>
      )}
      {completion?.preview && <ToolOutput preview={completion.preview} getFullText={getFullText} />}
      {/* 渲染工具结果中的图片：内联 base64 优先（同步可用、无需鉴权二次
          请求），仅当没有内联图片时才走 artifact 路径。artifact 不一定是
          图片（run_cmd 的 stdout artifact 是文本），由 ArtifactBlock 按
          媒体类型分发渲染。 */}
      {completion &&
        ((completion.images || []).length > 0
          ? (completion.images || []).map((img, i) => (
              <InlineImage key={i} mediaType={img.media_type} data={img.data} />
            ))
          : (completion.artifacts || []).map((art) => (
              <ArtifactBlock key={art.id} artifact={art} />
            )))}
      {diff && !diffSuppressed && <DiffView diffText={diff} />}
    </div>
  )
})

// 工具输出区：默认折叠，展开显示有界 preview（截断以 "\n…" 结尾标记）；
// copy 按钮始终复制完整输出。
function ToolOutput({
  preview,
  getFullText,
}: {
  preview: string
  getFullText: () => Promise<string>
}) {
  const [label, setLabel] = useState<'copy' | 'copied' | 'copy failed'>('copy')
  const truncated = preview.endsWith('\n…')
  return (
    <details className="tool-output disclosure">
      <summary>
        <span className="tool-output-label">
          {`output · ${preview.length} chars${truncated ? ' · truncated' : ''}`}
        </span>
        <button
          type="button"
          className="tool-copy"
          title="复制完整输出"
          onClick={async (e) => {
            e.preventDefault() // 不触发 details 展开/收起
            e.stopPropagation()
            try {
              const text = await getFullText()
              if (!(await copyText(text))) throw new Error('clipboard unavailable')
              setLabel('copied')
            } catch {
              setLabel('copy failed')
            }
            setTimeout(() => setLabel('copy'), 1500)
          }}
        >
          {label === 'copied' ? (
            <>
              <Icon name="check" /> copied
            </>
          ) : (
            label
          )}
        </button>
      </summary>
      <div className="tool-preview mono">{preview}</div>
    </details>
  )
}
