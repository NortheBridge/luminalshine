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
      // Tests live at <repoRoot>/tests/frontend/, outside the web package.
      // Node's module-resolution walk-up from there never reaches this
      // package's node_modules, so npm imports from test files (such as
      // @vue/test-utils) fail to resolve. Explicitly alias each npm package
      // the tests pull in to its absolute install location here.
      '@vue/test-utils': resolve(__dirname, 'node_modules/@vue/test-utils'),
      // Same reason as @vue/test-utils: tests at <repoRoot>/tests/frontend/
      // need this package to be reachable but Node's walk-up resolution
      // from there can't find the web package's node_modules.
      'naive-ui': resolve(__dirname, 'node_modules/naive-ui'),
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
    // Emit JUnit XML alongside the default console reporter so the
    // coverage-web GitHub Actions workflow can upload it to Codecov
    // Test Analytics. The dashboard ingests per-test pass/fail/flake
    // signals from this file; lcov.info handles line coverage
    // separately. Local `npm test` runs also emit the XML — it's
    // cheap (~kB) and removes the CI-vs-local config drift.
    reporters: ['default', 'junit'],
    outputFile: {
      junit: resolve(repoRoot, 'coverage/web/junit.xml'),
    },
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
