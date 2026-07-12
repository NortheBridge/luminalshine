import { createRouter, createWebHistory, RouteLocationNormalized } from 'vue-router';
import { useAuthStore } from '@/stores/auth';

// Route-level code splitting via dynamic imports
// Each view becomes a separate chunk loaded on demand
const DashboardView = () => import('@/views/DashboardView.vue');
const ApplicationsView = () => import('@/views/ApplicationsView.vue');
const SettingsView = () => import('@/views/SettingsView.vue');
const TroubleshootingView = () => import('@/views/TroubleshootingView.vue');
const ClientManagementView = () => import('@/views/ClientManagementView.vue');
const WebRtcClientView = () => import('@/views/WebRtcClientView.vue');
const AboutView = () => import('@/views/AboutView.vue');
const StatsView = () => import('@/views/StatsView.vue');

const routes = [
  { path: '/', component: DashboardView },
  { path: '/applications', component: ApplicationsView },
  { path: '/settings', component: SettingsView, meta: { container: 'lg' } },
  { path: '/logs', component: DashboardView },
  { path: '/troubleshooting', component: TroubleshootingView },
  { path: '/clients', component: ClientManagementView },
  { path: '/about', component: AboutView, meta: { container: 'lg' } },
  {
    path: '/api-tokens',
    alias: '/api-tokens/',
    redirect: { path: '/clients', query: { sec: 'tokens' } },
  },
  { path: '/webrtc', component: WebRtcClientView, meta: { container: 'full' } },
  { path: '/stats', component: StatsView },
];

const CHUNK_RELOAD_FLAG = 'sunshine:chunk-reload';
const chunkErrorPatterns = [
  'Failed to fetch dynamically imported module',
  'Importing a module script failed',
];

function isChunkLoadError(error: unknown): boolean {
  if (!error) return false;
  if (typeof error === 'string') {
    return chunkErrorPatterns.some((pattern) => error.includes(pattern));
  }
  if (error instanceof Error) {
    const message = error.message ?? '';
    if (chunkErrorPatterns.some((pattern) => message.includes(pattern))) {
      return true;
    }
    if (error.name === 'ChunkLoadError') {
      return true;
    }
    if ('code' in error && typeof (error as { code?: unknown }).code === 'string') {
      const code = (error as { code?: string }).code ?? '';
      return code === 'ERR_MODULE_NOT_FOUND';
    }
  }
  return false;
}

export const router = createRouter({
  // Use HTML5 history mode (no # in URLs)
  history: createWebHistory('/'),
  routes,
});

// Lightweight guard: if navigating to a protected route and not authenticated,
// open login modal (in-memory redirect) but allow navigation so URL stays.
router.beforeEach(async (to: RouteLocationNormalized) => {
  if (typeof window === 'undefined') return true;
  try {
    const auth = useAuthStore();
    // Ensure auth store initialized before route components mount
    if (!auth.ready && typeof auth.init === 'function') {
      try {
        await auth.init();
      } catch {
        /* ignore */
      }
    }
    // Stats-only sessions are pinned to /stats. UX only — the backend
    // allowlist is the actual security boundary; admin pages would just
    // render dead panels behind 403s.
    if (auth.isAuthenticated && auth.isStatsOnly() && to.path !== '/stats') {
      return { path: '/stats' };
    }
    // If not authenticated, trigger overlay (do not redirect)
    if (!auth.isAuthenticated) auth.requireLogin();
  } catch {
    /* ignore */
  }
  // Always allow navigation so URL remains intact
  return true;
});

router.onError((error) => {
  if (typeof window === 'undefined') return;
  if (!isChunkLoadError(error)) return;
  try {
    const storage = window.sessionStorage;
    if (storage && !storage.getItem(CHUNK_RELOAD_FLAG)) {
      storage.setItem(CHUNK_RELOAD_FLAG, Date.now().toString());
      window.location.reload();
      return;
    }
    storage?.removeItem(CHUNK_RELOAD_FLAG);
  } catch {}
  window.location.replace(window.location.origin);
});
