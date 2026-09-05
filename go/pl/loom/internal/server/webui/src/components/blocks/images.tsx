// images.tsx — inline images / artifact images + lightbox (same logic as the old blocks.js).
// - InlineImage: base64 data URI thumbnail, click/Enter to zoom (singleton lightbox overlay)
// - ArtifactImage: user attachment image (artifact reference, authenticated load)
// - ArtifactBlock: artifact_ref part, dispatched by media type (image / text preview + download / download only)

import { useEffect, useState, type KeyboardEvent } from 'react'
import type { ArtifactRef } from '../../protocol/events'
import { fmtBytes } from '../../lib/format'
import { useBlocksIO } from './context'

// --- Image lightbox (click to zoom): singleton overlay, closed by clicking the backdrop or pressing ESC ---
let lightboxEl: HTMLDivElement | null = null
let lightboxPrevFocus: HTMLElement | null = null
let lightboxKeyHandler: ((e: globalThis.KeyboardEvent) => void) | null = null

function closeImageLightbox() {
  if (!lightboxEl) return
  // Remove the keydown listener created with the lightbox (no more module-level listener left on document forever)
  if (lightboxKeyHandler) {
    document.removeEventListener('keydown', lightboxKeyHandler, true)
    lightboxKeyHandler = null
  }
  lightboxEl.remove()
  lightboxEl = null
  document.body.classList.remove('lightbox-open')
  // Return focus to the thumbnail that opened the lightbox, so screen reader users can continue reading
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
  img.decoding = 'async'
  overlay.appendChild(img)
  overlay.onclick = () => closeImageLightbox()
  document.body.appendChild(overlay)
  document.body.classList.add('lightbox-open')
  lightboxEl = overlay
  // Move focus into the lightbox so Esc/Tab don't reach background inputs
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

// Shared props for zoomable thumbnails: async decoding keeps large base64 images off the
// load-event critical path (decode happens on the raster thread when idle).
const zoomableProps = {
  tabIndex: 0,
  title: '点击放大',
  decoding: 'async' as const,
}

// InlineImage renders an inline image element (base64 data URI).
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

// ArtifactImage renders a user attachment image (artifact reference): loads asynchronously with auth;
// keeps an empty thumbnail placeholder until loaded, replaced in place by a notice on failure.
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

// ArtifactBlock renders an artifact_ref part, dispatched by media type:
// - image/*: inline image (large images from generate_image);
// - text-like (run_cmd's stdout/stderr artifacts, etc.): expandable full-text preview + download;
// - other binaries: download only.
// The media type prefers the backend-declared media_type; historical data without that field falls back to the fetch
// response's Content-Type (server-side DetectContentType sniffing). The text preview reads
// blob.text() instead of fetching the blobURL again: CSP connect-src 'self' does not cover blob:.
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

// ArtifactFile, non-image artifact: summary row (type + size + download link);
// text-like content is lazily loaded from the blob once expanded.
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
          {`${isText ? '输出附件' : '附件'} · ${mediaType || '二进制'} · ${fmtBytes(size)}`}
        </span>
        <a
          className="artifact-download"
          href={url}
          download
          title="下载完整内容"
          onClick={(e) => e.stopPropagation()}
        >
          下载
        </a>
      </summary>
      {isText && <div className="tool-preview mono">{text ?? ''}</div>}
    </details>
  )
}
