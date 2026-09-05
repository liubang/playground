import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Output goes to ../web/dist and is embedded into the loom binary by
// internal/server/web/web.go via embed.FS (dist is committed; Bazel only globs the artifacts and does not manage the JS build).
export default defineConfig({
  plugins: [react()],
  build: {
    outDir: '../web/dist',
    emptyOutDir: true,
    rollupOptions: {
      input: {
        index: new URL('./index.html', import.meta.url).pathname,
        share: new URL('./share.html', import.meta.url).pathname,
      },
    },
  },
  server: {
    port: 5173,
    // Dev-time proxy to the loom serve default listener (internal/server/server.go)
    proxy: {
      '/v1': 'http://127.0.0.1:7680',
    },
  },
})
