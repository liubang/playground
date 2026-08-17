import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// base 必须是 /console/：后端 brpc 仅把 /console/* 映射到 web_dir。
// 路由使用 hash 模式（后端静态服务不做 history fallback）。
export default defineConfig({
  base: '/console/',
  plugins: [vue()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    chunkSizeWarningLimit: 1024,
  },
  server: {
    // 本地开发时代理 /api、/healthz 到本地 minisearch server
    proxy: {
      '/api': { target: 'http://127.0.0.1:8080', changeOrigin: true },
      '/healthz': { target: 'http://127.0.0.1:8080', changeOrigin: true },
    },
  },
})
