// TranscriptView.tsx — 消息流视图：订阅 TranscriptController 的块模型渲染。
// 滚动跟随：流式 delta 遵循 following（用户上翻阅读时不打扰）；新块/卡片
// 强制回底（controller 侧 forceFollow 语义）。块组件按引用 memo——controller
// 对未变更的块保持对象引用，重渲只发生在版本号变化的块上。

import { memo, useLayoutEffect, useRef, type ReactNode } from 'react'
import type { BlockModel, TranscriptController } from '../app/transcript'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import {
  AssistantBlock,
  CompactBlock,
  FatalBlock,
  InterruptedBlock,
  NoticeBlock,
  ReasoningBlock,
  ResolvedNotice,
  StreamBlock,
  ThinkingBlock,
  UserBlock,
} from './blocks/blocks'
import { ToolBlock } from './blocks/ToolBlock'
import { ApprovalCard, QuestionCard } from './blocks/cards'
import { ArtifactBlock, InlineImage } from './blocks/images'

const FOLLOW_THRESHOLD_PX = 80

// 视图层回调集合（App 注入；分享页只传 fetchToolOutput 的子集——
// 审批/问答/反馈不出现，传 undefined 即不渲染对应交互）
export interface TranscriptViewIO {
  onResolveApproval?: (approvalId: string, decision: 'allow' | 'deny', always: boolean) => void
  onAnswerQuestion?: (
    questionId: string,
    answer: { selected: string[]; custom_text: string; skipped: boolean },
  ) => void
  onFeedback?: (runId: string, value: 0 | 1) => Promise<unknown>
  fetchToolOutput?: (callId: string) => Promise<string>
}

const BlockView = memo(
  function BlockView({ block, io }: { block: BlockModel; io: TranscriptViewIO }) {
    switch (block.kind) {
      case 'user':
        return <UserBlock text={block.text} createdAt={block.createdAt} images={block.images} />
      case 'assistant':
        return (
          <AssistantBlock text={block.text} actions={block.actions} onFeedback={io.onFeedback} />
        )
      case 'stream':
        return <StreamBlock text={block.text} />
      case 'reasoning':
        return <ReasoningBlock text={block.text} durationMs={block.durationMs} />
      case 'thinking':
        return <ThinkingBlock />
      case 'tool':
        return (
          <ToolBlock
            callId={block.callId}
            toolName={block.toolName}
            target={block.target}
            diff={block.diff}
            diffSuppressed={block.diffSuppressed}
            completion={block.completion}
            fetchToolOutput={
              io.fetchToolOutput && block.callId
                ? () => io.fetchToolOutput!(block.callId!)
                : undefined
            }
          />
        )
      case 'approval':
        return (
          <ApprovalCard
            payload={block.payload}
            diff={block.diff}
            resolving={block.resolving}
            onResolve={(decision, always) =>
              io.onResolveApproval?.(block.payload.approval_id || '', decision, always)
            }
          />
        )
      case 'question': {
        const qid = block.payload.question_id || block.payload.id || ''
        return (
          <QuestionCard
            payload={block.payload}
            resolving={block.resolving}
            onAnswer={(answer) => io.onAnswerQuestion?.(qid, answer)}
          />
        )
      }
      case 'notice':
        return <NoticeBlock text={block.text} warn={block.warn} />
      case 'resolved':
        return <ResolvedNotice ok={block.ok} actor={block.actor} what={block.what} />
      case 'fatal':
        return <FatalBlock text={block.text} />
      case 'interrupted':
        return <InterruptedBlock text={block.text} />
      case 'compact':
        return <CompactBlock payload={block.payload} />
      case 'image':
        return (
          <div className="block block-image">
            <InlineImage mediaType={block.mediaType} data={block.data} />
          </div>
        )
      case 'artifact':
        return <ArtifactBlock artifact={block.artifact} />
      default:
        return null
    }
  },
  (prev, next) => prev.block === next.block && prev.io === next.io,
)

export interface EmptyState {
  hidden: boolean
  hint: string
  showAddWs: boolean
  onAddWs: () => void
}

export function TranscriptView({
  controller,
  io,
  empty,
  className,
  scrollerOut,
  children,
}: {
  controller: TranscriptController
  io: TranscriptViewIO
  empty?: EmptyState
  className?: string
  // scrollerOut：调用方持有滚动容器引用（resync 保留滚动位置用）
  scrollerOut?: { el: HTMLDivElement | null }
  children?: ReactNode
}) {
  const blocks = useStore(controller.store, (s) => s.blocks)
  const following = useStore(controller.store, (s) => s.following)
  const followSeq = useStore(controller.store, (s) => s.followSeq)
  const scrollerRef = useRef<HTMLDivElement>(null)

  useLayoutEffect(() => {
    if (!scrollerOut) return
    scrollerOut.el = scrollerRef.current
    return () => {
      scrollerOut.el = null
    }
  }, [scrollerOut])

  // 回底请求：controller 挂的 rAF 回调递增 followSeq，此处消费
  useLayoutEffect(() => {
    if (followSeq === 0) return
    const el = scrollerRef.current
    if (el) el.scrollTop = el.scrollHeight
  }, [followSeq])

  // resync 保留滚动：AppController 在 applySnapshot 前写入 preserveScrollTop，
  // DOM 重建后在此恢复（内容同源，scrollTop 近似有效）
  useLayoutEffect(() => {
    const t = controller.store.get().preserveScrollTop
    if (t != null && scrollerRef.current) {
      scrollerRef.current.scrollTop = t
      controller.store.update((s) => {
        s.preserveScrollTop = null
      })
    }
  }, [blocks, controller])

  return (
    <div
      id="transcript"
      className={className ? 'transcript ' + className : 'transcript'}
      ref={scrollerRef}
      onScroll={() => {
        const el = scrollerRef.current
        if (!el) return
        const gap = el.scrollHeight - el.scrollTop - el.clientHeight
        controller.setFollowing(gap < FOLLOW_THRESHOLD_PX)
      }}
    >
      <div id="blocks" className="transcript-inner">
        {blocks.map((b) => (
          <BlockView key={b.id} block={b} io={io} />
        ))}
      </div>
      {children}
      {empty && (
        <div id="empty-state" className="empty-state" hidden={empty.hidden}>
          <div className="brand">◆ loom</div>
          <p id="empty-hint">{empty.hint}</p>
          <p>
            <button
              id="empty-add-ws"
              className="btn"
              type="button"
              hidden={!empty.showAddWs}
              onClick={empty.onAddWs}
            >
              Add workspace&hellip;
            </button>
          </p>
        </div>
      )}
      <button
        id="follow-btn"
        className="follow-btn"
        hidden={following}
        onClick={() => controller.followNow()}
      >
        <Icon name="arrow-down" /> back to bottom
      </button>
    </div>
  )
}
