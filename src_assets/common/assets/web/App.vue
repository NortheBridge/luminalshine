<template>
  <n-config-provider :theme="darkTheme" :theme-overrides="naiveOverrides">
    <n-loading-bar-provider>
      <n-dialog-provider>
        <n-notification-provider>
          <n-message-provider>
            <div class="flex h-screen w-full overflow-hidden bg-dark text-onDark">
              <!-- Icon rail (desktop) -->
              <nav
                class="hidden w-14 shrink-0 flex-col items-center gap-1.5 border-r border-line bg-chrome py-3 md:flex"
                :aria-label="t2('shell.nav_label', 'Primary')"
              >
                <RouterLink to="/" class="mb-2.5 shrink-0" aria-label="LuminalShine">
                  <img
                    src="/images/logo-luminalshine.png"
                    alt=""
                    aria-hidden="true"
                    class="h-[30px] w-[30px] rounded-full"
                  />
                </RouterLink>
                <RouterLink
                  v-for="item in navItems"
                  :key="item.path"
                  :to="item.path"
                  class="rail-btn"
                  :class="{ 'rail-btn-on': isActive(item.path) }"
                  :title="item.label"
                  :aria-label="item.label"
                >
                  <NavIcon :name="item.icon" :size="20" />
                </RouterLink>
                <div class="flex-1"></div>
                <button
                  type="button"
                  class="rail-btn"
                  :title="t2('navbar.logout', 'Log out')"
                  :aria-label="t2('navbar.logout', 'Log out')"
                  @click="logout"
                >
                  <NavIcon name="logout" :size="20" />
                </button>
              </nav>

              <div class="flex min-w-0 flex-1 flex-col">
                <!-- Host strip (desktop) -->
                <HostStrip />

                <!-- Compact header (mobile) -->
                <header
                  class="flex h-14 shrink-0 items-center gap-3 border-b border-line bg-chrome px-4 md:hidden"
                >
                  <RouterLink to="/" class="flex items-center gap-2.5" aria-label="LuminalShine">
                    <img
                      src="/images/logo-luminalshine.png"
                      alt=""
                      aria-hidden="true"
                      class="h-7 w-7 rounded-full"
                    />
                    <span class="text-sm font-semibold">LuminalShine</span>
                  </RouterLink>
                  <span class="truncate text-sm text-ink-3">{{ pageTitle }}</span>
                  <div class="ml-auto flex items-center gap-2">
                    <SavingStatus />
                    <n-dropdown
                      trigger="click"
                      :show-arrow="true"
                      :options="mobileMenuOptions"
                      @select="onMobileSelect"
                    >
                      <n-button type="default" size="small" :aria-label="t2('shell.menu', 'Menu')">
                        <NavIcon name="menu" :size="16" />
                      </n-button>
                    </n-dropdown>
                  </div>
                </header>

                <div class="flex min-h-0 flex-1">
                  <main class="app-scrollbar min-w-0 flex-1 overflow-y-auto">
                    <RouterView v-slot="{ Component, route: r }">
                      <div :class="containerClass(r)">
                        <Transition name="fade-fast" mode="out-in">
                          <component :is="Component" />
                        </Transition>
                      </div>
                    </RouterView>
                  </main>
                  <!-- Inspector column: pages teleport their contextual panel here.
                       Below lg it is a slide-over drawer behind a floating toggle. -->
                  <div
                    v-if="drawerOpen"
                    class="fixed inset-0 z-[85] bg-dark/60 lg:hidden"
                    aria-hidden="true"
                    @click="drawerOpen = false"
                  ></div>
                  <aside
                    :id="INSPECTOR_TARGET_ID"
                    class="shrink-0 flex-col overflow-hidden bg-chrome lg:static lg:flex lg:transition-[width] lg:duration-200"
                    :class="[
                      inspectorOpen
                        ? 'lg:w-[340px] lg:border-l lg:border-line'
                        : 'lg:w-0 lg:border-0',
                      drawerOpen
                        ? 'fixed inset-y-0 right-0 z-[90] flex w-[340px] max-w-[90vw] border-l border-line shadow-2xl'
                        : 'hidden',
                    ]"
                  ></aside>
                  <button
                    v-if="inspectorOpen"
                    type="button"
                    class="fixed bottom-4 right-4 z-[95] flex h-11 items-center gap-2 rounded-full border border-line-strong bg-chrome px-4 text-xs font-medium text-ink shadow-xl lg:hidden"
                    @click="drawerOpen = !drawerOpen"
                  >
                    <NavIcon :name="drawerOpen ? 'close' : 'chevron'" :size="14" />
                    {{ drawerOpen ? t2('shell.close', 'Close') : t2('shell.details', 'Details') }}
                  </button>
                </div>
              </div>

              <!-- Immediate background for login modal (no transition delay) -->
              <div v-if="loginOverlay" class="fixed inset-0 z-[110]">
                <div class="absolute inset-0 bg-dark/85"></div>
              </div>
              <LoginModal />
              <OfflineOverlay />
              <transition name="fade-fast">
                <div v-if="loggedOut" class="fixed inset-0 z-[120] flex flex-col">
                  <div class="absolute inset-0 bg-dark/90"></div>
                  <div
                    class="relative flex flex-1 flex-col items-center justify-center overflow-y-auto p-6"
                  >
                    <div class="mx-auto w-full max-w-md space-y-6 text-center">
                      <img
                        src="/images/logo-luminalshine.png"
                        alt="LuminalShine"
                        class="mx-auto h-24 w-24 select-none opacity-80"
                      />
                      <div class="space-y-2">
                        <h2 class="text-2xl font-semibold tracking-tight">
                          {{ $t('auth.logout_success') }}
                        </h2>
                        <p class="text-sm leading-relaxed opacity-80">
                          {{ $t('auth.logout_refresh_hint') }}
                        </p>
                      </div>
                      <div class="flex items-center justify-center pt-2">
                        <n-button type="primary" @click="refreshPage">
                          {{ $t('auth.logout_refresh_button') }}
                        </n-button>
                      </div>
                      <p class="mt-8 select-none text-[10px] uppercase tracking-wider opacity-60">
                        LuminalShine
                      </p>
                    </div>
                  </div>
                </div>
              </transition>
            </div>
          </n-message-provider>
        </n-notification-provider>
      </n-dialog-provider>
    </n-loading-bar-provider>
  </n-config-provider>
</template>

<script setup lang="ts">
import { ref, computed, watch } from 'vue';
import {
  NConfigProvider,
  NDialogProvider,
  NMessageProvider,
  NNotificationProvider,
  NLoadingBarProvider,
  NButton,
  NDropdown,
  darkTheme,
} from 'naive-ui';
import { useNaiveThemeOverrides } from '@/naive-theme';
import { useRoute, useRouter } from 'vue-router';
import type { RouteLocationNormalizedLoaded } from 'vue-router';
import { useT2 } from '@/composables/useT2';
import SavingStatus from '@/components/SavingStatus.vue';
import LoginModal from '@/components/LoginModal.vue';
import OfflineOverlay from '@/components/OfflineOverlay.vue';
import HostStrip from '@/components/shell/HostStrip.vue';
import NavIcon from '@/components/shell/NavIcon.vue';
import { INSPECTOR_TARGET_ID, useInspector } from '@/composables/useInspector';
import { http } from '@/http';
import { useAuthStore } from './stores/auth';
import { useConnectivityStore } from '@/stores/connectivity';
import { useHostStore } from '@/stores/host';

const naiveOverrides = useNaiveThemeOverrides();
const route = useRoute();
const router = useRouter();
const t2 = useT2();

const auth = useAuthStore();
const host = useHostStore();
const { open: inspectorOpen } = useInspector();
// Narrow-screen drawer for the inspector; closes on navigation.
const drawerOpen = ref(false);
watch(
  () => route.fullPath,
  () => {
    drawerOpen.value = false;
  },
);

// ---- navigation -----------------------------------------------------------
interface NavItem {
  path: string;
  icon: string;
  label: string;
}

const statsOnly = computed(() => auth.isStatsOnly());

const navItems = computed<NavItem[]>(() => {
  if (statsOnly.value) {
    return [{ path: '/stream', icon: 'stream', label: t2('shell.nav_stream', 'Stream') }];
  }
  return [
    { path: '/', icon: 'overview', label: t2('shell.nav_overview', 'Overview') },
    { path: '/stream', icon: 'stream', label: t2('shell.nav_stream', 'Stream') },
    { path: '/webrtc', icon: 'play', label: t2('shell.nav_play', 'Play') },
    { path: '/library', icon: 'library', label: t2('shell.nav_library', 'Library') },
    { path: '/clients', icon: 'clients', label: t2('shell.nav_clients', 'Clients') },
    { path: '/display', icon: 'display', label: t2('shell.nav_display', 'Display') },
    { path: '/settings', icon: 'settings', label: t2('shell.nav_settings', 'Settings') },
    {
      path: '/diagnostics',
      icon: 'diagnostics',
      label: t2('shell.nav_diagnostics', 'Diagnostics'),
    },
  ];
});

function isActive(path: string): boolean {
  return route.path === path;
}

const pageTitle = computed(() => {
  const key = route.meta?.['title'];
  if (typeof key === 'string' && key) return t2(key, key.split('.').pop() || '');
  return navItems.value.find((i) => i.path === route.path)?.label ?? '';
});

const mobileMenuOptions = computed(() => [
  ...navItems.value.map((i) => ({ label: i.label, key: i.path })),
  { type: 'divider' as const, key: 'divider' },
  { label: t2('navbar.logout', 'Log out'), key: '__logout' },
]);

function onMobileSelect(key: string | number): void {
  if (key === '__logout') {
    void logout();
    return;
  }
  if (typeof key === 'string') void router.push(key);
}

// ---- auth / overlays -------------------------------------------------------
const loggedOut = ref(false);
const loginOverlay = computed(
  () => auth.ready && auth.showLoginModal && !auth.isAuthenticated && !auth.logoutInitiated,
);

async function logout() {
  const connectivity = useConnectivityStore();
  try {
    await http.post('/api/auth/logout', {}, { validateStatus: () => true });
  } catch (e) {
    console.error('Logout failed:', e);
  }
  try {
    auth.logoutInitiated = true;
  } catch {
    /* best effort during logout */
  }
  try {
    auth.setAuthenticated(false);
  } catch {
    /* best effort during logout */
  }
  try {
    connectivity.stop();
    host.stop();
  } catch {
    /* best effort during logout */
  }
  loggedOut.value = true;
}

function refreshPage() {
  window.location.reload();
}

// ---- content container ----------------------------------------------------
// Route meta `container` picks the content wrapper:
//   flush — no padding; the page lays itself out (Overview)
//   full  — full width with the standard 16px gutter (Stream, Play)
//   lg/xl — centered, max-width; used by pages not yet rebuilt for the shell
const sizes: Record<string, string> = {
  flush: '',
  full: 'p-4',
  lg: 'mx-auto w-full max-w-5xl px-4 py-4 sm:px-6',
  xl: 'mx-auto w-full max-w-7xl px-4 py-4 sm:px-6',
};
function containerClass(r: RouteLocationNormalizedLoaded): string {
  const size = typeof r?.meta?.['container'] === 'string' ? r.meta['container'] : 'lg';
  return sizes[size] ?? sizes['lg'] ?? '';
}
</script>

<style scoped>
.rail-btn {
  width: 40px;
  height: 40px;
  border-radius: 10px;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #8b9096;
  transition:
    background-color 0.15s ease,
    color 0.15s ease;
}
.rail-btn:hover {
  color: var(--mc-ink);
  background: rgba(255, 255, 255, 0.04);
}
.rail-btn-on,
.rail-btn-on:hover {
  background: rgba(255, 176, 32, 0.14);
  color: var(--mc-gold);
}
.fade-fast-enter-active,
.fade-fast-leave-active {
  transition: opacity 0.12s ease;
}
.fade-fast-enter-from,
.fade-fast-leave-to {
  opacity: 0;
}
</style>
