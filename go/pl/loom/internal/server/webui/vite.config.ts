import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// 产物输出到 ../web/dist，由 internal/server/web/web.go 经 embed.FS 内嵌进
// loom 二进制（提交 dist 入库，Bazel 只 glob 产物，不纳管 JS 构建）。
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
    // 开发期代理到 loom serve 默认监听（internal/server/server.go）
    proxy: {
      '/v1': 'http://127.0.0.1:7680',
    },
  },
})
