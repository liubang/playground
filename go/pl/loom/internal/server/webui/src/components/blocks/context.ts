// context.ts — 块渲染的 IO 依赖注入（artifact 鉴权加载 / 完整工具输出拉取）。
// 主界面由 AppController 提供实现；分享页提供公开端点实现。

import { createContext, useContext } from 'react'

export interface ArtifactEntry {
  url: string
  mediaType: string
  blob: Blob
}

export interface BlocksIO {
  // artifact 加载：<img>/fetch 无法携带 Authorization 头，而 /v1/* 需要
  // Bearer 鉴权，因此用 fetch 拉取后生成 blob URL（内容寻址 + 不可变，
  // 按 id+size 缓存）。
  fetchArtifactURL: (id: string, size: number) => Promise<ArtifactEntry>
}

const noopIO: BlocksIO = {
  fetchArtifactURL: () => Promise.reject(new Error('BlocksIO not provided')),
}

export const BlocksIOContext = createContext<BlocksIO>(noopIO)

export function useBlocksIO(): BlocksIO {
  return useContext(BlocksIOContext)
}
