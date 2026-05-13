import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';
import { resolve } from 'node:path';

const repoRoot = resolve(__dirname, '../../../..');

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@web': __dirname,
      '@': __dirname,
    },
  },
  // Vite's default fs.allow is the directory containing this config
  // (src_assets/common/assets/web/). Tests live at <repoRoot>/tests/frontend/,
  // which is outside that root, so Vite refuses to load setup.ts and the
  // failure surfaces as a confusing "Does the file exist?" error. Whitelist
  // the repo root so setupFiles and test source files load cleanly.
  server: {
    fs: {
      allow: [repoRoot],
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: [resolve(repoRoot, 'tests/frontend/setup.ts')],
    include: [resolve(repoRoot, 'tests/frontend/**/*.test.ts')],
    css: true,
    coverage: {
      provider: 'v8',
      reporter: ['text', 'lcov', 'json'],
      reportsDirectory: resolve(repoRoot, 'coverage/web'),
      include: ['src_assets/common/assets/web/**/*.{ts,vue}'],
      exclude: [
        'src_assets/common/assets/web/**/*.d.ts',
        'src_assets/common/assets/web/**/__tests__/**',
        'src_assets/common/assets/web/main.ts',
        'src_assets/common/assets/web/env.d.ts',
      ],
      all: true,
    },
  },
});
