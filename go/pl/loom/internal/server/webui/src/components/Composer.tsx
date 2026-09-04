// Composer.tsx — input box (IME-safe, auto-growing height, steer state, cancel,
// image attachments) + model/reasoning pickers.
//
// Three image-attachment entries: clipboard paste, drop onto the composer, and
// the attach button. Images are canvas-compressed client-side (longest edge
// MAX_DIM) and submitted as base64 with the prompt; GIFs/small images keep their
// original bytes. Mirrors the old components/composer.js.

import { useCallback, useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import type { AppController } from '../app/controller'
import { useStore } from '../store/store'
import { Icon, type IconName } from '../lib/icons'
import { CtxGauge } from './CtxGauge'
import { PlanPanel } from './blocks/PlanPanel'
import { toast } from './ui/Toast'

// MAX_ATTACHMENTS matches the backend maxImageAttachments.
const MAX_ATTACHMENTS = 5
// Per-image source size cap (pre-compression); same as the TUI's maxImageLoadBytes.
const MAX_SOURCE_BYTES = 20 * 1024 * 1024
// Exported longest edge: the resolution cap recommended for vision models; also
// keeps the base64 payload within the body size limit.
const MAX_DIM = 1568

// blobToBase64 reads a Blob into a base64 string without the "data:" prefix.
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
  data: string // base64 (no data: prefix)
  previewUrl: string
}

const REASONING_OPTIONS = [
  { value: 'default', label: '默认（跟随模型）' },
  { value: 'off', label: 'Off' },
  { value: 'low', label: 'Low' },
  { value: 'medium', label: 'Medium' },
  { value: 'high', label: 'High' },
]

// Quick toggle for the approval baseline (same source as the settings panel's
// approval.mode; this one is a workspace-level override that takes effect next
// turn and is never persisted). Capsule: the default mode (standard) shows only
// the shield icon — the normal state demands no attention; non-default modes
// expand a short name with an amber warning.
const APPROVAL_OPTIONS = [
  { value: 'on-request', short: 'standard', hint: '默认：工作区内读写免审批，越界/危险才询问' },
  {
    value: 'danger-only',
    short: 'dev',
    hint: '开发模式：仅危险命令/危险站点弹审批，开发命令与正常访问自动放行',
  },
  { value: 'never', short: 'auto', hint: '无人值守：危险直接拒绝，永不等待审批' },
]

function fmtCtx(n: number): string {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(0) + 'M'
  if (n >= 1000) return (n / 1000).toFixed(0) + 'k'
  return String(n)
}

// AcItem is one autocomplete candidate: icon + main line + sub line + the text
// inserted on accept.
interface AcItem {
  icon: IconName
  main: string
  sub: string
  insert: string
}

export function Composer({ controller }: { controller: AppController }) {
  const busy = useStore(controller.store, (s) => s.busy)
  const locked = useStore(controller.store, (s) => !!(s.readOnly || s.archived))
  const readOnlyLabel = useStore(controller.store, (s) => s.readOnlyLabel)
  const imagesEnabled = useStore(controller.store, (s) => s.imagesEnabled)
  const imagesDisabledReason = useStore(controller.store, (s) => s.imagesDisabledReason)
  const curModelRef = useStore(controller.store, (s) => s.curModelRef)
  const curReasoning = useStore(controller.store, (s) => s.curReasoning)
  const approvalMode = useStore(controller.store, (s) => s.approvalMode)
  const models = useStore(controller.store, (s) => s.models)
  const plan = useStore(controller.store, (s) => s.plan)

  const [text, setText] = useState('')
  const [attachments, setAttachments] = useState<Attachment[]>([])
  const [focused, setFocused] = useState(false)
  const [picker, setPicker] = useState<'' | 'model' | 'reasoning' | 'approval'>('')
  const taRef = useRef<HTMLTextAreaElement>(null)
  const boxRef = useRef<HTMLDivElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)
  const composingRef = useRef(false)
  const [dragover, setDragover] = useState(false)

  // --- Autocomplete: @ file refs (workspace fuzzy search) and / skills (line start only) ---
  const [ac, setAc] = useState<null | { kind: 'file' | 'skill'; start: number; query: string }>(
    null,
  )
  const [acItems, setAcItems] = useState<AcItem[]>([])
  const [acSel, setAcSel] = useState(0)
  const skillsCache = useRef<AcItem[] | null>(null)
  const acTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const acSeq = useRef(0)

  // Draft sync: the controller stashes/restores text per session
  useEffect(() => {
    controller.composerText = text
  }, [text, controller])
  useEffect(() => {
    controller.onComposerRestore = (t: string) => {
      // 会话切换前，先把当前草稿带的附件预览 URL 全部 revoke（否则变成
      // 孤儿 blob URL 直到页面卸载才回收）。
      for (const a of attachmentsRef.current) URL.revokeObjectURL(a.previewUrl)
      attachmentsRef.current = []
      setAttachments([])
      setText(t)
    }
    return () => {
      controller.onComposerRestore = null
    }
  }, [controller])
  // 卸载时清掉挂起的 autocomplete 定时器，避免对已卸载组件 setState。
  useEffect(() => {
    return () => {
      if (acTimer.current) clearTimeout(acTimer.current)
    }
  }, [])

  // Auto-growing height
  useEffect(() => {
    const ta = taRef.current
    if (!ta) return
    ta.style.height = 'auto'
    ta.style.height = Math.min(200, ta.scrollHeight) + 'px'
  }, [text])

  // Trigger detection: @ must follow line start/whitespace (avoids emails); / only at input start
  useEffect(() => {
    const ta = taRef.current
    if (!ta || locked) {
      setAc(null)
      return
    }
    const upto = text.slice(0, ta.selectionStart ?? text.length)
    const at = /(?:^|\s)@([^\s@]*)$/.exec(upto)
    if (at) {
      setAc({ kind: 'file', start: upto.length - at[1].length - 1, query: at[1] })
      return
    }
    const slash = /^\/([^\s/]*)$/.exec(upto)
    if (slash) {
      setAc({ kind: 'skill', start: 0, query: slash[1] })
      return
    }
    setAc(null)
  }, [text, locked])

  // Completion data: skill list filtered client-side (cached once); files via
  // server-side fuzzy search (debounced)
  useEffect(() => {
    setAcSel(0)
    if (!ac) {
      setAcItems([])
      return
    }
    const seq = ++acSeq.current
    if (ac.kind === 'skill') {
      const applySkills = (items: AcItem[]) => {
        const q = ac.query.toLowerCase()
        setAcItems(items.filter((i) => i.main.toLowerCase().includes(q)).slice(0, 8))
      }
      if (skillsCache.current) {
        applySkills(skillsCache.current)
        return
      }
      controller.api
        .listSkills()
        .then((res) => {
          const items: AcItem[] = []
          for (const g of res.groups || []) {
            for (const sk of g.skills || []) {
              if (sk.disabled) continue
              items.push({
                icon: 'puzzle-piece',
                main: sk.name,
                sub: sk.description || '',
                insert: '/' + sk.name + ' ',
              })
            }
          }
          skillsCache.current = items
          if (acSeq.current === seq) applySkills(items)
        })
        .catch(() => {})
      return
    }
    if (acTimer.current) clearTimeout(acTimer.current)
    acTimer.current = setTimeout(() => {
      const wsId = controller.currentWorkspaceId()
      if (!wsId) return
      controller.api
        .searchWorkspaceFiles(wsId, ac.query)
        .then((res) => {
          if (acSeq.current !== seq) return
          setAcItems(
            (res.matches || []).slice(0, 8).map((m) => ({
              icon: m.kind === 'dir' ? 'folder' : 'file',
              main: m.name,
              sub: m.path,
              insert: '@' + m.path + (m.kind === 'dir' ? '/' : ' '),
            })),
          )
        })
        .catch(() => {
          if (acSeq.current === seq) setAcItems([])
        })
    }, 150)
  }, [ac]) // eslint-disable-line react-hooks/exhaustive-deps

  // Accept completion: replace the trigger token with the insert value and move
  // the caret to its end; directories end with / so drilling can continue.
  // pos is clamped to ac.start — if the caret moved out of the token without a
  // text change, an unclamped splice would duplicate the middle segment.
  const acceptAc = (item: AcItem) => {
    if (!ac) return
    const ta = taRef.current
    const pos = Math.max(ac.start, ta ? (ta.selectionStart ?? text.length) : text.length)
    setText(text.slice(0, ac.start) + item.insert + text.slice(pos))
    setAc(null)
    const caret = ac.start + item.insert.length
    requestAnimationFrame(() => {
      const t = taRef.current
      if (t) {
        t.focus()
        t.setSelectionRange(caret, caret)
      }
    })
  }

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

  // followup=true: queue for the next turn (becomes the next turn's prompt once
  // this turn ends) instead of steering the current turn.
  const submit = (followup = false) => {
    if (locked) return // read-only session: no follow-ups allowed
    const t = text.trim()
    if (!t && attachments.length === 0) return
    void controller.submitPrompt(
      t,
      attachments.map((a) => ({ media_type: a.mediaType, data: a.data })),
      followup,
    )
  }

  // Placeholder switches with focus: shortest key hints when focused, guidance
  // copy when blurred. Full shortcut docs live in the send button tooltip — the
  // input area's most valuable real estate is no place for a manual.
  let placeholder: string
  if (locked) {
    placeholder = readOnlyLabel || '只读'
  } else if (focused) {
    placeholder = busy
      ? 'Enter 干预本轮 · Ctrl+Enter 排队下一轮 · Shift+Enter 换行'
      : 'Enter 发送 · Shift+Enter 换行'
  } else {
    placeholder = busy ? '干预本轮…' : '给 loom 发消息…'
  }

  // Reasoning capsule: default state shows the entry name; non-default shows
  // only the level value (prefix/asterisk would be noise). Non-default tint
  // (is-on) makes "not on the default level" scannable.
  const reasoningLabel = curReasoning && curReasoning !== 'default' ? curReasoning : 'reasoning'

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
            // While the completion menu is open, navigation/confirm keys take priority —
            // but never during IME composition (Enter/Arrows belong to the IME
            // candidate window, e.g. typing pinyin after @).
            if (ac && !e.nativeEvent.isComposing && !composingRef.current) {
              if (e.key === 'Escape') {
                e.preventDefault()
                setAc(null)
                return
              }
              if (acItems.length > 0) {
                if (e.key === 'ArrowDown') {
                  e.preventDefault()
                  setAcSel((v) => (v + 1) % acItems.length)
                  return
                }
                if (e.key === 'ArrowUp') {
                  e.preventDefault()
                  setAcSel((v) => (v - 1 + acItems.length) % acItems.length)
                  return
                }
                if (e.key === 'Enter' || e.key === 'Tab') {
                  e.preventDefault()
                  acceptAc(acItems[acSel])
                  return
                }
              }
            }
            if (
              e.key === 'Enter' &&
              !e.shiftKey &&
              !composingRef.current &&
              !e.nativeEvent.isComposing
            ) {
              e.preventDefault()
              // Ctrl/Cmd+Enter: queue for next turn; plain Enter: steer current turn / send
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
            if (files.length === 0) return // plain-text paste: keep default behavior
            const hasText = (e.clipboardData.getData('text/plain') || '').length > 0
            if (!hasText) e.preventDefault() // image-only: block filename noise insertion
            void addFiles(files)
          }}
          onFocus={() => setFocused(true)}
          onBlur={() => {
            setFocused(false)
            setAc(null)
          }}
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
                e.target.value = '' // allow re-selecting the same file
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
              className={
                'picker-btn' +
                (picker === 'reasoning' ? ' is-active' : '') +
                (curReasoning && curReasoning !== 'default' ? ' is-on' : '')
              }
              title={`设置 reasoning（当前：${curReasoning || 'default'}）`}
              disabled={locked}
              onClick={() => setPicker(picker === 'reasoning' ? '' : 'reasoning')}
            >
              <span className="picker-label">{reasoningLabel}</span>
              <span className="picker-caret">
                <Icon name="caret-down" />
              </span>
            </button>
            <button
              id="approval-btn"
              className={
                'picker-btn' +
                (picker === 'approval' ? ' is-active' : '') +
                (approvalMode && approvalMode !== 'on-request' ? ' is-warn' : '')
              }
              title="切换审批基线模式（工作区级，下一轮生效）"
              disabled={locked}
              onClick={() => setPicker(picker === 'approval' ? '' : 'approval')}
            >
              <Icon name="shield-halved" />
              {/* Zero text in the normal state: standard demands no attention;
                  non-default modes expand a short name + caret */}
              {approvalMode && approvalMode !== 'on-request' && (
                <>
                  <span className="picker-label">
                    {APPROVAL_OPTIONS.find((o) => o.value === approvalMode)?.short || approvalMode}
                  </span>
                  <span className="picker-caret">
                    <Icon name="caret-down" />
                  </span>
                </>
              )}
            </button>
          </div>
          <div className="composer-actions">
            <CtxGauge controller={controller} />
            {/* send/stop are one two-state button: while busy the primary button
                becomes cancel (same spot, same shape — avoids the hierarchy mess
                of a red stop + teal send side by side) */}
            <button
              id="send-btn"
              className={'send-btn' + (busy ? ' is-stop' : '')}
              title={
                busy ? '取消当前轮' : '发送（Enter）· Ctrl+Enter 排队下一轮 · Shift+Enter 换行'
              }
              disabled={locked}
              onClick={() => (busy ? controller.cancelTurn() : submit())}
            >
              <Icon name={busy ? 'stop' : 'arrow-up'} />
            </button>
          </div>
        </div>
      </div>
      {picker === 'model' && (
        <PickerMenu anchorId="model-btn" navKeys onClose={() => setPicker('')}>
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
        <PickerMenu anchorId="reasoning-btn" navKeys onClose={() => setPicker('')}>
          <ReasoningMenu
            current={curReasoning}
            onPick={(effort) => {
              setPicker('')
              if (effort !== (curReasoning || 'default')) void controller.pickReasoning(effort)
            }}
          />
        </PickerMenu>
      )}
      {picker === 'approval' && (
        <PickerMenu anchorId="approval-btn" navKeys onClose={() => setPicker('')}>
          <ApprovalMenu
            current={approvalMode || 'on-request'}
            onPick={(mode) => {
              setPicker('')
              if (mode !== (approvalMode || 'on-request')) void controller.pickApprovalMode(mode)
            }}
          />
        </PickerMenu>
      )}
      {/* Autocomplete popover: shares PickerMenu with the pickers (anchored above the composer) */}
      {ac && acItems.length > 0 && (
        <PickerMenu anchorId="composer-box" onClose={() => setAc(null)}>
          {acItems.map((it, i) => (
            <button
              key={it.insert}
              type="button"
              className={'menu-item ac-item' + (i === acSel ? ' is-active' : '')}
              onMouseDown={(e) => {
                e.preventDefault() // keep textarea focus
                acceptAc(it)
              }}
              onMouseEnter={() => setAcSel(i)}
            >
              <span className="ac-icon">
                <Icon name={it.icon} />
              </span>
              <span className="ac-main">{it.main}</span>
              <span className="ac-sub">{it.sub}</span>
            </button>
          ))}
        </PickerMenu>
      )}
    </div>
  )
}

// PickerMenu — shared popover for model/reasoning/approval: anchored above the
// trigger button (the composer sits at the viewport bottom, so opening downward
// would be clipped); closes on outside click / Esc / window blur.
// navKeys（picker 菜单打开）：↑/↓ 在菜单项间循环移动焦点，Enter/Space 原生
// 触发点击；选项超出一屏时滚动跟随。自动补全菜单（navKeys=false）的方向键
// 由 textarea 自己的处理器消费（避免双重导航）。
function PickerMenu({
  anchorId,
  onClose,
  navKeys,
  children,
}: {
  anchorId: string
  onClose: () => void
  navKeys?: boolean
  children: React.ReactNode
}) {
  const popRef = useRef<HTMLDivElement>(null)
  const [style, setStyle] = useState<React.CSSProperties>({})

  useEffect(() => {
    const place = () => {
      const anchor = document.getElementById(anchorId)
      const pop = popRef.current
      if (!anchor || !pop) return
      const r = anchor.getBoundingClientRect()
      setStyle({
        left: Math.max(8, Math.min(r.left, innerWidth - pop.offsetWidth - 8)) + 'px',
        bottom: innerHeight - r.top + 6 + 'px',
      })
    }
    place()
    const onDocClick = (e: MouseEvent) => {
      const t = e.target as Node
      if (popRef.current?.contains(t)) return
      const a = document.getElementById(anchorId)
      if (a && (a === t || a.contains(t))) return
      onClose()
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        onClose()
        return
      }
      if (!navKeys || (e.key !== 'ArrowDown' && e.key !== 'ArrowUp')) return
      const pop = popRef.current
      if (!pop) return
      const items = [...pop.querySelectorAll<HTMLElement>('.menu-item')]
      if (items.length === 0) return
      e.preventDefault()
      const idx = items.indexOf(document.activeElement as HTMLElement)
      // 未聚焦任何项时从“当前选中项”（或首项）出发，不会跳两步
      const base =
        idx >= 0
          ? idx
          : Math.max(0, items.indexOf(pop.querySelector('.menu-item.is-active') as HTMLElement))
      const next = items[(base + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length]
      next.focus()
      next.scrollIntoView({ block: 'nearest' })
    }
    const onBlur = () => onClose()
    document.addEventListener('click', onDocClick)
    document.addEventListener('keydown', onKey)
    window.addEventListener('blur', onBlur)
    window.addEventListener('resize', place)
    return () => {
      document.removeEventListener('click', onDocClick)
      document.removeEventListener('keydown', onKey)
      window.removeEventListener('blur', onBlur)
      window.removeEventListener('resize', place)
    }
  }, [anchorId, onClose, navKeys])

  return createPortal(
    <div
      id="menu"
      className="menu"
      ref={popRef}
      style={style}
      role="menu"
      aria-orientation="vertical"
    >
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
                {/* The ✓ slot always occupies space (empty when unselected) so all
                    rows stay right-aligned */}
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

function ApprovalMenu({ current, onPick }: { current: string; onPick: (mode: string) => void }) {
  return (
    <>
      {APPROVAL_OPTIONS.map((opt) => (
        <button
          key={opt.value}
          type="button"
          className={'menu-item menu-item-hint' + (opt.value === current ? ' is-active' : '')}
          onClick={() => onPick(opt.value)}
        >
          {opt.short} · {opt.value}
          <span className="check">{opt.value === current && <Icon name="check" />}</span>
          <span className="menu-item-desc">{opt.hint}</span>
        </button>
      ))}
    </>
  )
}
