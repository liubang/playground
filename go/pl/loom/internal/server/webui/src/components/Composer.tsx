// Composer.tsx — 输入框（IME 安全、自适应高度、steer 态、取消、图片附件）
// + 模型/reasoning 切换器。
//
// 图片附件三条入口：剪贴板粘贴（paste）、拖入 composer（drop）、附件按钮
// 选择文件。图片在客户端经 canvas 压缩（最长边 MAX_DIM）后以 base64 随
// prompt 一并提交；GIF/小图保留原始字节。与旧 components/composer.js 对应。

import { useCallback, useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { Icon } from '../lib/icons'
import { CtxGauge } from './CtxGauge'
import { PlanPanel } from './blocks/PlanPanel'
import { toast } from './ui/Toast'

// MAX_ATTACHMENTS 与后端 maxImageAttachments 对齐。
const MAX_ATTACHMENTS = 5
// 单张原图上限（压缩前），与 TUI 的 maxImageLoadBytes 一致。
const MAX_SOURCE_BYTES = 20 * 1024 * 1024
// 导出最长边：视觉模型推荐的分辨率上限，同时把 base64 体积压在 body 限制内。
const MAX_DIM = 1568

// blobToBase64 读取 Blob 为不含 "data:" 前缀的 base64 字符串。
function blobToBase64(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const r = new FileReader()
    r.onload = () => {
      const s = String(r.result)
      resolve(s.slice(s.indexOf(',') + 1))
    }
    r.onerror = () => reject(r.error || new Error('read failed'))
    r.readAsDataURL(blob)
  })
}

interface Attachment {
  name: string
  mediaType: string
  data: string // base64（无 data: 前缀）
  previewUrl: string
}

const REASONING_OPTIONS = [
  { value: 'default', label: '默认（跟随模型）' },
  { value: 'off', label: 'Off' },
  { value: 'low', label: 'Low' },
  { value: 'medium', label: 'Medium' },
  { value: 'high', label: 'High' },
]

function fmtCtx(n: number): string {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(0) + 'M'
  if (n >= 1000) return (n / 1000).toFixed(0) + 'k'
  return String(n)
}

export function Composer({ controller }: { controller: AppController }) {
  const busy = useStore(controller.store, (s) => s.busy)
  const locked = useStore(controller.store, (s) => !!(s.readOnly || s.archived))
  const readOnlyLabel = useStore(controller.store, (s) => s.readOnlyLabel)
  const imagesEnabled = useStore(controller.store, (s) => s.imagesEnabled)
  const imagesDisabledReason = useStore(controller.store, (s) => s.imagesDisabledReason)
  const curModelRef = useStore(controller.store, (s) => s.curModelRef)
  const curReasoning = useStore(controller.store, (s) => s.curReasoning)
  const reasoningOverridden = useStore(controller.store, (s) => s.reasoningOverridden)
  const models = useStore(controller.store, (s) => s.models)
  const plan = useStore(controller.store, (s) => s.plan)

  const [text, setText] = useState('')
  const [attachments, setAttachments] = useState<Attachment[]>([])
  const [focused, setFocused] = useState(false)
  const [picker, setPicker] = useState<'' | 'model' | 'reasoning'>('')
  const taRef = useRef<HTMLTextAreaElement>(null)
  const boxRef = useRef<HTMLDivElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)
  const composingRef = useRef(false)
  const [dragover, setDragover] = useState(false)

  // 草稿同步：controller 侧按会话暂存/还原
  useEffect(() => {
    controller.composerText = text
  }, [text, controller])
  useEffect(() => {
    controller.onComposerRestore = (t: string) => {
      setText(t)
      if (!t) {
        // 清空时同时释放附件（提交成功路径）
        for (const a of attachmentsRef.current) URL.revokeObjectURL(a.previewUrl)
        attachmentsRef.current = []
        setAttachments([])
      }
    }
    return () => {
      controller.onComposerRestore = null
    }
  }, [controller])

  // 自适应高度
  useEffect(() => {
    const ta = taRef.current
    if (!ta) return
    ta.style.height = 'auto'
    ta.style.height = Math.min(200, ta.scrollHeight) + 'px'
  }, [text])

  const onError = useCallback((msg: string) => toast(msg), [])
  const attachmentsRef = useRef<Attachment[]>([])
  attachmentsRef.current = attachments

  const addFiles = useCallback(
    async (files: File[]) => {
      if (!imagesEnabled) {
        if (files.length) onError(imagesDisabledReason || '当前模型不支持图片输入')
        return
      }
      for (const f of files) {
        if (attachmentsRef.current.length >= MAX_ATTACHMENTS) {
          onError(`最多附带 ${MAX_ATTACHMENTS} 张图片`)
          return
        }
        if (f.size > MAX_SOURCE_BYTES) {
          onError(`图片「${f.name || '剪贴板'}」超过 ${MAX_SOURCE_BYTES / 1024 / 1024}MB 上限`)
          continue
        }
        try {
          const bitmap = await createImageBitmap(f)
          const scale = Math.min(1, MAX_DIM / Math.max(bitmap.width, bitmap.height))
          let blob: Blob = f
          let mediaType = f.type || 'image/png'
          if (scale < 1) {
            const canvas = document.createElement('canvas')
            canvas.width = Math.round(bitmap.width * scale)
            canvas.height = Math.round(bitmap.height * scale)
            canvas.getContext('2d')!.drawImage(bitmap, 0, 0, canvas.width, canvas.height)
            const keepPng =
              mediaType === 'image/png' || mediaType === 'image/webp' || mediaType === 'image/gif'
            mediaType = keepPng ? 'image/png' : 'image/jpeg'
            blob = await new Promise<Blob>((resolve, reject) =>
              canvas.toBlob(
                (b) => (b ? resolve(b) : reject(new Error('encode failed'))),
                mediaType,
                0.85,
              ),
            )
          }
          bitmap.close?.()
          const data = await blobToBase64(blob)
          const att: Attachment = {
            name: f.name || 'clipboard',
            mediaType,
            data,
            previewUrl: URL.createObjectURL(blob),
          }
          attachmentsRef.current = [...attachmentsRef.current, att]
          setAttachments(attachmentsRef.current)
        } catch (err) {
          onError('图片读取失败: ' + (err as Error).message)
        }
      }
    },
    [imagesEnabled, imagesDisabledReason, onError],
  )

  // followup=true：排入下一轮队列（turn 结束后接力为下一轮的 prompt），
  // 而不是注入当前 turn。
  const submit = (followup = false) => {
    if (locked) return // 只读会话：不允许追问
    const t = text.trim()
    if (!t && attachments.length === 0) return
    void controller.submitPrompt(
      t,
      attachments.map((a) => ({ media_type: a.mediaType, data: a.data })),
      followup,
    )
  }

  // placeholder 随焦点切换：聚焦时显示按键提示，失焦时显示引导文案。
  let placeholder: string
  if (locked) {
    placeholder = readOnlyLabel || '只读'
  } else if (focused) {
    placeholder = busy
      ? 'Enter to steer the running turn · Ctrl+Enter to queue for the next turn · Shift+Enter for newline'
      : 'Enter to send · Shift+Enter for newline · paste/drop images'
  } else {
    placeholder = busy ? 'Steer this turn… (Ctrl+Enter queues for next turn)' : 'Message loom…'
  }

  const reasoningLabel = (() => {
    const e = curReasoning || 'default'
    const map: Record<string, string> = {
      default: 'reasoning',
      off: 'reasoning: off',
      low: 'reasoning: low',
      medium: 'reasoning: medium',
      high: 'reasoning: high',
    }
    return map[e] || 'reasoning'
  })()

  return (
    <div className="composer">
      <PlanPanel plan={plan} />
      <div
        className={'composer-box' + (dragover ? ' is-dragover' : '')}
        id="composer-box"
        ref={boxRef}
        onDragOver={(e) => {
          if (locked) return
          if ([...(e.dataTransfer?.types || [])].includes('Files')) {
            e.preventDefault()
            setDragover(true)
          }
        }}
        onDragLeave={(e) => {
          if (!boxRef.current?.contains(e.relatedTarget as Node)) setDragover(false)
        }}
        onDrop={(e) => {
          setDragover(false)
          if (locked) return
          const files = [...(e.dataTransfer?.files || [])].filter((f) =>
            f.type.startsWith('image/'),
          )
          if (files.length === 0) return
          e.preventDefault()
          void addFiles(files)
        }}
      >
        <div id="attach-strip" className="attach-strip" hidden={attachments.length === 0}>
          {attachments.map((att, i) => (
            <div className="attach-item" key={att.previewUrl}>
              <img src={att.previewUrl} alt={att.name} />
              <button
                type="button"
                className="attach-remove"
                title="移除"
                onClick={() => {
                  URL.revokeObjectURL(att.previewUrl)
                  attachmentsRef.current = attachmentsRef.current.filter((_, j) => j !== i)
                  setAttachments(attachmentsRef.current)
                }}
              >
                <Icon name="xmark" />
              </button>
            </div>
          ))}
        </div>
        <textarea
          id="composer-input"
          ref={taRef}
          rows={1}
          placeholder={placeholder}
          value={text}
          disabled={locked}
          onChange={(e) => setText(e.target.value)}
          onCompositionStart={() => {
            composingRef.current = true
          }}
          onCompositionEnd={() => {
            composingRef.current = false
          }}
          onKeyDown={(e) => {
            if (
              e.key === 'Enter' &&
              !e.shiftKey &&
              !composingRef.current &&
              !e.nativeEvent.isComposing
            ) {
              e.preventDefault()
              // Ctrl/Cmd+Enter：排队到下一轮；普通 Enter：steer 当前轮 / 直接发送
              submit(e.ctrlKey || e.metaKey)
            }
          }}
          onPaste={(e) => {
            if (locked) return
            const files: File[] = []
            for (const item of e.clipboardData?.items || []) {
              if (item.kind === 'file' && item.type.startsWith('image/')) {
                const f = item.getAsFile()
                if (f) files.push(f)
              }
            }
            if (files.length === 0) return // 纯文本粘贴走默认行为
            const hasText = (e.clipboardData.getData('text/plain') || '').length > 0
            if (!hasText) e.preventDefault() // 纯图片：阻止插入文件名噪声
            void addFiles(files)
          }}
          onFocus={() => setFocused(true)}
          onBlur={() => setFocused(false)}
        />
        <div className="composer-bar">
          <div className="composer-tools">
            <button
              id="attach-btn"
              className="icon-btn"
              title={
                imagesEnabled
                  ? '添加图片（支持直接粘贴 / 拖拽）'
                  : imagesDisabledReason || '当前模型不支持图片输入'
              }
              disabled={locked || !imagesEnabled}
              onClick={() => fileInputRef.current?.click()}
            >
              <Icon name="image" />
            </button>
            <input
              ref={fileInputRef}
              id="attach-input"
              type="file"
              accept="image/png,image/jpeg,image/gif,image/webp"
              multiple
              hidden
              onChange={(e) => {
                void addFiles([...(e.target.files || [])])
                e.target.value = '' // 允许重复选择同一文件
              }}
            />
            <button
              id="model-btn"
              className={'picker-btn' + (picker === 'model' ? ' is-active' : '')}
              title="切换模型"
              disabled={locked}
              onClick={() => setPicker(picker === 'model' ? '' : 'model')}
            >
              <span className="picker-label">{controller.modelLabel(curModelRef)}</span>
              <span className="picker-caret">
                <Icon name="caret-down" />
              </span>
            </button>
            <button
              id="reasoning-btn"
              className={'picker-btn' + (picker === 'reasoning' ? ' is-active' : '')}
              title="设置 reasoning"
              disabled={locked}
              onClick={() => setPicker(picker === 'reasoning' ? '' : 'reasoning')}
            >
              <span className="picker-label">
                {reasoningLabel}
                {reasoningOverridden && curReasoning && curReasoning !== 'default' && (
                  <Icon name="star" />
                )}
              </span>
              <span className="picker-caret">
                <Icon name="caret-down" />
              </span>
            </button>
          </div>
          <div className="composer-actions">
            <CtxGauge controller={controller} />
            <button
              id="cancel-btn"
              className="icon-btn-circle btn-danger"
              title="cancel turn"
              hidden={!busy}
              onClick={controller.cancelTurn}
            >
              <Icon name="stop" />
            </button>
            <button
              id="send-btn"
              className="send-btn"
              title="发送 (Enter)"
              disabled={locked}
              onClick={() => submit()}
            >
              <Icon name="arrow-up" />
            </button>
          </div>
        </div>
      </div>
      {picker === 'model' && (
        <PickerMenu anchorId="model-btn" onClose={() => setPicker('')}>
          <ModelMenu
            models={models}
            currentRef={curModelRef}
            onPick={(ref) => {
              setPicker('')
              if (ref !== curModelRef) void controller.pickModel(ref)
            }}
          />
        </PickerMenu>
      )}
      {picker === 'reasoning' && (
        <PickerMenu anchorId="reasoning-btn" onClose={() => setPicker('')}>
          <ReasoningMenu
            current={curReasoning}
            onPick={(effort) => {
              setPicker('')
              if (effort !== (curReasoning || 'default')) void controller.pickReasoning(effort)
            }}
          />
        </PickerMenu>
      )}
    </div>
  )
}

// PickerMenu — 模型/reasoning 共用浮层：锚定到触发按钮上方（composer 在
// 视口底部，向下会被裁剪）；外部点击 / Esc / 窗口失焦关闭。
function PickerMenu({
  anchorId,
  onClose,
  children,
}: {
  anchorId: string
  onClose: () => void
  children: React.ReactNode
}) {
  const popRef = useRef<HTMLDivElement>(null)
  const [style, setStyle] = useState<React.CSSProperties>({})

  useEffect(() => {
    const anchor = document.getElementById(anchorId)
    const pop = popRef.current
    if (anchor && pop) {
      const r = anchor.getBoundingClientRect()
      setStyle({
        left: Math.max(8, Math.min(r.left, innerWidth - pop.offsetWidth - 8)) + 'px',
        bottom: innerHeight - r.top + 6 + 'px',
      })
    }
    const onDocClick = (e: MouseEvent) => {
      const t = e.target as Node
      if (popRef.current?.contains(t)) return
      const a = document.getElementById(anchorId)
      if (a && (a === t || a.contains(t))) return
      onClose()
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
    }
    const onBlur = () => onClose()
    document.addEventListener('click', onDocClick)
    document.addEventListener('keydown', onKey)
    window.addEventListener('blur', onBlur)
    return () => {
      document.removeEventListener('click', onDocClick)
      document.removeEventListener('keydown', onKey)
      window.removeEventListener('blur', onBlur)
    }
  }, [anchorId, onClose])

  return createPortal(
    <div id="menu" className="menu" ref={popRef} style={style}>
      {children}
    </div>,
    document.body,
  )
}

function ModelMenu({
  models,
  currentRef,
  onPick,
}: {
  models: { provider: string; name: string; context_window?: number; modalities?: string[] }[]
  currentRef: string
  onPick: (ref: string) => void
}) {
  const groups = new Map<string, typeof models>()
  for (const mo of models) {
    if (!groups.has(mo.provider)) groups.set(mo.provider, [])
    groups.get(mo.provider)!.push(mo)
  }
  return (
    <>
      {[...groups.entries()].map(([provider, list]) => (
        <div key={provider}>
          <div className="menu-group">{provider}</div>
          {list.map((mo) => {
            const ref = provider + '/' + mo.name
            return (
              <button
                key={ref}
                type="button"
                className={'menu-item' + (ref === currentRef ? ' is-active' : '')}
                onClick={() => onPick(ref)}
              >
                {mo.name}
                {(mo.modalities || []).includes('image') && (
                  <span className="mod" title="支持图片输入">
                    <Icon name="image" />
                  </span>
                )}
                {mo.context_window ? (
                  <span className="ctx">{fmtCtx(mo.context_window)}</span>
                ) : null}
                {/* ✓ 槽位始终占位（未选中留空），保持所有行右侧对齐 */}
                <span className="check">{ref === currentRef && <Icon name="check" />}</span>
              </button>
            )
          })}
        </div>
      ))}
    </>
  )
}

function ReasoningMenu({ current, onPick }: { current: string; onPick: (effort: string) => void }) {
  const eff = current || 'default'
  return (
    <>
      {REASONING_OPTIONS.map((opt) => (
        <button
          key={opt.value}
          type="button"
          className={'menu-item' + (opt.value === eff ? ' is-active' : '')}
          onClick={() => onPick(opt.value)}
        >
          {opt.label}
          <span className="check">{opt.value === eff && <Icon name="check" />}</span>
        </button>
      ))}
    </>
  )
}
