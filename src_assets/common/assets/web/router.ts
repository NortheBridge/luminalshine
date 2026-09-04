import { createRouter, createWebHistory, RouteLocationNormalized } from 'vue-router';
import type { RouteLocationRaw, RouteRecordRaw } from 'vue-router';
import { useAuthStore } from '@/stores/auth';

// Route-level code splitting via dynamic imports
// Each view becomes a separate chunk loaded on demand
const OverviewView = () => import('@/views/OverviewView.vue');
const StreamView = () => import('@/views/StreamView.vue');
const LibraryView = () => import('@/views/LibraryView.vue');
const SettingsView = () => import('@/views/SettingsView.vue');
const DiagnosticsView = () => import('@/views/DiagnosticsView.vue');
const ClientsView = () => import('@/views/ClientsView.vue');
const WebRtcClientView = () => import('@/views/WebRtcClientView.vue');
const DisplayView = () => import('@/views/DisplayView.vue');

// Mission Control navigation: seven destinations plus the in-browser
// player. Paths stay single-depth on purpose: the Vite build uses base './',
// so assets resolve relative to the URL — a nested route would request
// /display/assets/*.
//
// Route meta:
//   container — content wrapper, see App.vue containerClass()
//   title     — locale key for the page title shown in the mobile header
const routes: RouteRecordRaw[] = [
  { path: '/', component: OverviewView, meta: { container: 'flush', title: 'shell.nav_overview' } },
  {
    path: '/stream',
    component: StreamView,
    meta: { container: 'full', title: 'shell.nav_stream' },
  },
  {
    path: '/webrtc',
    component: WebRtcClientView,
    meta: { container: 'full', title: 'shell.nav_play' },
  },
  {
    path: '/library',
    component: LibraryView,
    meta: { container: 'full', title: 'shell.nav_library' },
  },
  {
    path: '/clients',
    component: ClientsView,
    meta: { container: 'full', title: 'shell.nav_clients' },
  },
  {
    path: '/display',
    component: DisplayView,
    meta: { container: 'lg', title: 'shell.nav_display' },
  },
  {
    path: '/settings',
    component: SettingsView,
    meta: { container: 'lg', title: 'shell.nav_settings' },
  },
  {
    path: '/diagnostics',
    component: DiagnosticsView,
    meta: { container: 'lg', title: 'shell.nav_diagnostics' },
  },

  // Legacy paths. Bookmarks, deep links inside older pages and the
  // classic Sunshine URLs all land on their new home.
  { path: '/applications', redirect: '/library' },
  { path: '/stats', redirect: '/stream' },
  { path: '/logs', redirect: { path: '/diagnostics', hash: '#logs' } },
  {
    path: '/troubleshooting',
    redirect: (to): RouteLocationRaw => ({
      path: '/diagnostics',
      query: { ...to.query },
      hash: to.hash,
    }),
  },
  { path: '/about', redirect: { path: '/diagnostics', query: { sec: 'about' } } },
  { path: '/vgd-control-panel', redirect: '/display' },
  { path: '/vgd-about', redirect: { path: '/display', query: { sec: 'about' } } },
  {
    path: '/api-tokens',
    alias: '/api-tokens/',
    redirect: { path: '/clients', query: { sec: 'tokens' } },
  },
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

/** The page a stats-only session is pinned to. */
export const STATS_ONLY_HOME = '/stream';

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
    // Stats-only sessions are pinned to the Stream page. UX only — the
    // backend allowlist is the actual security boundary; admin pages would
    // just render dead panels behind 403s.
    if (auth.isAuthenticated && auth.isStatsOnly() && to.path !== STATS_ONLY_HOME) {
      return { path: STATS_ONLY_HOME };
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
  } catch {
    /* sessionStorage unavailable; fall through to a plain reload */
  }
  window.location.replace(window.location.origin);
});
