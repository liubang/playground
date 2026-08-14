// composer.js — 输入框（IME 安全、自适应高度、steer 态、取消、图片附件）。
//
// 图片附件三条入口：剪贴板粘贴（paste）、拖入 composer（drop）、附件按钮
// 选择文件。图片在客户端经 canvas 压缩（最长边 MAX_DIM）后以 base64 随
// prompt 一并提交；服务端落 artifact store，模型请求时再 derive 线上格式
// （见 internal/media）。GIF/小图保留原始字节，避免破坏动图与不必要的重编码。

import { iconEl } from '../icons.js'

// MAX_ATTACHMENTS 与后端 maxImageAttachments（internal/ui/update.go）对齐。
const MAX_ATTACHMENTS = 5
// 单张原图上限（压缩前），与 TUI 的 maxImageLoadBytes 一致。
const MAX_SOURCE_BYTES = 20 * 1024 * 1024
// 导出最长边：视觉模型推荐的分辨率上限，同时把 base64 体积压在 body 限制内。
const MAX_DIM = 1568

// blobToBase64 读取 Blob 为不含 "data:" 前缀的 base64 字符串。
function blobToBase64(blob) {
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

export class Composer {
  // callbacks: onSubmit(text, images), onCancel(), onError(message)
  constructor({
    textarea,
    sendBtn,
    cancelBtn,
    attachBtn,
    fileInput,
    stripEl,
    boxEl,
    onSubmit,
    onCancel,
    onError,
  }) {
    this.ta = textarea
    this.attachBtn = attachBtn || null
    this.fileInput = fileInput || null
    this.stripEl = stripEl || null
    this.boxEl = boxEl || null
    this.onError = onError || ((m) => console.warn(m))
    this.running = false
    this.readOnly = false
    // 当前模型不支持图片输入时置 false 并给出原因（粘贴/拖拽/按钮统一拦截）
    this.imagesEnabled = true
    this.imagesDisabledReason = ''
    this.composing = false
    this.focused = false
    // 附件：[{name, mediaType, data, previewUrl}] —— data 为 base64（无 data: 前缀）
    this.attachments = []

    // followup=true：排入下一轮队列（turn 结束后接力为下一轮的 prompt），
    // 而不是注入当前 turn（deepseek-harness 的 next-turn 投递）。
    const submit = (followup = false) => {
      if (this.readOnly) return // 只读会话（子 agent）：不允许追问
      const text = this.ta.value.trim()
      if (!text && this.attachments.length === 0) return
      onSubmit(
        text,
        this.attachments.map((a) => ({
          media_type: a.mediaType,
          data: a.data,
        })),
        followup,
      )
    }

    sendBtn.onclick = () => submit()
    cancelBtn.onclick = onCancel

    this.ta.addEventListener('compositionstart', () => {
      this.composing = true
    })
    this.ta.addEventListener('compositionend', () => {
      this.composing = false
    })
    this.ta.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && !e.shiftKey && !this.composing && !e.isComposing) {
        e.preventDefault()
        // Ctrl/Cmd+Enter：排队到下一轮；普通 Enter：steer 当前轮 / 直接发送
        submit(e.ctrlKey || e.metaKey)
      }
    })
    // 粘贴：剪贴板里只取图片项；纯文本走默认插入。截图（Cmd+V）命中此路径。
    this.ta.addEventListener('paste', (e) => this._onPaste(e))
    this.ta.addEventListener('input', () => this._autosize())
    this.ta.addEventListener('focus', () => {
      this.focused = true
      this._applyState()
    })
    this.ta.addEventListener('blur', () => {
      this.focused = false
      this._applyState()
    })

    // 拖拽：仅接受文件类型，避免拦截选区内的文本拖放。
    if (this.boxEl) {
      this.boxEl.addEventListener('dragover', (e) => {
        if (this.readOnly) return
        if ([...(e.dataTransfer?.types || [])].includes('Files')) {
          e.preventDefault()
          this.boxEl.classList.add('is-dragover')
        }
      })
      this.boxEl.addEventListener('dragleave', (e) => {
        if (!this.boxEl.contains(e.relatedTarget)) this.boxEl.classList.remove('is-dragover')
      })
      this.boxEl.addEventListener('drop', (e) => {
        this.boxEl.classList.remove('is-dragover')
        if (this.readOnly) return
        const files = [...(e.dataTransfer?.files || [])].filter((f) => f.type.startsWith('image/'))
        if (files.length === 0) return
        e.preventDefault()
        this._addFiles(files)
      })
    }

    // 附件按钮 → 隐藏的 <input type=file>
    if (this.attachBtn && this.fileInput) {
      this.attachBtn.onclick = () => {
        if (!this.readOnly) this.fileInput.click()
      }
      this.fileInput.addEventListener('change', () => {
        this._addFiles([...(this.fileInput.files || [])])
        this.fileInput.value = '' // 允许重复选择同一文件
      })
    }
  }

  // _onPaste 处理剪贴板：有图片项时消费图片；同时带文本则保留文本插入。
  _onPaste(e) {
    if (this.readOnly) return
    const files = []
    for (const item of e.clipboardData?.items || []) {
      if (item.kind === 'file' && item.type.startsWith('image/')) {
        const f = item.getAsFile()
        if (f) files.push(f)
      }
    }
    if (files.length === 0) return // 纯文本粘贴走默认行为
    const hasText = (e.clipboardData.getData('text/plain') || '').length > 0
    if (!hasText) e.preventDefault() // 纯图片：阻止插入文件名噪声
    this._addFiles(files)
  }

  async _addFiles(files) {
    if (!this.imagesEnabled) {
      if (files.length) this.onError(this.imagesDisabledReason || '当前模型不支持图片输入')
      return
    }
    for (const f of files) {
      if (this.attachments.length >= MAX_ATTACHMENTS) {
        this.onError(`最多附带 ${MAX_ATTACHMENTS} 张图片`)
        return
      }
      if (f.size > MAX_SOURCE_BYTES) {
        this.onError(`图片「${f.name || '剪贴板'}」超过 ${MAX_SOURCE_BYTES / 1024 / 1024}MB 上限`)
        continue
      }
      try {
        const att = await this._fileToAttachment(f)
        this.attachments.push(att)
        this._renderStrip()
      } catch (err) {
        this.onError('图片读取失败: ' + err.message)
      }
    }
  }

  // 客户端压缩：>MAX_DIM 的图用 canvas 缩到最长边 MAX_DIM，导出 jpeg（不透明）
  // 或 png（含透明通道）；小图直接读原始字节，保留 gif 动图与原始编码。
  async _fileToAttachment(file) {
    const bitmap = await createImageBitmap(file)
    const scale = Math.min(1, MAX_DIM / Math.max(bitmap.width, bitmap.height))
    let blob = file
    let mediaType = file.type || 'image/png'
    if (scale < 1) {
      const canvas = document.createElement('canvas')
      canvas.width = Math.round(bitmap.width * scale)
      canvas.height = Math.round(bitmap.height * scale)
      canvas.getContext('2d').drawImage(bitmap, 0, 0, canvas.width, canvas.height)
      const keepPng =
        mediaType === 'image/png' || mediaType === 'image/webp' || mediaType === 'image/gif'
      mediaType = keepPng ? 'image/png' : 'image/jpeg'
      blob = await new Promise((resolve, reject) =>
        canvas.toBlob(
          (b) => (b ? resolve(b) : reject(new Error('encode failed'))),
          mediaType,
          0.85,
        ),
      )
    }
    bitmap.close?.()
    const data = await blobToBase64(blob)
    return {
      name: file.name || 'clipboard',
      mediaType,
      data,
      previewUrl: URL.createObjectURL(blob),
    }
  }

  _renderStrip() {
    if (!this.stripEl) return
    this.stripEl.textContent = ''
    this.stripEl.hidden = this.attachments.length === 0
    this.attachments.forEach((att, i) => {
      const item = document.createElement('div')
      item.className = 'attach-item'
      const img = document.createElement('img')
      img.src = att.previewUrl
      img.alt = att.name
      const rm = document.createElement('button')
      rm.type = 'button'
      rm.className = 'attach-remove'
      rm.title = '移除'
      rm.appendChild(iconEl('xmark'))
      rm.onclick = () => {
        URL.revokeObjectURL(att.previewUrl)
        this.attachments.splice(i, 1)
        this._renderStrip()
      }
      item.appendChild(img)
      item.appendChild(rm)
      this.stripEl.appendChild(item)
    })
  }

  _autosize() {
    this.ta.style.height = 'auto'
    this.ta.style.height = Math.min(200, this.ta.scrollHeight) + 'px'
  }

  clearDraft() {
    this.ta.value = ''
    this._autosize()
    for (const a of this.attachments) URL.revokeObjectURL(a.previewUrl)
    this.attachments = []
    this._renderStrip()
  }

  restoreDraft(text) {
    this.ta.value = text
    this._autosize()
  }

  draft() {
    return this.ta.value
  }

  setRunning(running) {
    this.running = running
    this._applyState()
  }

  // setReadOnly 切换只读模式（子 agent 会话）：输入禁用，提示固定；
  // 审批/提问走 transcript 卡片，不受影响。
  setReadOnly(readOnly) {
    this.readOnly = readOnly
    this.ta.disabled = readOnly
    if (this.attachBtn) this.attachBtn.disabled = readOnly || !this.imagesEnabled
    this._applyState()
  }

  // setImagesEnabled 按当前模型的 modalities 门控图片附件入口：禁用时
  // 附件按钮置灰并给出原因提示；粘贴/拖拽在 _addFiles 入口统一拦截。
  setImagesEnabled(enabled, reason = '') {
    this.imagesEnabled = enabled
    this.imagesDisabledReason = reason
    if (this.attachBtn) {
      this.attachBtn.disabled = !enabled || this.readOnly
      this.attachBtn.title = enabled
        ? '添加图片（支持直接粘贴 / 拖拽）'
        : reason || '当前模型不支持图片输入'
    }
  }

  // placeholder 随焦点切换：聚焦时显示按键提示，失焦时显示引导文案。
  _applyState() {
    if (this.readOnly) {
      this.ta.placeholder = '子 agent 会话 · 只读'
      return
    }
    if (this.focused) {
      this.ta.placeholder = this.running
        ? 'Enter to steer the running turn · Ctrl+Enter to queue for the next turn · Shift+Enter for newline'
        : 'Enter to send · Shift+Enter for newline · paste/drop images'
    } else {
      this.ta.placeholder = this.running
        ? 'Steer this turn… (Ctrl+Enter queues for next turn)'
        : 'Message loom…'
    }
  }

  setCancellable(cancellable, btn) {
    btn.hidden = !cancellable
  }
}
