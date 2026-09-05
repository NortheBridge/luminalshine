<script setup lang="ts">
/**
 * Global search for the host strip: type to jump to a page, an app, a client
 * or a settings section; Enter on free text opens Settings with that query.
 */
import { computed, nextTick, ref } from 'vue';
import { useRouter } from 'vue-router';
import { useT2 } from '@/composables/useT2';
import { useAppsStore } from '@/stores/apps';
import { useHostStore } from '@/stores/host';
import { useAuthStore } from '@/stores/auth';
import NavIcon from '@/components/shell/NavIcon.vue';

interface Hit {
  id: string;
  kind: 'page' | 'app' | 'client' | 'settings';
  label: string;
  detail: string;
  to: { path: string; query?: Record<string, string> };
}

const t2 = useT2();
const router = useRouter();
const apps = useAppsStore();
const host = useHostStore();
const auth = useAuthStore();

const query = ref('');
const open = ref(false);
const active = ref(0);
const input = ref<HTMLInputElement | null>(null);

const PAGES = computed<Hit[]>(() => [
  {
    id: 'p-overview',
    kind: 'page',
    label: t2('shell.nav_overview', 'Overview'),
    detail: '/',
    to: { path: '/' },
  },
  {
    id: 'p-stream',
    kind: 'page',
    label: t2('shell.nav_stream', 'Stream'),
    detail: '/stream',
    to: { path: '/stream' },
  },
  {
    id: 'p-play',
    kind: 'page',
    label: t2('shell.nav_play', 'Play'),
    detail: '/webrtc',
    to: { path: '/webrtc' },
  },
  {
    id: 'p-library',
    kind: 'page',
    label: t2('shell.nav_library', 'Library'),
    detail: '/library',
    to: { path: '/library' },
  },
  {
    id: 'p-clients',
    kind: 'page',
    label: t2('shell.nav_clients', 'Clients'),
    detail: '/clients',
    to: { path: '/clients' },
  },
  {
    id: 'p-display',
    kind: 'page',
    label: t2('shell.nav_display', 'Display'),
    detail: '/display',
    to: { path: '/display' },
  },
  {
    id: 'p-settings',
    kind: 'page',
    label: t2('shell.nav_settings', 'Settings'),
    detail: '/settings',
    to: { path: '/settings' },
  },
  {
    id: 'p-diag',
    kind: 'page',
    label: t2('shell.nav_diagnostics', 'Diagnostics'),
    detail: '/diagnostics',
    to: { path: '/diagnostics' },
  },
  {
    id: 'p-tokens',
    kind: 'page',
    label: t2('navbar.api_tokens', 'API Tokens'),
    detail: '/clients?sec=tokens',
    to: { path: '/clients', query: { sec: 'tokens' } },
  },
]);

const SETTINGS_SECTIONS: Array<[string, string]> = [
  ['general', 'General'],
  ['input', 'Input'],
  ['av', 'Audio / Video'],
  ['capture', 'Capture'],
  ['network', 'Network'],
  ['files', 'Files'],
  ['advanced', 'Advanced'],
  ['steamlibrary', 'Steam Library'],
  ['playnite', 'Playnite'],
];

const hits = computed<Hit[]>(() => {
  const q = query.value.trim().toLowerCase();
  if (!q) return [];
  const out: Hit[] = [];
  const match = (s: string) => s.toLowerCase().includes(q);
  for (const p of PAGES.value) if (match(p.label) || match(p.detail)) out.push(p);
  if (!auth.isStatsOnly()) {
    for (const [id, name] of SETTINGS_SECTIONS) {
      if (match(name)) {
        out.push({
          id: `s-${id}`,
          kind: 'settings',
          label: name,
          detail: t2('shell.nav_settings', 'Settings'),
          to: { path: '/settings', query: { sec: id } },
        });
      }
    }
    for (const app of apps.apps || []) {
      const name = String(app.name || '');
      if (name && match(name)) {
        out.push({
          id: `a-${app.uuid || name}`,
          kind: 'app',
          label: name,
          detail: t2('shell.nav_library', 'Library'),
          to: { path: '/library', query: { q: name } },
        });
      }
    }
    for (const c of host.clients) {
      if (c.name && match(c.name)) {
        out.push({
          id: `c-${c.uuid}`,
          kind: 'client',
          label: c.name,
          detail: c.connected
            ? t2('overview.connected', 'Connected')
            : t2('shell.nav_clients', 'Clients'),
          to: { path: '/clients', query: { id: c.uuid } },
        });
      }
    }
    // Free-text fallback: search the settings pages for the words.
    out.push({
      id: 'free',
      kind: 'settings',
      label: `${t2('shell.search_settings_for', 'Search settings for')} “${query.value.trim()}”`,
      detail: t2('shell.nav_settings', 'Settings'),
      to: { path: '/settings', query: { jump: query.value.trim() } },
    });
  }
  return out.slice(0, 9);
});

function iconFor(kind: Hit['kind']): string {
  return kind === 'app'
    ? 'library'
    : kind === 'client'
      ? 'clients'
      : kind === 'settings'
        ? 'settings'
        : 'overview';
}

function choose(hit?: Hit): void {
  if (!hit) return;
  open.value = false;
  query.value = '';
  void router.push(hit.to);
  input.value?.blur();
}
function onKey(e: KeyboardEvent): void {
  if (e.key === 'ArrowDown') {
    e.preventDefault();
    active.value = Math.min(hits.value.length - 1, active.value + 1);
  } else if (e.key === 'ArrowUp') {
    e.preventDefault();
    active.value = Math.max(0, active.value - 1);
  } else if (e.key === 'Enter') {
    e.preventDefault();
    choose(hits.value[active.value] ?? hits.value[0]);
  } else if (e.key === 'Escape') {
    open.value = false;
    input.value?.blur();
  }
}
function onInput(): void {
  open.value = true;
  active.value = 0;
}
function onBlur(): void {
  // Let a click on a result land before the list closes.
  setTimeout(() => (open.value = false), 120);
}
function focus(): void {
  void nextTick(() => input.value?.focus());
}
defineExpose({ focus });
</script>

<template>
  <div class="relative">
    <label
      class="flex h-8 w-[220px] items-center gap-2 rounded-lg border border-line-strong bg-dark px-2.5 text-xs text-ink-4 focus-within:border-primary"
    >
      <NavIcon name="search" :size="14" />
      <input
        ref="input"
        v-model="query"
        type="search"
        role="combobox"
        aria-autocomplete="list"
        aria-controls="mc-global-search-list"
        :aria-expanded="open && hits.length > 0"
        class="min-w-0 flex-1 bg-transparent text-xs text-ink outline-none placeholder:text-ink-4"
        :placeholder="t2('shell.search_placeholder', 'Search settings, apps, clients…')"
        :aria-label="t2('shell.search_placeholder', 'Search settings, apps, clients…')"
        autocomplete="off"
        @input="onInput"
        @focus="open = !!query"
        @blur="onBlur"
        @keydown="onKey"
      />
    </label>
    <div
      v-if="open && hits.length"
      id="mc-global-search-list"
      class="mc-panel absolute right-0 top-9 z-[60] w-[320px] overflow-hidden shadow-xl"
      role="listbox"
    >
      <button
        v-for="(h, i) in hits"
        :key="h.id"
        type="button"
        class="mc-row mc-row-clickable w-full text-left"
        :class="{ 'mc-row-selected': i === active }"
        role="option"
        :aria-selected="i === active"
        @mousedown.prevent="choose(h)"
        @mouseenter="active = i"
      >
        <span class="flex min-w-0 items-center gap-2.5">
          <NavIcon :name="iconFor(h.kind)" :size="14" class="text-ink-4" />
          <span class="mc-row-primary truncate">{{ h.label }}</span>
        </span>
        <span class="mc-row-secondary shrink-0">{{ h.detail }}</span>
      </button>
    </div>
  </div>
</template>
