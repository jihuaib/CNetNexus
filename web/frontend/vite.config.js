import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

const BACKEND = process.env.BACKEND_URL || 'http://localhost:5174';

export default defineConfig({
    plugins: [vue()],
    server: {
        port: 5173,
        host: '0.0.0.0',
        proxy: {
            '/api': { target: BACKEND, changeOrigin: true },
            '/ws':  { target: BACKEND.replace(/^http/, 'ws'), ws: true, changeOrigin: true }
        }
    }
});
