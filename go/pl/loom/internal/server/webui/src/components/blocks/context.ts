// context.ts — IO dependency injection for block rendering (authenticated artifact loading / full tool output fetching).
// The main UI gets its implementation from AppController; the share page provides a public-endpoint implementation.

import { createContext, useContext } from 'react'

export interface ArtifactEntry {
  url: string
  mediaType: string
  blob: Blob
}

export interface BlocksIO {
  // Artifact loading: <img>/fetch cannot carry an Authorization header, while /v1/* requires
  // Bearer auth, so we fetch and then create a blob URL (content-addressed + immutable,
  // cached by id+size).
  fetchArtifactURL: (id: string, size: number) => Promise<ArtifactEntry>
  // revealFile opens a workspace path in the right panel's Files tab. Absent on
  // the read-only share page (no file tree there).
  revealFile?: (path: string) => void
}

const noopIO: BlocksIO = {
  fetchArtifactURL: () => Promise.reject(new Error('BlocksIO not provided')),
}

export const BlocksIOContext = createContext<BlocksIO>(noopIO)

export function useBlocksIO(): BlocksIO {
  return useContext(BlocksIOContext)
}
