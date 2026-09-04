<script setup lang="ts">
/**
 * Overview — the single pane. Everything the old Dashboard, Clients,
 * Applications and Troubleshooting pages asked you to visit for is visible
 * here at once: the live (or last) session, host health, clients,
 * applications, recent sessions and the event tail. Stream settings for
 * the next session sit in the inspector.
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import { storeToRefs } from 'pinia';
import { NAlert, NButton, useDialog, useMessage } from 'naive-ui';
import { useHostStore, type SessionSummary } from '@/stores/host';
import type { App } from '@/stores/apps';
import { useConfigStore } from '@/stores/config';
import { useAppsStore } from '@/stores/apps';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import { useT2 } from '@/composables/useT2';
import { useUpdateCheck } from '@/composables/useUpdateCheck';
import { useCrashDump } from '@/composables/useCrashDump';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import NavIcon from '@/components/shell/NavIcon.vue';
import LinkButton from '@/components/shell/LinkButton.vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import HighPerformanceCard from '@/components/HighPerformanceCard.vue';
import PlayniteReinstallButton from '@/components/PlayniteReinstallButton.vue';

const t2 = useT2();
const host = useHostStore();
const router = useRouter();
const configStore = useConfigStore();
const appsStore = useAppsStore();
const auth = useAuthStore();
const message = useMessage();
const dialog = useDialog();
const { config, metadata } = storeToRefs(configStore);
const update = useUpdateCheck();
const crash = useCrashDump();

// ---- clock (for live durations / relative times) ---------------------------
const nowSec = ref(Math.floor(Date.now() / 1000));
let clock: ReturnType<typeof setInterval> | null = null;

// ---- live session ----------------------------------------------------------
interface LiveMetrics {
  fps?: number | undefined;
  mbps?: number | undefined;
  encodeMs?: number | undefined;
  losses?: number | undefined;
  targetMbps?: number | undefined;
  hdr?: boolean | undefined;
}
const live = ref<LiveMetrics>({});
let liveTimer: ReturnType<typeof setInterval> | null = null;

const activeSession = computed(() => host.activeSession);

async function refreshLiveMetrics(): Promise<void> {
  const s = host.activeSession;
  if (!s) {
    live.value = {};
    return;
  }
  const to = Math.floor(Date.now() / 1000);
  const from = to - 60;
  try {
    const r = await http.get(`./api/sessions/${s.id}?from=${from}&to=${to}`, {
      validateStatus: () => true,
    });
    if (r.status < 200 || r.status >= 300 || !r.data) return;
    const data = r.data as {
      series?: Record<string, Array<[number, number]>>;
      metadata?: Record<string, unknown>;
    };
    const series = data.series ?? {};
    const last = (key: string): number | undefined => {
      const arr = series[key];
      if (!Array.isArray(arr) || arr.length === 0) return undefined;
      const v = Number(arr[arr.length - 1]?.[1]);
      return Number.isFinite(v) ? v : undefined;
    };
    const meta = data.metadata ?? {};
    live.value = {
      fps: last('actual_fps'),
      mbps: last('network_throughput_mbps'),
      encodeMs: last('encode_latency_ms'),
      losses: last('client_losses'),
      targetMbps:
        typeof meta['bitrate_mbps_target'] === 'number' ? meta['bitrate_mbps_target'] : undefined,
      hdr: meta['hdr'] === true || meta['hdr'] === 'true',
    };
  } catch {
    /* keep last values */
  }
}

watch(
  () => activeSession.value?.id ?? null,
  (id) => {
    if (liveTimer) clearInterval(liveTimer);
    liveTimer = null;
    if (id) {
      void refreshLiveMetrics();
      liveTimer = setInterval(() => void refreshLiveMetrics(), 5000);
    } else {
      live.value = {};
    }
  },
  { immediate: true },
);

function fmt1(v?: number): string {
  return typeof v === 'number' ? (Number.isInteger(v) ? String(v) : v.toFixed(1)) : '—';
}
function fmt0(v?: number): string {
  return typeof v === 'number' ? String(Math.round(v)) : '—';
}

function fmtDuration(s: SessionSummary): string {
  const end = s.stream_ended_at ?? nowSec.value;
  const secs = Math.max(0, end - s.started_at);
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  const sec = secs % 60;
  if (h > 0) return `${h}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
  return `${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
}
function fmtDurationShort(s: SessionSummary): string {
  const end = s.stream_ended_at ?? nowSec.value;
  const secs = Math.max(0, end - s.started_at);
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  if (h > 0) return `${h} h ${m} min`;
  return `${m} min`;
}
function fmtRelative(epochSec: number | null): string {
  if (!epochSec) return t2('overview.never', 'never');
  const diff = nowSec.value - epochSec;
  if (diff < 60) return t2('overview.just_now', 'just now');
  const min = Math.floor(diff / 60);
  if (min < 60) return `${min} min ${t2('overview.ago', 'ago')}`;
  const h = Math.floor(min / 60);
  if (h < 48) return `${h} h ${t2('overview.ago', 'ago')}`;
  const d = Math.floor(h / 24);
  return `${d} d ${t2('overview.ago', 'ago')}`;
}
function fmtClock(epochSec: number): string {
  const d = new Date(epochSec * 1000);
  const today = new Date();
  const sameDay =
    d.getFullYear() === today.getFullYear() &&
    d.getMonth() === today.getMonth() &&
    d.getDate() === today.getDate();
  if (sameDay) return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return d.toLocaleDateString([], { month: 'short', day: 'numeric' });
}
function sessionApp(s: SessionSummary): string {
  return s.metadata?.application?.trim() || t2('overview.streaming', 'Streaming');
}
function sessionClient(s: SessionSummary): string {
  return s.metadata?.client_name?.trim() || t2('overview.unknown_client', 'unknown client');
}
function sessionFormat(s: SessionSummary): string {
  const m = s.metadata ?? {};
  const parts: string[] = [];
  if (m.codec) parts.push(m.codec);
  if (m.resolution_w && m.resolution_h) {
    parts.push(`${m.resolution_w}×${m.resolution_h}${m.fps ? ` @${m.fps}` : ''}`);
  }
  return parts.join(' · ');
}
function sessionSub(s: SessionSummary): string {
  const parts = [sessionFormat(s), fmtDurationShort(s)];
  if (s.stream_ended_at != null) parts.push(fmtClock(s.started_at));
  return parts.filter(Boolean).join(' · ');
}

// ---- panels ----------------------------------------------------------------
const lastSession = computed(() => host.lastSession);
const recentSessions = computed(() => host.sessions.slice(0, 3));
const clients = computed(() => host.clients.slice(0, 4));

const apps = computed(() => (appsStore.apps || []).slice(0, 5));
function appSource(app: App): string {
  if (typeof app['playnite-id'] === 'string' && app['playnite-id']) {
    return app['playnite-managed'] === 'auto'
      ? t2('overview.app_playnite_managed', 'Playnite · managed')
      : t2('overview.app_playnite_manual', 'Playnite · manual');
  }
  const cmd = Array.isArray(app.cmd) ? app.cmd.join(' ') : String(app.cmd ?? '');
  if (/steam/i.test(cmd)) return 'Steam';
  return t2('overview.app_custom', 'Custom');
}
function appRunning(app: App): boolean {
  const running = activeSession.value?.metadata?.application?.trim();
  return !!running && typeof app.name === 'string' && app.name.trim() === running;
}

const events = computed(() => host.logLines.slice(-6).reverse());
function levelClass(level: string): string {
  const l = level.toLowerCase();
  if (l === 'warning' || l === 'warn') return 'mc-log-warn';
  if (l === 'error' || l === 'fatal') return 'mc-log-error';
  return 'text-ink-4';
}

// Installed version with its branch / short commit, and a pre-release marker
// when GitHub says the installed tag is one (the old Dashboard's info alert).
const versionLabel = computed(() => {
  const parts = [update.displayVersion.value];
  const branch = update.branch.value;
  if (branch && !['main', 'master'].includes(branch)) parts.push(branch);
  if (update.commit.value) parts.push(update.commit.value.slice(0, 7));
  if (update.installedVersionNotStable.value) parts.push(t2('overview.prerelease', 'pre-release'));
  return parts.join(' · ');
});

// Health list = host store checks + the update row.
const healthRows = computed(() => {
  const rows = host.healthChecks.map((c) => ({ ...c }));
  if (!auth.isStatsOnly()) {
    rows.unshift({
      id: 'update',
      label: t2('overview.check_update', 'LuminalShine'),
      state: !update.checked.value
        ? ('unknown' as const)
        : update.updateAvailable.value
          ? ('warn' as const)
          : ('ok' as const),
      detail: !update.checked.value
        ? t2('overview.checking', 'checking…')
        : update.updateAvailable.value
          ? `${t2('overview.update_available', 'update')} ${update.displayVersion.value} → ${
              update.stableBuildAvailable.value
                ? update.githubVersion.value.version
                : update.preReleaseVersion.value.version
            }`
          : update.remoteReachable.value
            ? `${versionLabel.value} · ${t2('overview.up_to_date', 'up to date')}`
            : `${versionLabel.value} · ${t2('overview.offline_check', 'update check offline')}`,
    });
  }
  return rows;
});
const passing = computed(() => healthRows.value.filter((r) => r.state === 'ok').length);
const dotClass = (state: string) =>
  state === 'ok'
    ? ''
    : state === 'warn'
      ? 'mc-dot-gold'
      : state === 'danger'
        ? 'mc-dot-red'
        : 'mc-dot-grey';

// ---- attention items (ported from the Dashboard banners) ------------------
const isWindows = computed(() => host.isWindows);
const playnite = computed(() => host.playnite);
const playniteUpdateAvailable = computed(
  () => !!(playnite.value && playnite.value.installed && playnite.value.update_available),
);

function isRecord(value: unknown): value is App {
  return !!value && typeof value === 'object';
}
function isPlayniteFullscreenEntry(app: App): boolean {
  if (app['playnite-fullscreen'] === true) return true;
  if (typeof app.name === 'string' && app.name === 'Playnite (Fullscreen)') return true;
  const cmdValue = app.cmd;
  const cmdText = Array.isArray(cmdValue)
    ? cmdValue.filter((v): v is string => typeof v === 'string').join(' ')
    : typeof cmdValue === 'string'
      ? cmdValue
      : '';
  const cmdLower = cmdText.toLowerCase();
  return cmdLower.includes('playnite-launcher') && cmdLower.includes('--fullscreen');
}
function isPlayniteApp(app: App): boolean {
  if (typeof app['playnite-id'] === 'string' && app['playnite-id'].length > 0) return true;
  return isPlayniteFullscreenEntry(app);
}
function getAppsSnapshot(): App[] {
  return (appsStore.apps || []).filter((app): app is App => isRecord(app));
}
const playniteAutoSyncedAppsCount = computed(
  () =>
    getAppsSnapshot().filter((app) => isPlayniteApp(app) && app['playnite-managed'] === 'auto')
      .length,
);
const hasPlayniteFullscreenApp = computed(() =>
  getAppsSnapshot().some((app) => isPlayniteFullscreenEntry(app)),
);
const showPlayniteMissingPluginBanner = computed(() => {
  if (!isWindows.value) return false;
  const p = playnite.value;
  if (!p || p.active === true || p.installed !== false) return false;
  return playniteAutoSyncedAppsCount.value > 0 || hasPlayniteFullscreenApp.value;
});
const playniteMissingPluginBannerText = computed(() => {
  const details: string[] = [];
  if (playniteAutoSyncedAppsCount.value > 0) {
    const count = playniteAutoSyncedAppsCount.value;
    details.push(`${count} auto-synced app${count === 1 ? '' : 's'}`);
  }
  if (hasPlayniteFullscreenApp.value) details.push('a Playnite (Fullscreen) launcher entry');
  const detected = details.length > 0 ? details.slice(0, 2).join(' and ') : 'Playnite entries';
  return `Detected ${detected}, but the Playnite plugin is no longer installed. Reinstall the plugin to restore integration, or purge Playnite games to remove all Playnite entries from LuminalShine.`;
});
interface ApiResult {
  status?: boolean;
  error?: string;
  message?: string;
}
function errorText(e: unknown): string {
  return e instanceof Error && e.message ? e.message : 'Request failed';
}
const resolvingPlaynitePluginIssue = ref(false);
const purgingPlayniteApps = ref(false);

async function refreshPlayniteAndApps() {
  await Promise.all([host.refreshPlaynite(), appsStore.loadApps(true).catch(() => [])]);
}
async function resolvePlaynitePluginIssue() {
  if (resolvingPlaynitePluginIssue.value || purgingPlayniteApps.value) return;
  resolvingPlaynitePluginIssue.value = true;
  try {
    const r = await http.post(
      '/api/playnite/install',
      { restart: true },
      { validateStatus: () => true },
    );
    const body = r.data as ApiResult | undefined;
    const ok = r.status >= 200 && r.status < 300 && body?.status === true;
    if (ok) {
      message.success('Playnite plugin reinstalled.');
      await refreshPlayniteAndApps();
    } else {
      const err = body?.error || body?.message || `HTTP ${r.status}`;
      message.error(`Failed to reinstall Playnite plugin: ${err}`);
    }
  } catch (e: unknown) {
    message.error(`Failed to reinstall Playnite plugin: ${errorText(e)}`);
  } finally {
    resolvingPlaynitePluginIssue.value = false;
  }
}
async function purgePlayniteGames() {
  if (purgingPlayniteApps.value || resolvingPlaynitePluginIssue.value) return;
  purgingPlayniteApps.value = true;
  try {
    const r = await http.get('/api/apps', { validateStatus: () => true });
    const list = (r.data as { apps?: unknown } | undefined)?.apps;
    if (r.status !== 200 || !Array.isArray(list)) throw new Error(`HTTP ${r.status}`);
    appsStore.setApps(list as App[]);
    const indexes = getAppsSnapshot()
      .map((app, index) => ({ app, index }))
      .filter((item) => isPlayniteApp(item.app))
      .map((item) => item.index)
      .sort((a, b) => b - a);
    if (!indexes.length) {
      message.info('No Playnite apps found to purge.');
      await refreshPlayniteAndApps();
      return;
    }
    for (const index of indexes) {
      const d = await http.delete(`./api/apps/${index}`, { validateStatus: () => true });
      const body = d.data as ApiResult | undefined;
      const ok = d.status >= 200 && d.status < 300 && body?.status === true;
      if (!ok) throw new Error(body?.error || body?.message || `HTTP ${d.status}`);
    }
    try {
      await configStore.fetchConfig(true);
    } catch {
      /* the config refresh is best effort */
    }
    await refreshPlayniteAndApps();
    message.success(`Removed ${indexes.length} Playnite app${indexes.length === 1 ? '' : 's'}.`);
  } catch (e: unknown) {
    message.error(`Failed to purge Playnite apps: ${errorText(e)}`);
    await refreshPlayniteAndApps();
  } finally {
    purgingPlayniteApps.value = false;
  }
}
function openPurgePlayniteGamesConfirm() {
  dialog.warning({
    title: 'Purge Playnite games?',
    content:
      'This removes all Playnite entries from LuminalShine, including auto-synced games and the Playnite (Fullscreen) launcher.',
    positiveText: 'Purge',
    negativeText: 'Cancel',
    onPositiveClick: async () => {
      await purgePlayniteGames();
    },
  });
}
async function onPlayniteReinstallDone(res: { ok: boolean; error?: string }) {
  if (res.ok) message.success('Playnite Extension updated');
  else message.error('Update failed' + (res.error ? `: ${res.error}` : ''));
  await refreshPlayniteAndApps();
}

const showVigemBanner = computed(
  () =>
    isWindows.value &&
    (config.value as Record<string, unknown>)?.['controller'] === 'enabled' &&
    host.vigemInstalled === false,
);
const showGoldenBanner = computed(
  () =>
    isWindows.value && host.golden?.exists === true && host.golden?.needs_layout_upgrade === true,
);
const showPreNotes = ref(false);
const showStableNotes = ref(false);

const hasAttention = computed(
  () =>
    playniteUpdateAvailable.value ||
    showPlayniteMissingPluginBanner.value ||
    host.crashDumpVisible ||
    showVigemBanner.value ||
    showGoldenBanner.value ||
    host.fatalLines.length > 0 ||
    update.stableBuildAvailable.value ||
    (update.notifyPreReleases.value && update.preReleaseBuildAvailable.value) ||
    update.buildVersionIsDirty.value,
);

// ---- session details live on the Stream page ---------------------------
function openSession(id: string) {
  void router.push({ path: '/stream', query: { id } });
}

// ---- lifecycle -------------------------------------------------------------
onMounted(async () => {
  clock = setInterval(() => (nowSec.value = Math.floor(Date.now() / 1000)), 1000);
  await auth.waitForAuthentication();
  if (!host.running) void host.start();
  if (!auth.isStatsOnly()) {
    void update.run();
    if (!appsStore.apps.length) void appsStore.loadApps();
  }
});
onBeforeUnmount(() => {
  if (clock) clearInterval(clock);
  if (liveTimer) clearInterval(liveTimer);
});
</script>

<template>
  <div class="grid grid-cols-12 gap-3.5 p-4">
    <!-- Live session / idle -->
    <section class="mc-panel col-span-12 lg:col-span-8">
      <template v-if="activeSession">
        <div class="mc-panel-h">
          <div class="flex min-w-0 items-center gap-2.5">
            <span class="mc-tag mc-tag-live"
              ><span class="mc-dot mc-dot-gold mc-dot-pulse"></span
              >{{ t2('overview.live', 'Live') }}</span
            >
            <span class="truncate text-sm font-semibold">{{ sessionApp(activeSession) }}</span>
            <span class="text-ink-4">→</span>
            <span class="truncate text-[13px] text-ink-2">{{ sessionClient(activeSession) }}</span>
          </div>
          <div class="flex shrink-0 items-center gap-2">
            <span class="font-mono text-xs text-ink-3">{{ fmtDuration(activeSession) }}</span>
            <NButton size="small" @click="openSession(activeSession.id)">{{
              t2('overview.details', 'Details')
            }}</NButton>
          </div>
        </div>
        <div class="grid grid-cols-2 gap-3 px-4 pb-1.5 pt-3.5 sm:grid-cols-5">
          <div>
            <div class="mc-kicker">{{ t2('overview.metric_fps', 'Actual FPS') }}</div>
            <div class="mc-metric-value">{{ fmt0(live.fps) }}</div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.metric_bitrate', 'Bitrate') }}</div>
            <div class="mc-metric-value">
              {{ fmt0(live.mbps) }}<span class="mc-metric-unit">Mbps</span>
            </div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.metric_encode', 'Encode') }}</div>
            <div class="mc-metric-value">
              {{ fmt1(live.encodeMs) }}<span class="mc-metric-unit">ms</span>
            </div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.metric_target', 'Target') }}</div>
            <div class="mc-metric-value">
              {{ fmt0(live.targetMbps) }}<span class="mc-metric-unit">Mbps</span>
            </div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.metric_losses', 'Loss events') }}</div>
            <div class="mc-metric-value">{{ fmt0(live.losses) }}</div>
          </div>
        </div>
        <div class="px-4 pb-3.5 pt-1.5 text-[11.5px] text-ink-3">
          {{ sessionFormat(activeSession) }}<span v-if="live.hdr"> · HDR</span>
          <span v-if="metadata?.capture_backend_display">
            · {{ metadata.capture_backend_display }}</span
          >
        </div>
      </template>
      <template v-else>
        <div class="mc-panel-h">
          <div class="flex items-center gap-2.5">
            <span class="mc-tag">{{ t2('overview.idle', 'Idle') }}</span>
            <span class="text-sm font-semibold">{{
              t2('overview.no_session', 'No active session')
            }}</span>
          </div>
          <div class="flex items-center gap-2">
            <LinkButton v-if="!auth.isStatsOnly()" to="/clients" size="small"
              ><NavIcon name="clients" :size="14" />{{
                t2('overview.pair', 'Pair with PIN')
              }}</LinkButton
            >
            <LinkButton to="/stream" size="small"
              ><NavIcon name="stream" :size="14" />{{
                t2('shell.nav_stream', 'Stream')
              }}</LinkButton
            >
          </div>
        </div>
        <div class="px-4 pb-1.5 pt-3.5 text-[12.5px] leading-relaxed text-ink-3">
          {{
            host.monitorOffline
              ? t2(
                  'overview.monitor_offline',
                  'The session monitor service is offline; live telemetry will appear once LuminalShineSessionMonitor is running.',
                )
              : t2(
                  'overview.ready_hint',
                  'Host is ready. A Moonlight or browser client can connect now; the virtual display attaches when the session starts.',
                )
          }}
        </div>
        <div class="grid grid-cols-2 gap-3 px-4 pb-4 pt-2.5 sm:grid-cols-4">
          <div>
            <div class="mc-kicker">{{ t2('overview.last_session', 'Last session') }}</div>
            <div class="text-[13px] font-medium">
              {{ lastSession ? `${sessionApp(lastSession)} · ${sessionClient(lastSession)}` : '—' }}
            </div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.ended', 'Ended') }}</div>
            <div class="text-[13px] font-medium">
              {{
                lastSession?.stream_ended_at
                  ? `${fmtClock(lastSession.stream_ended_at)} · ${fmtDurationShort(lastSession)}`
                  : '—'
              }}
            </div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.clients_connected', 'Clients') }}</div>
            <div class="text-[13px] font-medium">
              {{
                host.clientsLoaded
                  ? `${host.clients.length} ${t2('overview.paired', 'paired')}`
                  : '—'
              }}
            </div>
          </div>
          <div>
            <div class="mc-kicker">{{ t2('overview.ready_checks', 'Ready checks') }}</div>
            <div class="text-[13px] font-medium">
              {{
                healthRows.length
                  ? `${passing} ${t2('overview.of', 'of')} ${healthRows.length} ${t2('overview.passing', 'passing')}`
                  : '—'
              }}
            </div>
          </div>
        </div>
      </template>
    </section>

    <!-- Host health -->
    <section class="mc-panel col-span-12 lg:col-span-4">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('overview.health', 'Host health') }}</span>
        <RouterLink v-if="!auth.isStatsOnly()" to="/diagnostics" class="mc-panel-link">{{
          t2('shell.nav_diagnostics', 'Diagnostics')
        }}</RouterLink>
      </div>
      <div v-if="healthRows.length === 0" class="px-4 py-3 text-xs text-ink-4">
        {{ t2('overview.health_unavailable', 'Health checks are not available for this session.') }}
      </div>
      <div v-for="row in healthRows" :key="row.id" class="mc-check">
        <span class="mc-check-label"
          ><span class="mc-dot" :class="dotClass(row.state)"></span
          ><span class="truncate">{{ row.label }}</span></span
        >
        <span class="mc-check-value">{{ row.detail }}</span>
      </div>
    </section>

    <!-- Needs attention -->
    <section v-if="hasAttention" class="mc-panel col-span-12">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('overview.attention', 'Needs attention') }}</span>
      </div>
      <div class="space-y-3 p-4 text-sm">
        <NAlert v-if="host.crashDumpVisible" type="error" :show-icon="true">
          <div class="flex w-full flex-col gap-3 md:flex-row md:items-center md:justify-between">
            <div class="min-w-0 space-y-1">
              <p class="m-0 text-sm font-medium">
                {{ t2('config.crash_dump_title', 'Recent crash detected') }}
              </p>
              <p class="m-0 text-xs opacity-80">
                {{
                  t2(
                    'config.crash_dump_desc',
                    'LuminalShine detected a recent crash dump. Please export a crash bundle and include it when filing an issue.',
                  )
                }}
              </p>
              <p v-if="crash.details.value" class="m-0 text-xs opacity-60">
                {{ crash.detected.value }} {{ crash.details.value }}
              </p>
            </div>
            <div class="grid shrink-0 gap-2 sm:flex sm:flex-wrap sm:items-center">
              <NButton
                tag="a"
                size="small"
                href="https://github.com/NortheBridge/luminalshine/issues/new?template=bug_report.yml"
                target="_blank"
                rel="noopener noreferrer"
                >{{ t2('config.crash_dump_report', 'Report issue') }}</NButton
              >
              <NButton
                type="primary"
                size="small"
                :loading="crash.exportPending.value"
                :disabled="crash.exportPending.value"
                @click="crash.exportBundle()"
              >
                {{
                  crash.exportPending.value
                    ? t2('config.crash_dump_export_preparing', 'Preparing crash bundle…')
                    : t2('config.crash_dump_export', 'Export crash bundle')
                }}
              </NButton>
              <NButton tertiary size="small" @click="crash.dismiss()">{{
                t2('config.crash_dump_dismiss', 'Dismiss')
              }}</NButton>
            </div>
          </div>
        </NAlert>

        <NAlert v-if="host.fatalLines.length > 0" type="error" :show-icon="true">
          <div class="space-y-2">
            <p class="m-0 text-sm font-medium">
              {{ t2('overview.fatal_title', 'Fatal errors during startup') }}
            </p>
            <ul class="list-disc space-y-1 pl-5 text-xs">
              <li v-for="(line, i) in host.fatalLines" :key="i">{{ line.message }}</li>
            </ul>
            <LinkButton to="/diagnostics#logs" type="error" size="small">{{
              t2('index.view_logs', 'View logs')
            }}</LinkButton>
          </div>
        </NAlert>

        <NAlert v-if="playniteUpdateAvailable" type="warning" :show-icon="true">
          <div class="flex w-full flex-col gap-3 md:flex-row md:items-center md:justify-between">
            <div class="min-w-0">
              <p class="m-0 text-sm font-medium">Playnite Extension update available</p>
              <p class="m-0 text-xs opacity-80">
                {{
                  (playnite?.installed_version || 'unknown') +
                  ' → ' +
                  (playnite?.packaged_version || 'unknown')
                }}
              </p>
            </div>
            <div class="shrink-0">
              <PlayniteReinstallButton
                size="small"
                :strong="true"
                :restart="true"
                :label="'Update Playnite Extension'"
                @done="onPlayniteReinstallDone"
              />
            </div>
          </div>
        </NAlert>

        <NAlert v-if="showPlayniteMissingPluginBanner" type="warning" :show-icon="true">
          <div class="flex w-full flex-col gap-3 md:flex-row md:items-center md:justify-between">
            <div class="min-w-0">
              <p class="m-0 text-sm font-medium">Playnite Extension missing</p>
              <p class="m-0 text-xs opacity-80">{{ playniteMissingPluginBannerText }}</p>
            </div>
            <div class="grid shrink-0 gap-2 sm:flex sm:flex-wrap sm:items-center">
              <NButton
                size="small"
                type="primary"
                :loading="resolvingPlaynitePluginIssue"
                :disabled="resolvingPlaynitePluginIssue || purgingPlayniteApps"
                @click="resolvePlaynitePluginIssue"
                >Resolve issue</NButton
              >
              <NButton
                size="small"
                type="error"
                secondary
                :loading="purgingPlayniteApps"
                :disabled="purgingPlayniteApps || resolvingPlaynitePluginIssue"
                @click="openPurgePlayniteGamesConfirm"
                >Purge Playnite games</NButton
              >
            </div>
          </div>
        </NAlert>

        <NAlert v-if="showVigemBanner" type="warning" :show-icon="true">
          <div class="flex w-full flex-col gap-3 md:flex-row md:items-center md:justify-between">
            <div class="min-w-0">
              <p class="m-0 text-sm font-medium">
                {{
                  t2('config.vigem_missing_title', 'Virtual Gamepad Driver (ViGEm) not installed')
                }}
              </p>
              <p class="m-0 text-xs opacity-80">
                {{
                  t2(
                    'config.vigem_missing_desc',
                    'LuminalShine requires the ViGEmBus driver to emulate controllers on Windows. It is no longer bundled. Please download and install it manually:',
                  )
                }}
                <span v-if="host.vigemVersion" class="ml-2 opacity-60"
                  >({{ t2('config.vigem_detected_version', 'Detected') }}:
                  {{ host.vigemVersion }})</span
                >
              </p>
            </div>
            <div class="shrink-0">
              <NButton
                tag="a"
                type="primary"
                size="small"
                href="https://github.com/nefarius/ViGEmBus/releases/latest"
                target="_blank"
                rel="noopener noreferrer"
                >{{ t2('config.vigem_install', 'Download ViGEmBus') }}</NButton
              >
            </div>
          </div>
        </NAlert>

        <NAlert v-if="showGoldenBanner" type="warning" :show-icon="true">
          <div class="flex w-full flex-col gap-3 md:flex-row md:items-center md:justify-between">
            <div class="min-w-0">
              <p class="m-0 text-sm font-medium">
                {{
                  t2(
                    'config.golden_layout_upgrade_title',
                    'Golden display snapshot needs an update',
                  )
                }}
              </p>
              <p class="m-0 text-xs opacity-80">
                {{
                  t2(
                    'config.golden_layout_upgrade_desc',
                    'Your saved snapshot predates display layout support. Recreate it to restore portrait and landscape monitor layouts correctly.',
                  )
                }}
              </p>
            </div>
            <div class="shrink-0">
              <LinkButton
                :to="{
                  path: '/settings',
                  query: { sec: 'av', jump: 'dd_always_restore_from_golden' },
                }"
                type="primary"
                size="small"
                >{{
                  t2('config.golden_layout_upgrade_action', 'Open display settings')
                }}</LinkButton
              >
            </div>
          </div>
        </NAlert>

        <NAlert v-if="update.buildVersionIsDirty.value" type="success" :show-icon="true">{{
          t2('index.version_dirty', 'Development build.')
        }}</NAlert>

        <NAlert
          v-if="update.notifyPreReleases.value && update.preReleaseBuildAvailable.value"
          type="warning"
          :show-icon="false"
        >
          <div class="flex w-full flex-col gap-3">
            <div class="flex flex-col gap-3 md:flex-row md:items-center md:justify-between">
              <div class="min-w-0">
                <p class="m-0 text-sm font-medium">
                  {{ t2('index.new_pre_release', 'A new pre-release is available') }}
                </p>
                <p class="m-0 text-xs opacity-80">
                  {{ update.displayVersion.value }} → {{ update.preReleaseVersion.value.version }}
                </p>
              </div>
              <div class="grid shrink-0 gap-2 sm:flex sm:flex-wrap sm:items-center">
                <NButton size="small" @click="showPreNotes = !showPreNotes">{{
                  showPreNotes
                    ? t2('index.hide_notes', 'Hide notes')
                    : t2('index.view_notes', 'Release notes')
                }}</NButton>
                <NButton
                  tag="a"
                  size="small"
                  type="primary"
                  :href="update.preReleaseRelease.value?.html_url"
                  target="_blank"
                  >{{ t2('index.download', 'Download') }}</NButton
                >
              </div>
            </div>
            <div
              v-if="showPreNotes"
              class="max-h-72 overflow-auto rounded-lg border border-line bg-dark p-3 text-xs"
            >
              <p class="mb-2 font-semibold">{{ update.preReleaseRelease.value?.name }}</p>
              <!-- eslint-disable vue/no-v-html -- sanitized by DOMPurify in renderReleaseMarkdown -->
              <div
                class="release-notes-prose prose prose-sm prose-invert max-w-none"
                v-html="update.preReleaseHtml.value"
              />
              <!-- eslint-enable vue/no-v-html -->
            </div>
          </div>
        </NAlert>

        <NAlert v-if="update.stableBuildAvailable.value" type="warning" :show-icon="false">
          <div class="flex w-full flex-col gap-3">
            <div class="flex flex-col gap-3 md:flex-row md:items-center md:justify-between">
              <div class="min-w-0">
                <p class="m-0 text-sm font-medium">
                  {{ t2('index.new_stable', 'A new stable release is available') }}
                </p>
                <p class="m-0 text-xs opacity-80">
                  {{ update.displayVersion.value }} → {{ update.githubVersion.value.version }}
                </p>
              </div>
              <div class="grid shrink-0 gap-2 sm:flex sm:flex-wrap sm:items-center">
                <NButton size="small" @click="showStableNotes = !showStableNotes">{{
                  showStableNotes
                    ? t2('index.hide_notes', 'Hide notes')
                    : t2('index.view_notes', 'Release notes')
                }}</NButton>
                <NButton
                  tag="a"
                  size="small"
                  type="primary"
                  :href="update.githubRelease.value?.html_url"
                  target="_blank"
                  >{{ t2('index.download', 'Download') }}</NButton
                >
              </div>
            </div>
            <div
              v-if="showStableNotes"
              class="max-h-72 overflow-auto rounded-lg border border-line bg-dark p-3 text-xs"
            >
              <p class="mb-2 font-semibold">{{ update.githubRelease.value?.name }}</p>
              <!-- eslint-disable vue/no-v-html -- sanitized by DOMPurify in renderReleaseMarkdown -->
              <div
                class="release-notes-prose prose prose-sm prose-invert max-w-none"
                v-html="update.stableReleaseHtml.value"
              />
              <!-- eslint-enable vue/no-v-html -->
            </div>
          </div>
        </NAlert>
      </div>
    </section>

    <!-- Clients -->
    <section v-if="!auth.isStatsOnly()" class="mc-panel col-span-12 md:col-span-6 xl:col-span-4">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('shell.nav_clients', 'Clients') }}</span>
        <RouterLink to="/clients" class="mc-panel-link">{{
          t2('overview.pair', 'Pair with PIN')
        }}</RouterLink>
      </div>
      <div v-if="host.clientsLoaded && clients.length === 0" class="px-4 py-3 text-xs text-ink-4">
        {{ t2('overview.no_clients', 'No clients paired yet.') }}
      </div>
      <div v-for="c in clients" :key="c.uuid" class="mc-row">
        <div class="min-w-0">
          <div class="mc-row-primary truncate">
            {{ c.name || t2('overview.unknown_client', 'unknown client') }}
          </div>
          <div class="mc-row-secondary">
            {{
              c.connected
                ? activeSession
                  ? sessionFormat(activeSession)
                  : t2('overview.connected', 'Connected')
                : `${t2('overview.last_seen', 'Last seen')} ${fmtRelative(c.lastSeen)}`
            }}
          </div>
        </div>
        <span class="mc-tag" :class="c.connected ? 'mc-tag-ok' : ''">{{
          c.connected ? t2('overview.connected', 'Connected') : t2('overview.idle', 'Idle')
        }}</span>
      </div>
    </section>

    <!-- Applications -->
    <section v-if="!auth.isStatsOnly()" class="mc-panel col-span-12 md:col-span-6 xl:col-span-4">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('shell.nav_library', 'Library') }}</span>
        <RouterLink to="/library" class="mc-panel-link">{{
          t2('overview.manage', 'Manage')
        }}</RouterLink>
      </div>
      <div v-if="apps.length === 0" class="px-4 py-3 text-xs text-ink-4">
        {{ t2('overview.no_apps', 'No applications configured.') }}
      </div>
      <div v-for="(app, i) in apps" :key="app.uuid || i" class="mc-row">
        <div class="min-w-0">
          <div class="mc-row-primary truncate">{{ app.name }}</div>
          <div class="mc-row-secondary">{{ appSource(app) }}</div>
        </div>
        <span class="mc-tag" :class="appRunning(app) ? 'mc-tag-live' : ''">{{
          appRunning(app) ? t2('overview.running', 'Running') : t2('overview.ready', 'Ready')
        }}</span>
      </div>
    </section>

    <!-- Recent sessions -->
    <section
      class="mc-panel col-span-12 xl:col-span-4"
      :class="auth.isStatsOnly() ? 'lg:col-span-12' : 'md:col-span-12'"
    >
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('overview.recent_sessions', 'Recent sessions') }}</span>
        <RouterLink to="/stream" class="mc-panel-link">{{
          t2('shell.nav_stream', 'Stream')
        }}</RouterLink>
      </div>
      <div v-if="host.monitorOffline" class="px-4 py-3 text-xs text-ink-4">
        {{ t2('overview.monitor_offline_short', 'Session monitor offline.') }}
      </div>
      <div
        v-else-if="host.sessionsLoaded && recentSessions.length === 0"
        class="px-4 py-3 text-xs text-ink-4"
      >
        {{ t2('overview.no_sessions', 'No sessions recorded yet.') }}
      </div>
      <div
        v-for="s in recentSessions"
        :key="s.id"
        class="mc-row mc-row-clickable"
        @click="openSession(s.id)"
      >
        <div class="min-w-0">
          <div class="mc-row-primary truncate">{{ sessionApp(s) }} · {{ sessionClient(s) }}</div>
          <div class="mc-row-secondary truncate">{{ sessionSub(s) }}</div>
        </div>
        <span class="mc-tag" :class="s.stream_ended_at == null ? 'mc-tag-live' : ''">{{
          s.stream_ended_at == null
            ? t2('overview.live', 'Live')
            : t2('overview.ended_tag', 'Ended')
        }}</span>
      </div>
    </section>

    <!-- Events -->
    <section v-if="!auth.isStatsOnly()" class="mc-panel col-span-12">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('overview.events', 'Recent log') }}</span>
        <div class="flex items-center gap-3">
          <button type="button" class="mc-panel-link" @click="host.refreshLogs()">
            {{ t2('overview.refresh', 'Refresh') }}
          </button>
          <RouterLink to="/diagnostics#logs" class="mc-panel-link">{{
            t2('overview.open_logs', 'Open logs')
          }}</RouterLink>
        </div>
      </div>
      <div v-if="events.length === 0" class="px-4 py-3 text-xs text-ink-4">
        {{ host.logsLoaded ? t2('overview.no_log', 'The log is empty.') : '…' }}
      </div>
      <div v-else class="py-1.5">
        <div v-for="(line, i) in events" :key="i" class="mc-log">
          <span class="mc-log-time">{{ line.time }}</span>
          <span :class="levelClass(line.level)">{{ line.level }}</span>
          <span class="truncate">{{ line.message }}</span>
        </div>
      </div>
    </section>

    <!-- High-performance preset (Windows) -->
    <div v-if="!auth.isStatsOnly() && isWindows" class="col-span-12">
      <HighPerformanceCard />
    </div>

    <!-- Inspector: stream settings for the next session -->
    <InspectorPanel
      v-if="!auth.isStatsOnly()"
      :title="t2('overview.stream_settings', 'Stream settings')"
      :subtitle="t2('overview.stream_settings_sub', 'Applies to the next session · auto-saves')"
      :tag="t2('overview.stream_settings_tag', 'Audio / Video')"
    >
      <div class="mc-field">
        <ConfigFieldRenderer v-model="config.encoder" setting-key="encoder" desc="" size="small" />
      </div>
      <div class="mc-field">
        <ConfigFieldRenderer
          v-model="config.hevc_mode"
          setting-key="hevc_mode"
          desc=""
          size="small"
        />
      </div>
      <div class="mc-field">
        <ConfigFieldRenderer
          v-model="config.av1_mode"
          setting-key="av1_mode"
          desc=""
          size="small"
        />
      </div>
      <div class="mc-field">
        <ConfigFieldRenderer
          v-model="config.max_bitrate"
          setting-key="max_bitrate"
          desc=""
          size="small"
        />
      </div>
      <div v-if="isWindows" class="mc-field">
        <ConfigFieldRenderer
          v-model="config.vgd_hdr_peak_nits"
          setting-key="vgd_hdr_peak_nits"
          desc=""
          size="small"
        />
      </div>
      <div v-if="isWindows" class="mc-field">
        <ConfigFieldRenderer
          v-model="config.virtual_display_layout"
          setting-key="virtual_display_layout"
          desc=""
          size="small"
        />
      </div>
      <div v-if="isWindows" class="mc-field">
        <ConfigFieldRenderer
          v-model="config.dd_config_revert_on_disconnect"
          setting-key="dd_config_revert_on_disconnect"
          desc=""
          size="small"
        />
      </div>
      <template #footer>
        <span class="text-[11.5px] text-ink-3">{{
          t2('overview.stream_settings_footer', 'Changes apply to the next session')
        }}</span>
        <LinkButton to="/settings" size="small">{{
          t2('overview.all_settings', 'All settings')
        }}</LinkButton>
      </template>
    </InspectorPanel>
  </div>
</template>

<style scoped>
.release-notes-prose :deep(h1),
.release-notes-prose :deep(h2),
.release-notes-prose :deep(h3) {
  margin-top: 1em;
  margin-bottom: 0.5em;
  font-size: 1.1em;
}
.release-notes-prose :deep(p),
.release-notes-prose :deep(ul),
.release-notes-prose :deep(ol) {
  margin-top: 0.5em;
  margin-bottom: 0.5em;
}
.release-notes-prose :deep(pre) {
  font-size: 0.85em;
  padding: 0.6em 0.8em;
  border-radius: 0.4rem;
  overflow-x: auto;
}
.release-notes-prose :deep(a) {
  text-decoration: underline;
  text-underline-offset: 2px;
}
</style>
