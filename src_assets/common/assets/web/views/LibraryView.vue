<script setup lang="ts">
/**
 * Library — applications as a cover grid with source filters. Selecting a
 * card fills the inspector with the app's summary; editing hands off to the
 * existing editor (AppEditModal), which owns the full form.
 */
import { computed, defineAsyncComponent, onMounted, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import { NButton, NInput, useMessage } from 'naive-ui';
import { http } from '@/http';
import { useT2 } from '@/composables/useT2';
import { useAppsStore, type App } from '@/stores/apps';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import { useHostStore } from '@/stores/host';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import LinkButton from '@/components/shell/LinkButton.vue';
import NavIcon from '@/components/shell/NavIcon.vue';

const AppEditModal = defineAsyncComponent(() => import('@/components/AppEditModal.vue'));

const t2 = useT2();
const route = useRoute();
const message = useMessage();
const appsStore = useAppsStore();
const auth = useAuthStore();
const configStore = useConfigStore();
const host = useHostStore();

const isWindows = computed(
  () => String(configStore.metadata?.platform || '').toLowerCase() === 'windows',
);
const playnite = computed(() => host.playnite);
const playniteAvailable = computed(() => !!(playnite.value?.installed || playnite.value?.active));

// ---- list + filters -------------------------------------------------------
type Source = 'all' | 'playnite' | 'steam' | 'custom';
const filter = ref<Source>('all');
const query = ref(typeof route.query['q'] === 'string' ? String(route.query['q']) : '');
watch(
  () => route.query['q'],
  (q) => {
    if (typeof q === 'string') query.value = q;
  },
);

function sourceOf(app: App): Exclude<Source, 'all'> {
  if (typeof app['playnite-id'] === 'string' && app['playnite-id']) return 'playnite';
  const cmd = Array.isArray(app.cmd) ? app.cmd.join(' ') : String(app.cmd ?? '');
  if (/steam/i.test(cmd)) return 'steam';
  return 'custom';
}
function sourceLabel(app: App): string {
  const s = sourceOf(app);
  if (s === 'playnite') {
    if (app['playnite-managed'] === 'manual')
      return t2('overview.app_playnite_manual', 'Playnite · manual');
    const src = app['playnite-source'];
    if (typeof src === 'string' && src) return `Playnite · ${src.split('+').join(' + ')}`;
    return t2('overview.app_playnite_managed', 'Playnite · managed');
  }
  return s === 'steam' ? 'Steam' : t2('overview.app_custom', 'Custom');
}

interface Row {
  app: App;
  index: number;
  key: string;
}
const rows = computed<Row[]>(() =>
  (appsStore.apps || []).map((app, index) => ({
    app,
    index,
    key: `${app.uuid || ''}|${app.name || ''}|${index}`,
  })),
);
const visible = computed(() => {
  const q = query.value.trim().toLowerCase();
  return rows.value.filter((r) => {
    if (filter.value !== 'all' && sourceOf(r.app) !== filter.value) return false;
    if (
      q &&
      !String(r.app.name || '')
        .toLowerCase()
        .includes(q)
    )
      return false;
    return true;
  });
});
const counts = computed(() => {
  const c = { all: rows.value.length, playnite: 0, steam: 0, custom: 0 };
  for (const r of rows.value) c[sourceOf(r.app)] += 1;
  return c;
});

const runningName = computed(() => host.activeSession?.metadata?.application?.trim() || '');
function isRunning(app: App): boolean {
  return !!runningName.value && String(app.name || '').trim() === runningName.value;
}

// ---- covers ---------------------------------------------------------------
const coverFailed = ref(new Set<string>());
function hasCover(app: App): boolean {
  if (!app.uuid || coverFailed.value.has(app.uuid)) return false;
  return !!(app['image-path'] || app['playnite-id']);
}
function coverUrl(app: App): string {
  return app.uuid ? `/api/apps/${encodeURIComponent(app.uuid)}/cover` : '';
}
function markCoverFailed(app: App): void {
  if (!app.uuid) return;
  const next = new Set(coverFailed.value);
  next.add(app.uuid);
  coverFailed.value = next;
}
function initials(app: App): string {
  const words = String(app.name || '?')
    .replace(/[^\p{L}\p{N} ]/gu, ' ')
    .split(/\s+/)
    .filter(Boolean);
  const letters = words.slice(0, 3).map((w) => w[0]?.toUpperCase() ?? '');
  return letters.join('') || '?';
}

// ---- selection + inspector ----------------------------------------------
const selectedKey = ref<string | null>(null);
const selected = computed(() => rows.value.find((r) => r.key === selectedKey.value) ?? null);
function select(row: Row): void {
  selectedKey.value = row.key;
}
function prepCount(app: App): number {
  return Array.isArray(app['prep-cmd']) ? app['prep-cmd'].length : 0;
}
function overrideCount(app: App): number {
  const o = app['config-overrides'];
  return o && typeof o === 'object' ? Object.keys(o).length : 0;
}
function cmdText(app: App): string {
  return Array.isArray(app.cmd) ? app.cmd.join(' ') : String(app.cmd ?? '');
}
const summaryRows = computed<Array<[string, string]>>(() => {
  const app = selected.value?.app;
  if (!app) return [];
  const rows: Array<[string, string]> = [];
  const push = (k: string, v: unknown) => {
    if (v == null || v === '') return;
    rows.push([k, String(v)]);
  };
  push(t2('library.f_source', 'Source'), sourceLabel(app));
  push(t2('library.f_command', 'Command'), cmdText(app));
  push(t2('library.f_workdir', 'Working directory'), app['working-dir']);
  push(t2('library.f_output', 'Output'), app.output);
  push(t2('library.f_prep', 'Prep commands'), prepCount(app));
  push(t2('library.f_overrides', 'Config overrides'), overrideCount(app));
  push(
    t2('library.f_elevated', 'Run as admin'),
    app.elevated ? t2('stream.on', 'on') : t2('stream.off', 'off'),
  );
  if (typeof app['exit-timeout'] === 'number')
    push(t2('library.f_exit', 'Exit timeout'), `${app['exit-timeout']} s`);
  if (app['playnite-id']) push('Playnite ID', app['playnite-id']);
  return rows;
});

// ---- editor (existing modal) ---------------------------------------------
const showModal = ref(false);
const modalApp = ref<App | null>(null);
const modalIndex = ref(-1);
function openAdd(): void {
  modalApp.value = null;
  modalIndex.value = -1;
  showModal.value = true;
}
function openEdit(row: Row): void {
  modalApp.value = row.app;
  modalIndex.value = row.index;
  showModal.value = true;
}
async function reload(): Promise<void> {
  try {
    await appsStore.loadApps(true);
  } catch {
    appsStore.setApps([]);
  }
  if (selectedKey.value && !rows.value.some((r) => r.key === selectedKey.value)) {
    selectedKey.value = null;
  }
}
async function onSaved(): Promise<void> {
  await reload();
}
async function onDeleted(): Promise<void> {
  selectedKey.value = null;
  await reload();
}

// ---- Playnite -------------------------------------------------------------
const syncing = ref(false);
async function forceSync(): Promise<void> {
  if (syncing.value) return;
  syncing.value = true;
  try {
    const r = await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    if (r.status >= 200 && r.status < 300) {
      message.success(t2('library.sync_done', 'Playnite sync requested.'));
      await reload();
      await host.refreshPlaynite();
    } else {
      message.error(`${t2('library.sync_failed', 'Sync failed')} (HTTP ${r.status}).`);
    }
  } catch (e) {
    message.error(e instanceof Error ? e.message : t2('library.sync_failed', 'Sync failed'));
  } finally {
    syncing.value = false;
  }
}

onMounted(async () => {
  await auth.waitForAuthentication();
  if (!host.running) void host.start();
  await reload();
});
</script>

<template>
  <div class="flex flex-col gap-3.5">
    <div class="flex flex-wrap items-center gap-3">
      <div>
        <div class="mc-page-title">{{ t2('shell.nav_library', 'Library') }}</div>
        <div class="mc-page-sub">
          {{ counts.all }} {{ t2('library.apps', 'apps')
          }}<span v-if="isWindows && playnite">
            ·
            {{
              playniteAvailable
                ? t2('library.playnite_connected', 'Playnite connected')
                : t2('library.playnite_absent', 'Playnite not installed')
            }}</span
          >
        </div>
      </div>
      <div class="flex-1"></div>
      <NInput
        v-model:value="query"
        size="small"
        clearable
        :placeholder="t2('library.filter', 'Filter apps')"
        class="!w-48"
      />
      <div class="mc-chips" role="tablist">
        <button
          v-for="f in ['all', 'playnite', 'steam', 'custom'] as const"
          :key="f"
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': filter === f }"
          role="tab"
          :aria-selected="filter === f"
          @click="filter = f"
        >
          {{
            f === 'all'
              ? t2('_common.all', 'All')
              : f === 'playnite'
                ? 'Playnite'
                : f === 'steam'
                  ? 'Steam'
                  : t2('overview.app_custom', 'Custom')
          }}
          <span class="ml-1 text-ink-4">{{ counts[f] }}</span>
        </button>
      </div>
      <NButton
        v-if="isWindows && playniteAvailable"
        size="small"
        :loading="syncing"
        @click="forceSync"
      >
        <NavIcon name="sync" :size="14" />{{ t2('library.force_sync', 'Force sync') }}
      </NButton>
      <NButton size="small" type="primary" @click="openAdd"
        ><NavIcon name="plus" :size="14" />{{ t2('_common.add', 'Add') }}</NButton
      >
    </div>

    <div v-if="visible.length === 0" class="mc-panel px-4 py-6 text-center text-xs text-ink-4">
      {{
        rows.length === 0
          ? t2('overview.no_apps', 'No applications configured.')
          : t2('library.no_match', 'No apps match this filter.')
      }}
    </div>
    <div v-else class="grid grid-cols-2 gap-3.5 md:grid-cols-3 xl:grid-cols-4">
      <div
        v-for="r in visible"
        :key="r.key"
        class="mc-panel cursor-pointer gap-2 p-2.5 transition-colors"
        :class="
          selectedKey === r.key
            ? 'border-primary/60 shadow-[0_0_0_1px_rgba(255,176,32,0.25)]'
            : 'hover:border-line-strong'
        "
        role="button"
        tabindex="0"
        @click="select(r)"
        @dblclick="openEdit(r)"
        @keydown.enter.prevent="select(r)"
      >
        <div
          class="relative flex aspect-video items-center justify-center overflow-hidden rounded-lg border border-line bg-[linear-gradient(160deg,#1D2229_0%,#14171B_100%)]"
        >
          <img
            v-if="hasCover(r.app)"
            :src="coverUrl(r.app)"
            :alt="r.app.name || ''"
            class="h-full w-full object-cover"
            loading="lazy"
            @error="markCoverFailed(r.app)"
          />
          <span v-else class="text-2xl font-semibold tracking-wider text-[#3A4048]">{{
            initials(r.app)
          }}</span>
        </div>
        <div class="flex items-center justify-between gap-2">
          <div class="min-w-0">
            <div class="truncate text-[12.5px] font-medium">
              {{ r.app.name || t2('library.untitled', '(untitled)') }}
            </div>
            <div class="truncate text-[11px] text-ink-3">{{ sourceLabel(r.app) }}</div>
          </div>
          <span class="mc-tag" :class="isRunning(r.app) ? 'mc-tag-live' : ''">{{
            isRunning(r.app) ? t2('overview.running', 'Running') : t2('overview.ready', 'Ready')
          }}</span>
        </div>
      </div>
    </div>

    <section v-if="isWindows && playnite" class="mc-panel">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{
          t2('library.playnite_title', 'Playnite integration')
        }}</span>
        <RouterLink :to="{ path: '/settings', query: { sec: 'playnite' } }" class="mc-panel-link"
          >{{ t2('shell.nav_settings', 'Settings') }} · Playnite</RouterLink
        >
      </div>
      <div class="grid grid-cols-1 gap-3 px-4 pb-3.5 pt-3 sm:grid-cols-3">
        <div class="flex items-center gap-2.5">
          <span
            class="mc-dot"
            :class="
              playnite.installed ? (playnite.update_available ? 'mc-dot-gold' : '') : 'mc-dot-grey'
            "
          ></span>
          <div>
            <div class="text-[12.5px] font-medium">
              {{ t2('library.playnite_extension', 'Extension') }}
            </div>
            <div class="text-[11px] text-ink-3">
              {{
                playnite.installed
                  ? `${playnite.installed_version || '?'} ${t2('library.installed', 'installed')}`
                  : t2('library.not_installed', 'not installed')
              }}<span v-if="playnite.update_available">
                · {{ playnite.packaged_version || '?' }}
                {{ t2('library.packaged', 'packaged') }}</span
              >
            </div>
          </div>
        </div>
        <div class="flex items-center gap-2.5">
          <span class="mc-dot" :class="playnite.active ? '' : 'mc-dot-grey'"></span>
          <div>
            <div class="text-[12.5px] font-medium">
              {{
                playnite.active
                  ? t2('library.playnite_running', 'Playnite running')
                  : t2('library.playnite_stopped', 'Playnite not running')
              }}
            </div>
            <div class="text-[11px] text-ink-3">
              {{
                playnite.active
                  ? t2('library.sync_active', 'sync active')
                  : t2('library.sync_paused', 'launch Playnite to resume syncing')
              }}
            </div>
          </div>
        </div>
        <div class="flex items-center gap-2.5">
          <span class="mc-dot" :class="counts.playnite > 0 ? '' : 'mc-dot-grey'"></span>
          <div>
            <div class="text-[12.5px] font-medium">
              {{ counts.playnite }} {{ t2('library.playnite_games', 'games from Playnite') }}
            </div>
            <div class="text-[11px] text-ink-3">
              {{ counts.all - counts.playnite }} {{ t2('library.other_apps', 'other apps') }}
            </div>
          </div>
        </div>
      </div>
    </section>

    <AppEditModal
      :key="`${modalIndex}|${modalApp?.uuid || modalApp?.name || 'new'}`"
      v-model="showModal"
      :app="modalApp"
      :index="modalIndex"
      @saved="onSaved"
      @deleted="onDeleted"
    />

    <InspectorPanel
      v-if="selected"
      :title="selected.app.name || t2('library.untitled', '(untitled)')"
      :subtitle="`${sourceLabel(selected.app)}${selected.app.uuid ? ` · ${selected.app.uuid.slice(0, 8)}…` : ''}`"
      :tag="
        isRunning(selected.app) ? t2('overview.running', 'Running') : t2('overview.ready', 'Ready')
      "
      :tag-kind="isRunning(selected.app) ? 'live' : ''"
    >
      <div v-if="hasCover(selected.app)" class="px-[18px] pt-3">
        <img
          :src="coverUrl(selected.app)"
          :alt="selected.app.name || ''"
          class="aspect-video w-full rounded-lg border border-line object-cover"
          @error="markCoverFailed(selected.app)"
        />
      </div>
      <div
        class="grid grid-cols-[118px_minmax(0,1fr)] gap-x-3 gap-y-1.5 px-[18px] pb-2 pt-3 text-xs"
      >
        <template v-for="[k, v] in summaryRows" :key="k">
          <span class="text-ink-3">{{ k }}</span>
          <span
            class="break-all text-ink"
            :class="
              k === t2('library.f_command', 'Command') ||
              k === t2('library.f_workdir', 'Working directory')
                ? 'font-mono text-[11.5px]'
                : ''
            "
            >{{ v }}</span
          >
        </template>
      </div>
      <div class="px-[18px] pb-3 text-[11px] text-ink-4">
        {{
          t2(
            'library.inspector_hint',
            'Launch, prep commands, display overrides and frame generation are edited in the app editor.',
          )
        }}
      </div>
      <template #footer>
        <LinkButton to="/webrtc" size="small"
          ><NavIcon name="play" :size="14" />{{ t2('shell.nav_play', 'Play') }}</LinkButton
        >
        <NButton size="small" type="primary" @click="openEdit(selected)">{{
          t2('library.edit', 'Edit')
        }}</NButton>
      </template>
    </InspectorPanel>
  </div>
</template>
