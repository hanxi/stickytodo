import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// 为什么 base: '/app/'？
// Go 后端用 embed.FS 把 dist/ 挂到 /app 下，所有资源 URL 需要带该前缀，
// 否则生产构建后 /assets/xxx.js 会 404。
export default defineConfig({
  base: '/app/',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
  },
  server: {
    port: 5173,
    strictPort: false,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
      '/health': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
});
