// images.tsx — 内联图片 / artifact 图片 + 灯箱（与旧 blocks.js 同逻辑）。
// - InlineImage：base64 data URI 缩略图，点击/回车放大（灯箱单例 overlay）
// - ArtifactImage：用户附件图片（artifact 引用，鉴权加载）
// - ArtifactBlock：artifact_ref part，按媒体类型分发（图片 / 文本预览+下载 / 仅下载）

import { useEffect, useState, type KeyboardEvent } from 'react'
import type { ArtifactRef } from '../../protocol/events'
import { fmtBytes } from '../../lib/format'
import { useBlocksIO } from './context'

// --- 图片灯箱（点击放大）：单例 overlay，点击遮罩或按 ESC 关闭 ---
let lightboxEl: HTMLDivElement | null = null
let lightboxPrevFocus: HTMLElement | null = null
let lightboxKeyHandler: ((e: globalThis.KeyboardEvent) => void) | null = null

function closeImageLightbox() {
  if (!lightboxEl) return
  // 拆掉随灯箱而生的 keydown 监听（不再让一个模块级监听器永远挂在 document 上）
  if (lightboxKeyHandler) {
    document.removeEventListener('keydown', lightboxKeyHandler, true)
    lightboxKeyHandler = null
  }
  lightboxEl.remove()
  lightboxEl = null
  document.body.classList.remove('lightbox-open')
  // 焦点归还到打开灯箱的那个缩略图，供屏幕阅读器用户继续阅读
  const prev = lightboxPrevFocus
  lightboxPrevFocus = null
  prev?.focus?.()
}

function openImageLightbox(src: string, alt?: string) {
  if (!src) return
  closeImageLightbox()
  const overlay = document.createElement('div')
  overlay.className = 'lightbox'
  overlay.setAttribute('role', 'dialog')
  overlay.setAttribute('aria-modal', 'true')
  overlay.setAttribute('aria-label', alt || 'image preview')
  overlay.tabIndex = -1
  const img = document.createElement('img')
  img.src = src
  img.alt = alt || 'image'
  overlay.appendChild(img)
  overlay.onclick = () => closeImageLightbox()
  document.body.appendChild(overlay)
  document.body.classList.add('lightbox-open')
  lightboxEl = overlay
  // 焦点进灯箱，使 Esc/Tab 不遇背景输入框
  lightboxPrevFocus = (document.activeElement as HTMLElement) || null
  overlay.focus()
  lightboxKeyHandler = (e: globalThis.KeyboardEvent) => {
    if (e.key === 'Escape') {
      e.stopPropagation()
      closeImageLightbox()
    }
  }
  document.addEventListener('keydown', lightboxKeyHandler, true)
}

function onZoomKeydown(e: KeyboardEvent<HTMLImageElement>) {
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault()
    openImageLightbox((e.target as HTMLImageElement).src, (e.target as HTMLImageElement).alt)
  }
}

const zoomableProps = {
  tabIndex: 0,
  title: '点击放大',
}

// InlineImage 渲染一个内联图片元素（base64 data URI）。
export function InlineImage({ mediaType, data }: { mediaType?: string; data: string }) {
  const [failed, setFailed] = useState(false)
  if (failed) return <div className="notice is-warn">图片加载失败</div>
  return (
    <img
      className="inline-image"
      loading="lazy"
      alt="image"
      src={`data:${mediaType || 'image/png'};base64,${data}`}
      onError={() => setFailed(true)}
      onClick={(e) => openImageLightbox((e.target as HTMLImageElement).src, 'image')}
      onKeyDown={onZoomKeydown}
      {...zoomableProps}
    />
  )
}

// ArtifactImage 渲染用户附件图片（artifact 引用）：异步鉴权加载，
// 加载完成前保持空缩略图占位，失败时原位替换为提示。
export function ArtifactImage({ artifact }: { artifact: ArtifactRef }) {
  const { fetchArtifactURL } = useBlocksIO()
  const [src, setSrc] = useState('')
  const [failed, setFailed] = useState(false)
  useEffect(() => {
    let alive = true
    fetchArtifactURL(artifact.id, artifact.size).then(
      ({ url }) => {
        if (alive) setSrc(url)
      },
      () => {
        if (alive) setFailed(true)
      },
    )
    return () => {
      alive = false
    }
  }, [artifact.id, artifact.size, fetchArtifactURL])
  if (failed) return <div className="notice is-warn">图片加载失败</div>
  return (
    <img
      className="inline-image"
      loading="lazy"
      alt="attachment image"
      src={src || undefined}
      onError={() => setFailed(true)}
      onClick={src ? (e) => openImageLightbox((e.target as HTMLImageElement).src) : undefined}
      onKeyDown={src ? onZoomKeydown : undefined}
      {...(src ? zoomableProps : {})}
    />
  )
}

// ArtifactBlock 渲染 artifact_ref part，按媒体类型分发：
// - image/*：内联图片（generate_image 的大型图片）；
// - 文本类（run_cmd 的 stdout/stderr artifact 等）：可展开的全文预览 + 下载；
// - 其他二进制：仅下载。
// 媒体类型优先取后端声明的 media_type；历史数据没有该字段时回退到 fetch
// 响应的 Content-Type（服务端 DetectContentType 嗅探）。文本预览读
// blob.text() 而非再 fetch blobURL：CSP connect-src 'self' 不覆盖 blob:。
export function ArtifactBlock({ artifact }: { artifact: ArtifactRef }) {
  const { fetchArtifactURL } = useBlocksIO()
  const [resolved, setResolved] = useState<{ url: string; mediaType: string; blob: Blob } | null>(
    null,
  )
  const [failed, setFailed] = useState(false)
  useEffect(() => {
    let alive = true
    fetchArtifactURL(artifact.id, artifact.size).then(
      (entry) => {
        if (alive) setResolved(entry)
      },
      () => {
        if (alive) setFailed(true)
      },
    )
    return () => {
      alive = false
    }
  }, [artifact.id, artifact.size, fetchArtifactURL])

  if (failed)
    return (
      <div className="block block-artifact">
        <div className="notice is-warn">artifact 加载失败</div>
      </div>
    )
  if (!resolved) return <div className="block block-artifact" />

  const type = artifact.media_type || resolved.mediaType || ''
  if (type.startsWith('image/')) {
    return (
      <div className="block block-artifact">
        <img
          className="inline-image"
          loading="lazy"
          alt="artifact image"
          src={resolved.url}
          onClick={(e) => openImageLightbox((e.target as HTMLImageElement).src)}
          onKeyDown={onZoomKeydown}
          {...zoomableProps}
        />
      </div>
    )
  }
  return (
    <div className="block block-artifact">
      <ArtifactFile url={resolved.url} mediaType={type} size={artifact.size} blob={resolved.blob} />
    </div>
  )
}

// ArtifactFile 非图片 artifact：summary 行（类型 + 大小 + 下载链接）；
// 文本类展开后从 blob 懒加载全文。
function ArtifactFile({
  url,
  mediaType,
  size,
  blob,
}: {
  url: string
  mediaType: string
  size: number
  blob: Blob
}) {
  const isText = mediaType.startsWith('text/') || mediaType === 'application/json'
  const [text, setText] = useState<string | null>(null)
  return (
    <details
      className="artifact-file disclosure"
      onToggle={(e) => {
        const d = e.target as HTMLDetailsElement
        if (!d.open || text !== null || !isText) return
        blob.text().then(
          (t) => setText(t),
          () => setText('(读取失败)'),
        )
      }}
    >
      <summary>
        <span className="artifact-file-label">
          {`${isText ? 'output artifact' : 'artifact'} · ${mediaType || 'binary'} · ${fmtBytes(size)}`}
        </span>
        <a
          className="artifact-download"
          href={url}
          download
          title="下载完整内容"
          onClick={(e) => e.stopPropagation()}
        >
          download
        </a>
      </summary>
      {isText && <div className="tool-preview mono">{text ?? ''}</div>}
    </details>
  )
}
