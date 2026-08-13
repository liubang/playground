// blocks.js — transcript 块渲染器（docs/WEB_DESIGN.md §3.3/§6.3）。
// 铁律：一切模型/工具文本只走 textContent；innerHTML 入口统一位于
// markdown.js（marked → DOMPurify；hljs 高亮片段经 sanitizeHtml 复用同一白名单）。

import { renderMarkdownInto } from '../markdown.js'
import { renderDiff } from '../diff.js'
import { fmtTokens, fmtBytes, fmtMsgTime, fmtMsgTimeTitle, copyText } from '../format.js'
import { icon } from '../icons.js'

export function el(tag, cls, text) {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text != null) e.textContent = text
  return e
}

// --- user ---

// 右侧气泡，无标签（参考截图样式）。
// createdAt: 可选 ISO/RFC 时间戳，用于悬浮显示该条消息的时间。
// images: 可选图片附件数组，元素为 artifact 引用 {id, size, media_type}
// 或内联 base64 {data, media_type}（历史 image part）；渲染在气泡内、
// 文字上方（附件先行、说明文字随后的阅读顺序）。resolveURL 为 artifact
// 鉴权加载入口（main.js 注入的 fetchArtifactURL）。
// 结构：容器 .block-user（无背景）→ 气泡 .user-bubble + 操作行
// （复制 + 时间），操作行在气泡下方右对齐，不占用气泡内部空间。
export function userBlock(text, createdAt, images, resolveURL) {
  const b = el('div', 'block block-user')
  const bubble = el('div', 'user-bubble')
  if (images && images.length > 0) {
    const wrap = el('div', 'user-images')
    for (const ref of images) {
      wrap.appendChild(
        ref.data != null
          ? inlineImageEl(ref.media_type, ref.data)
          : artifactImageEl(ref, resolveURL),
      )
    }
    bubble.appendChild(wrap)
  }
  if (text || !images || images.length === 0) {
    bubble.appendChild(el('div', 'user-text', text))
  }
  b.appendChild(bubble)
  b.appendChild(messageActions(b, 'user', { createdAt, getText: () => text }))
  return b
}

// --- assistant（完成态，markdown 渲染） ---
// 无标签，左侧纯文本（参考截图样式）。操作行不在建块时挂载——一轮里
// 「文本→工具→文本」有多段，由 transcript 在轮结束时一次性挂到末段
// （attachAssistantActions），避免中间段出现又消失的闪烁。
export function assistantBlock(text) {
  const b = el('div', 'block block-assistant')
  const md = el('div', 'md')
  renderMarkdownInto(md, text)
  b.appendChild(md)
  return b
}

// attachAssistantActions 给已渲染的 assistant 块补挂操作行（复制/反馈 + 时间）。
// createdAt 为该块内容的事件时间；fb 为反馈上下文（见 messageActions）。
// 幂等：已挂过的块直接跳过。
export function attachAssistantActions(blockEl, { createdAt, fb }) {
  if (!blockEl || blockEl.querySelector(':scope > .msg-actions')) return
  const md = blockEl.querySelector(':scope > .md')
  if (!md) return
  blockEl.appendChild(
    messageActions(blockEl, 'assistant', {
      createdAt,
      fb,
      getText: () => md.innerText,
    }),
  )
}

// --- stream（进行中草稿，markdown 实时渲染） ---
// 无标签，左侧纯文本。每个 delta 都会重渲全量 buffer（marked 对不完整
// 语法容错良好，未闭合的代码围栏也会按代码块渲染）；渲染走 rAF 合帧 +
// 最小间隔节流，避免长文本下每帧全量解析造成卡顿。
const STREAM_RENDER_MIN_INTERVAL_MS = 60

export function streamBlock() {
  const b = el('div', 'block block-assistant')
  const md = el('div', 'md')
  const cursor = el('span', 'stream-cursor', '▍')
  b.appendChild(md)
  let buf = ''
  let scheduled = false
  let destroyed = false
  let lastRender = 0
  const render = () => {
    scheduled = false
    if (destroyed) return
    lastRender = performance.now()
    renderMarkdownInto(md, buf)
    // 光标跟随渲染内容末尾：末节点是段落/列表项时嵌入其中，避免独占一行
    const last = md.lastElementChild
    if (last && (last.tagName === 'P' || last.tagName === 'LI')) last.appendChild(cursor)
    else md.appendChild(cursor)
  }
  const schedule = () => {
    if (scheduled || destroyed) return
    scheduled = true
    const wait = Math.max(0, STREAM_RENDER_MIN_INTERVAL_MS - (performance.now() - lastRender))
    setTimeout(() => requestAnimationFrame(render), wait)
  }
  return {
    el: b,
    append(delta) {
      buf += delta
      schedule()
    },
    text: () => buf,
    // 收笔：取消未执行的合帧，同步做最终渲染并移除光标。操作行由
    // transcript 在轮结束时统一挂载（attachAssistantActions）。
    finalize() {
      destroyed = true
      renderMarkdownInto(md, buf)
      cursor.remove()
    },
    // 丢弃（canonical 文本整段替换时）：停止后续合帧渲染。
    discard() {
      destroyed = true
    },
  }
}

// --- 消息操作行（复制 / 点赞 / 踩） + 时间 ---
// 挂在 block 末尾、对话结束后常显：既是反馈/复制入口，也是本条消息
// 已结束的标志。role: "user" 仅显示复制；"assistant" 显示复制 + 点赞/踩。
// 图标为静态常量 SVG（非模型/工具文本，不触碰 textContent 铁律）。
//
// fb（可选反馈上下文）：{ runId, feedback, onFeedback }
//   runId      本轮 run id；空则不渲染赞/踩（旧消息无 trace 可投）
//   feedback   初始态 "up" | "down" | ""（localStorage 恢复）
//   onFeedback async (runId, value) —— value 1=赞 0=踩；失败时操作行回滚
// 投票语义：已激活的按钮再点无效；点另一个按钮覆盖上一票（后端按
// 确定性 score id 幂等覆盖，不产生重复分数）。
// 图标统一走 icons.js（FA solid 内联 SVG，与 TUI NerdIcons 同族）；
// 均为静态常量，不触碰 textContent 铁律。
const ICONS = {
  copy: icon('copy'),
  check: icon('check'),
  up: icon('thumbs-up'),
  down: icon('thumbs-down'),
}

function iconBtn(cls, iconHtml, label) {
  const b = el('button', 'msg-action ' + cls)
  b.type = 'button'
  b.title = label
  b.setAttribute('aria-label', label)
  b.innerHTML = iconHtml // 常量 SVG，无用户输入
  return b
}

function messageActions(blockEl, role, { createdAt, getText, fb }) {
  const row = el('div', 'msg-actions')

  // 时间：常显短格式（8月6日 14:34），原生 tooltip 提供完整时间。
  // user 消息：时间在左、复制在右（整体右对齐于气泡下方）；
  // assistant 消息：按钮在左，时间靠行尾。
  let tip = null
  if (createdAt) {
    tip = el('span', 'msg-time-tip', fmtMsgTime(createdAt))
    tip.title = fmtMsgTimeTitle(createdAt)
  }
  if (tip && role === 'user') row.appendChild(tip)

  const copy = iconBtn('msg-copy', ICONS.copy, '复制该条消息')
  copy.onclick = async (e) => {
    e.stopPropagation()
    try {
      const text = typeof getText === 'function' ? await getText() : ''
      // copyText 内置非安全上下文（内网 IP）降级；失败抛错走 is-fail
      if (!(await copyText(text))) throw new Error('clipboard unavailable')
      copy.classList.add('is-done')
      copy.innerHTML = ICONS.check
    } catch {
      copy.classList.add('is-fail')
    }
    setTimeout(() => {
      copy.classList.remove('is-done', 'is-fail')
      copy.innerHTML = ICONS.copy
    }, 1500)
  }
  row.appendChild(copy)

  if (role === 'assistant' && fb && fb.runId && fb.onFeedback) {
    const vote = (btn, other, value) => async (e) => {
      e.stopPropagation()
      if (btn.classList.contains('is-active')) return // 已投该票：no-op
      const wasOther = other.classList.contains('is-active')
      btn.classList.add('is-active')
      other.classList.remove('is-active')
      try {
        await fb.onFeedback(fb.runId, value)
      } catch {
        // 提交失败：回滚到点击前的选中态
        btn.classList.remove('is-active')
        if (wasOther) other.classList.add('is-active')
      }
    }
    const up = iconBtn('msg-up', ICONS.up, '赞')
    const down = iconBtn('msg-down', ICONS.down, '踩')
    if (fb.feedback === 'up') up.classList.add('is-active')
    else if (fb.feedback === 'down') down.classList.add('is-active')
    up.onclick = vote(up, down, 1)
    down.onclick = vote(down, up, 0)
    row.appendChild(up)
    row.appendChild(down)
  }

  if (tip && role !== 'user') row.appendChild(tip)
  return row
}

// --- thinking（等待模型首个 token / 工具间等待的三点动画） ---
export function thinkingBlock() {
  const b = el('div', 'block block-thinking')
  b.appendChild(el('span', 't-dot'))
  b.appendChild(el('span', 't-dot'))
  b.appendChild(el('span', 't-dot'))
  return b
}

// --- reasoning（折叠块） ---

export function reasoningBlock() {
  const d = document.createElement('details')
  d.className = 'block block-reasoning disclosure'
  const summary = el('summary', '', 'reasoning')
  const body = el('div', 'body')
  d.appendChild(summary)
  d.appendChild(body)
  let buf = ''
  return {
    el: d,
    append(delta) {
      buf += delta
      body.textContent = buf
      summary.textContent = `reasoning · ${buf.length} chars`
    },
  }
}

// --- tool ---

// st → [图标, 文案]；注意 className 用 err（CSS 类）而 status 文本用 error
const TOOL_STATUS = {
  ok: ['check', 'ok'],
  err: ['xmark', 'error'],
  canceled: ['ban', 'canceled'],
}

// hooks.onCopy: async () => string —— 复制完整输出用。实时事件的 preview
// 是有界摘要，完整内容需向 server 取；snapshot 重建路径有 full_text，用不到。
export function toolBlock(payload, hooks = {}) {
  const b = el('div', 'block block-tool')
  const head = el('div', 'tool-head')
  head.appendChild(el('span', 'tool-name mono', payload.tool_name || 'tool'))
  if (payload.target) head.appendChild(el('span', 'tool-target mono', payload.target))
  const status = el('span', 'tool-status running', 'running')
  head.appendChild(status)
  const dur = el('span', 'tool-dur mono')
  head.appendChild(dur)
  b.appendChild(head)
  let errEl = null
  return {
    el: b,
    complete(p) {
      const st = p.status === 'success' ? 'ok' : p.status === 'error' ? 'err' : 'canceled'
      status.className = 'tool-status ' + st
      const meta = TOOL_STATUS[st]
      status.innerHTML = meta ? icon(meta[0]) + ' ' + meta[1] : ''
      if (!meta) status.textContent = p.status || 'done'
      if (p.duration_ms != null) dur.textContent = fmtDuration(p.duration_ms)
      if (p.error_message || p.error) {
        errEl = el('div', 'tool-error', p.error_message || p.error)
        b.appendChild(errEl)
      }
      if (p.preview) {
        b.appendChild(
          toolOutput(p.preview, async () => {
            if (p.full_text) return p.full_text
            if (hooks.onCopy) return hooks.onCopy()
            return p.preview // server 取不到时兜底复制摘要
          }),
        )
      }
      // 渲染工具结果中的图片。generate_image 的结果同时携带内联 base64
      // 图片和同一图片的 artifact 引用（前者供模型回看，后者供展示），
      // 两条都渲染会把同一张图显示两遍，因此内联图片优先（data: URI
      // 同步可用、无需带鉴权的二次请求）；仅当没有内联图片时（如超过
      // 内联大小上限的图）才走 artifact 路径。artifact 不一定是图片
      // （run_cmd 的 stdout/stderr artifact 是文本），由 artifactBlock
      // 按媒体类型分发渲染。
      const resultImages = p.images || []
      if (resultImages.length > 0) {
        for (const img of resultImages) {
          b.appendChild(imageBlock(img.media_type, img.data))
        }
      } else {
        for (const art of p.artifacts || []) {
          b.appendChild(artifactBlock(art, hooks.resolveArtifactURL))
        }
      }
    },
  }
}

// 工具输出区：默认折叠，展开显示有界 preview（截断以 "\n…" 结尾标记）；
// copy 按钮始终复制完整输出。
function toolOutput(preview, getFullText) {
  const d = document.createElement('details')
  d.className = 'tool-output disclosure'
  const summary = el('summary')
  const truncated = preview.endsWith('\n…')
  summary.appendChild(
    el(
      'span',
      'tool-output-label',
      `output · ${preview.length} chars${truncated ? ' · truncated' : ''}`,
    ),
  )
  const copyBtn = el('button', 'tool-copy', 'copy')
  copyBtn.title = '复制完整输出'
  copyBtn.onclick = async (e) => {
    e.preventDefault() // 不触发 details 展开/收起
    e.stopPropagation()
    try {
      const text = await getFullText()
      if (!(await copyText(text))) throw new Error('clipboard unavailable')
      copyBtn.innerHTML = ICONS.check + ' copied'
    } catch {
      copyBtn.textContent = 'copy failed'
    }
    setTimeout(() => {
      copyBtn.textContent = 'copy'
    }, 1500)
  }
  summary.appendChild(copyBtn)
  d.appendChild(summary)
  d.appendChild(el('div', 'tool-preview mono', preview))
  return d
}

function fmtDuration(ms) {
  if (ms < 1000) return ms + 'ms'
  return (ms / 1000).toFixed(1) + 's'
}

// --- 历史（snapshot 重建）工具块辅助 ---

// histTarget 从 tool_call.arguments（wire 上已是 object）提取展示用目标。
export function histTarget(call) {
  const a = call?.arguments
  if (!a || typeof a !== 'object') return ''
  const v = a.path || a.file_path || a.command || a.cmd || a.query || a.pattern || a.url || ''
  const s = String(v)
  return s.length > 120 ? s.slice(0, 120) + '…' : s
}

// histCompletion 把 ToolResult 映射为 toolBlock.complete(p) 需要的载荷。
// preview 有界（600 chars）用于展示；full_text 完整保留用于复制。
export function histCompletion(r) {
  const status =
    r.status === 'success' ? 'success' : r.status === 'cancelled' ? 'canceled' : 'error'
  const content = r.content || []
  const texts = content.filter((c) => c.kind === 'text' && c.text).map((c) => c.text)
  const fullText = texts.join('\n')
  let preview = fullText
  if (preview.length > 600) preview = preview.slice(0, 600) + '\n…'
  let durationMs
  if (r.started_at && r.finished_at) {
    const ms = new Date(r.finished_at) - new Date(r.started_at)
    if (Number.isFinite(ms) && ms >= 0) durationMs = ms
  }
  const images = content.filter((c) => c.kind === 'image' && c.image).map((c) => c.image)
  // model_only 的 artifact（view_image）不进展示通道：文本头已包含路径/
  // 类型/尺寸等审计信息，图片本体只给模型看（展示是 present_image 的职责）。
  const artifacts = content
    .filter((c) => c.kind === 'artifact_ref' && c.artifact && !c.model_only)
    .map((c) => c.artifact)
  return {
    status,
    duration_ms: durationMs,
    preview,
    full_text: fullText,
    error_message: r.error?.message || '',
    images,
    artifacts,
  }
}

// tool 块展开 diff（tool.prepared 挂载）
export function attachDiff(blockEl, diffText) {
  blockEl.appendChild(renderDiff(diffText))
}

// --- approval 卡片 ---

// callbacks: onResolve({decision, always})
// diffEl（可选）：调用方从工具块移入的已渲染 diff 节点。diff 只随
// tool.prepared 传输并渲染在工具块上，审批期间移入卡片，不重复渲染。
export function approvalCard(payload, { onResolve, diffEl }) {
  const card = el('div', 'block card-approval')
  const title = el('div', 'card-title')
  const titleLabel = el('span', 'card-title-label')
  titleLabel.innerHTML = icon('circle-question') + ' Approval required'
  title.appendChild(titleLabel)
  title.appendChild(el('span', 'risk', 'R' + (payload.risk ?? '?')))
  title.appendChild(el('span', 'mono', payload.tool_name || ''))
  card.appendChild(title)
  if (payload.description) card.appendChild(el('div', 'desc', payload.description))
  // cmd 仅在 target 与 description 不同时展示，避免同一段话渲染两遍
  if (payload.target && payload.target !== payload.description) {
    const cmd = el('div', 'cmd')
    cmd.appendChild(el('code', '', payload.target))
    card.appendChild(cmd)
  }
  if (diffEl) card.appendChild(diffEl)

  const actions = el('div', 'actions')
  const allow = el('button', 'btn btn-primary', 'Allow')
  const always = el('button', 'btn btn-secondary', 'Allow always')
  const deny = el('button', 'btn btn-danger', 'Deny')
  // rule_preview 为空表示该调用不可记忆（后端 ApprovalRulePreview），
  // 此时隐藏 Allow always，避免提供一个静默无效的选项。
  const preview = payload.rule_preview || ''
  const memo = el(
    'span',
    'memo',
    preview ? `allow always remembers "${preview}" for the workspace` : '',
  )
  allow.onclick = () => onResolve({ decision: 'allow', always: false })
  always.onclick = () => onResolve({ decision: 'allow', always: true })
  deny.onclick = () => onResolve({ decision: 'deny', always: false })
  actions.appendChild(allow)
  if (preview) actions.appendChild(always)
  actions.appendChild(deny)
  if (preview) actions.appendChild(memo)
  card.appendChild(actions)
  return {
    el: card,
    setResolving() {
      allow.disabled = always.disabled = deny.disabled = true
    },
  }
}

// resolved 收编 notice
export function resolvedNotice(ok, text) {
  const n = el('div', 'resolved')
  const st = el('span', ok ? 'ok' : 'no')
  st.innerHTML = icon(ok ? 'check' : 'xmark')
  n.appendChild(st)
  const t = el('span')
  t.appendChild(el('b', '', (ok ? 'Allowed' : 'Denied') + ' '))
  t.appendChild(document.createTextNode('(' + text.actor + ') · ' + text.what))
  n.appendChild(t)
  return n
}

// --- question 卡片 ---

// callbacks: onAnswer({selected, customText, skipped})
export function questionCard(payload, { onAnswer }) {
  const q = payload.question || payload // snapshot 重建时为 PendingRequest.Question
  const card = el('div', 'block card-question')
  const title = el('div', 'card-title')
  title.appendChild(el('span', '', '? Loom asks'))
  if (q.allow_multiple) title.appendChild(el('span', 'multi', '（可多选）'))
  card.appendChild(title)
  card.appendChild(el('div', 'q-text', q.text || ''))

  const inputType = q.allow_multiple ? 'checkbox' : 'radio'
  const name = 'q_' + Math.random().toString(36).slice(2, 8)
  const optionInputs = []
  for (const opt of q.options || []) {
    const label = el('label', 'opt')
    const input = document.createElement('input')
    input.type = inputType
    input.name = name
    input.value = opt.label
    const span = el('span')
    span.appendChild(document.createTextNode(opt.label))
    if (opt.description) span.appendChild(el('span', 'desc', ' — ' + opt.description))
    label.appendChild(input)
    label.appendChild(span)
    card.appendChild(label)
    optionInputs.push(input)
  }
  const custom = document.createElement('input')
  custom.type = 'text'
  custom.placeholder = 'custom answer… (optional)'
  card.appendChild(custom)

  const actions = el('div', 'actions')
  const submit = el('button', 'btn btn-primary', 'Submit')
  const skip = el('button', 'btn btn-secondary', 'Skip')
  submit.onclick = () => {
    const selected = optionInputs.filter((i) => i.checked).map((i) => i.value)
    onAnswer({ selected, custom_text: custom.value.trim(), skipped: false })
  }
  skip.onclick = () => onAnswer({ selected: [], custom_text: '', skipped: true })
  actions.appendChild(submit)
  actions.appendChild(skip)
  card.appendChild(actions)
  return {
    el: card,
    setResolving() {
      submit.disabled = skip.disabled = true
    },
  }
}

// --- plan 面板（plan.updated 驱动；钉在 composer 上方，Claude Code 风格清单） ---

const PLAN_STATUS_ICON = {
  todo: 'square-o',
  in_progress: 'square',
  done: 'square-check',
}

// renderPlanInto 就地重绘面板内容；不触碰 details.open，保留用户的折叠状态。
// plan 为空（无 items）时隐藏面板。
export function renderPlanInto(panel, plan) {
  panel.textContent = ''
  const items = plan?.items || []
  if (items.length === 0) {
    panel.hidden = true
    return
  }
  panel.hidden = false
  const done = items.filter((i) => i.status === 'done').length
  const summary = el('summary')
  summary.appendChild(el('span', 'plan-title', plan.title || 'plan'))
  summary.appendChild(el('span', 'plan-progress', `${done}/${items.length} done`))
  panel.appendChild(summary)
  const list = el('ul', 'plan-items')
  for (const item of items) {
    const li = el('li', 'plan-item is-' + (item.status || 'todo'))
    const ic = el('span', 'plan-icon')
    ic.innerHTML = icon(PLAN_STATUS_ICON[item.status] || 'square-o')
    li.appendChild(ic)
    li.appendChild(el('span', 'plan-goal', item.goal || ''))
    list.appendChild(li)
  }
  panel.appendChild(list)
}

// --- image（内联图片 / artifact 图片 + 灯箱） ---

// 图片灯箱（点击放大）：单例 overlay，点击任一 inline-image 时以近原尺寸
// 展示；点击遮罩或按 ESC 关闭，打开期间锁定 body 滚动。复用现有
// --z-modal 层级与 fadein 动画。
let lightboxEl = null

function openImageLightbox(src, alt) {
  if (!src) return
  closeImageLightbox()
  const overlay = el('div', 'lightbox')
  const img = document.createElement('img')
  img.src = src
  img.alt = alt || 'image'
  overlay.appendChild(img)
  overlay.onclick = () => closeImageLightbox()
  document.body.appendChild(overlay)
  document.body.classList.add('lightbox-open')
  lightboxEl = overlay
}

function closeImageLightbox() {
  if (!lightboxEl) return
  lightboxEl.remove()
  lightboxEl = null
  document.body.classList.remove('lightbox-open')
}

document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') closeImageLightbox()
})

// makeZoomable 给缩略图挂上「点击/回车放大」交互。
function makeZoomable(img) {
  img.tabIndex = 0
  img.title = '点击放大'
  img.onclick = () => openImageLightbox(img.src, img.alt)
  img.onkeydown = (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault()
      openImageLightbox(img.src, img.alt)
    }
  }
}

// inlineImageEl 渲染一个内联图片元素（base64 data URI）。
function inlineImageEl(mediaType, base64Data) {
  const img = document.createElement('img')
  img.className = 'inline-image'
  img.loading = 'lazy'
  img.alt = 'image'
  img.onerror = () => img.replaceWith(el('div', 'notice is-warn', '图片加载失败'))
  img.src = `data:${mediaType || 'image/png'};base64,${base64Data}`
  makeZoomable(img)
  return img
}

// imageBlock 渲染一个内联图片块（base64 data URI），用于 transcript 中的
// image part（工具结果中的内联图片；用户附件见 userBlock 的气泡内渲染）。
export function imageBlock(mediaType, base64Data) {
  const b = el('div', 'block block-image')
  b.appendChild(inlineImageEl(mediaType, base64Data))
  return b
}

// resolveArtifact 按引用取回 artifact 内容：优先走调用方注入的
// resolveURL(id, size)（带鉴权 fetch + blob URL），无注入时退回直链
// fetch（仅适用于无鉴权部署）。
function resolveArtifact(artifact, resolveURL) {
  const resolve =
    resolveURL ||
    ((id, size) =>
      fetch(`/v1/artifacts/${encodeURIComponent(id)}?size=${size}`)
        .then((r) => {
          if (!r.ok) throw new Error(`artifact fetch failed (HTTP ${r.status})`)
          return r.blob()
        })
        .then((blob) => ({
          url: URL.createObjectURL(blob),
          mediaType: blob.type || '',
          blob,
        })))
  return resolve(artifact.id, artifact.size)
}

// artifactImageEl 渲染用户附件图片（artifact 引用）：异步鉴权加载，
// 加载完成前保持空缩略图占位，失败时原位替换为提示。用户附件按契约
// 必为图片（storeImageAttachments 只收图片），非图片脏数据由 onerror
// 兜底，不在气泡内渲染下载卡片。
function artifactImageEl(artifact, resolveURL) {
  const img = document.createElement('img')
  img.className = 'inline-image'
  img.loading = 'lazy'
  img.alt = 'attachment image'
  img.onerror = () => img.replaceWith(el('div', 'notice is-warn', '图片加载失败'))
  resolveArtifact(artifact, resolveURL).then(
    ({ url }) => {
      img.src = url
      makeZoomable(img)
    },
    () => img.replaceWith(el('div', 'notice is-warn', '图片加载失败')),
  )
  return img
}

// artifactBlock 渲染 artifact_ref part，按媒体类型分发：
// - image/*：内联图片（generate_image 的大型图片，避免在 transcript JSON
//   中内嵌多 MB base64）；
// - 文本类（run_cmd 的 stdout/stderr artifact、compact 外化的工具输出
//   等）：可展开的全文预览 + 下载；
// - 其他二进制：仅下载。
// 媒体类型优先取后端在 artifact 上声明的 media_type；历史数据没有该字
// 段，回退到 fetch 响应的 Content-Type（服务端 DetectContentType 嗅探）。
// <img> 无法携带 Authorization 头，而 /v1/* 需要 Bearer 鉴权，因此实际
// 加载走 resolveURL(id, size) => Promise<{url, mediaType, blob}>（main.js
// 注入，fetch 带鉴权后 createObjectURL）；无 resolveURL 时退回直链 fetch
// （仅适用于无鉴权部署）。文本预览读 blob.text() 而非再 fetch blobURL：
// CSP connect-src 'self' 不覆盖 blob: scheme，二次 fetch 可能被拦。
export function artifactBlock(artifact, resolveURL) {
  const b = el('div', 'block block-artifact')
  const fail = () => b.replaceChildren(el('div', 'notice is-warn', 'artifact 加载失败'))
  resolveArtifact(artifact, resolveURL).then(({ url, mediaType, blob }) => {
    const type = artifact.media_type || mediaType || ''
    if (type.startsWith('image/')) {
      const img = document.createElement('img')
      img.className = 'inline-image'
      img.loading = 'lazy'
      img.alt = 'artifact image'
      img.onerror = () => b.replaceChildren(el('div', 'notice is-warn', '图片加载失败'))
      img.src = url
      makeZoomable(img)
      b.replaceChildren(img)
      return
    }
    b.replaceChildren(artifactFileBlock(url, type, artifact.size, blob))
  }, fail)
  return b
}

// artifactFileBlock 渲染非图片 artifact：summary 行（类型 + 大小 + 下载
// 链接）；文本类展开后从 blob 懒加载全文（鉴权问题已在 resolve 阶段解决）。
function artifactFileBlock(url, mediaType, size, blob) {
  const d = document.createElement('details')
  d.className = 'artifact-file disclosure'
  const summary = el('summary')
  const isText = mediaType.startsWith('text/') || mediaType === 'application/json'
  summary.appendChild(
    el(
      'span',
      'artifact-file-label',
      `${isText ? 'output artifact' : 'artifact'} · ${mediaType || 'binary'} · ${fmtBytes(size)}`,
    ),
  )
  const dl = el('a', 'artifact-download', 'download')
  dl.href = url
  dl.download = ''
  dl.title = '下载完整内容'
  dl.onclick = (e) => e.stopPropagation() // 不触发 details 展开/收起
  summary.appendChild(dl)
  d.appendChild(summary)
  if (isText) {
    const preview = el('div', 'tool-preview mono')
    let loaded = false
    d.addEventListener('toggle', () => {
      if (!d.open || loaded) return
      loaded = true
      blob.text().then(
        (t) => {
          preview.textContent = t
        },
        () => {
          preview.textContent = '(读取失败)'
        },
      )
    })
    d.appendChild(preview)
  }
  return d
}

// --- notice / fatal ---

export function noticeBlock(text, warn) {
  return el('div', 'notice' + (warn ? ' is-warn' : ''), text)
}

// context.compacted 明细卡片：压缩前后估值 + 触发原因 + 各级动作明细。
export function compactBlock(p) {
  const wrap = el('div', 'notice compact')
  const before = fmtTokens(p.est_tokens_before) || '?'
  const after = fmtTokens(p.est_tokens_after) || '?'
  const head = el('div', 'compact-head')
  head.innerHTML = icon('bolt') + ` context compacted · ${before} → ${after}`
  wrap.appendChild(head)
  const details = []
  if (p.trigger) details.push('trigger: ' + p.trigger)
  if (p.masked_outputs) {
    const bytes = p.masked_bytes ? ` (${fmtBytes(p.masked_bytes)})` : ''
    details.push(`mask ${p.masked_outputs} outputs${bytes}`)
  }
  if (p.archived_messages) details.push(`archive ${p.archived_messages} msgs`)
  if (p.summarized) details.push('summary handoff')
  if (details.length) wrap.appendChild(el('div', 'compact-detail', details.join(' · ')))
  return wrap
}

export function fatalBlock(text) {
  return el('div', 'block block-fatal', text)
}
