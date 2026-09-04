import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';

// Keep the test hermetic: no HTTP, and no heavy view modules — the router
// only needs *a* component per route to resolve redirects.
vi.mock('@web/http', () => ({
  http: {
    get: vi.fn(async () => ({ status: 200, data: {} })),
    post: vi.fn(async () => ({ status: 200, data: {} })),
    delete: vi.fn(async () => ({ status: 200, data: {} })),
  },
  refreshSession: vi.fn(async () => false),
  initHttpLayer: vi.fn(async () => {}),
}));
vi.mock('@web/views/OverviewView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/StreamView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/LibraryView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/SettingsView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/DiagnosticsView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/ClientsView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/WebRtcClientView.vue', () => ({ default: { template: '<div />' } }));
vi.mock('@web/views/DisplayView.vue', () => ({ default: { template: '<div />' } }));

import { router, STATS_ONLY_HOME } from '@web/router';
import { useAuthStore } from '@web/stores/auth';

describe('router', () => {
  beforeEach(async () => {
    setActivePinia(createPinia());
    const auth = useAuthStore();
    auth.ready = true;
    auth.setRole('admin');
    auth.setAuthenticated(true);
    await router.push('/');
    await router.isReady();
  });

  it('sends the classic Sunshine and pre-merge paths to their new home', async () => {
    const cases: Array<[string, string]> = [
      ['/applications', '/library'],
      ['/stats', '/stream'],
      ['/troubleshooting', '/diagnostics'],
      ['/vgd-control-panel', '/display'],
    ];
    for (const [from, to] of cases) {
      await router.push(from);
      expect(router.currentRoute.value.path, from).toBe(to);
    }
    await router.push('/about');
    expect(router.currentRoute.value.fullPath).toBe('/diagnostics?sec=about');
    await router.push('/vgd-about');
    expect(router.currentRoute.value.fullPath).toBe('/display?sec=about');
    await router.push('/logs');
    expect(router.currentRoute.value.fullPath).toBe('/diagnostics#logs');
    await router.push('/troubleshooting#logs');
    expect(router.currentRoute.value.fullPath).toBe('/diagnostics#logs');
    await router.push('/api-tokens');
    expect(router.currentRoute.value.fullPath).toBe('/clients?sec=tokens');
  });

  it('keeps the eight Mission Control destinations addressable', async () => {
    for (const path of [
      '/',
      '/stream',
      '/webrtc',
      '/library',
      '/clients',
      '/display',
      '/settings',
      '/diagnostics',
    ]) {
      await router.push(path);
      expect(router.currentRoute.value.path, path).toBe(path);
      expect(router.currentRoute.value.matched.length, path).toBeGreaterThan(0);
    }
  });

  it('pins a stats-only session to the Stream page', async () => {
    const auth = useAuthStore();
    auth.setRole('stats');
    await router.push('/settings');
    expect(router.currentRoute.value.path).toBe(STATS_ONLY_HOME);
    await router.push('/');
    expect(router.currentRoute.value.path).toBe(STATS_ONLY_HOME);
  });
});
