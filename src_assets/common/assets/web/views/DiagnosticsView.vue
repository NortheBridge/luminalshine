<script setup lang="ts">
/**
 * Diagnostics — health cards, host actions and the log viewer on one page;
 * the inspector carries the host identity report (copyable) and the reset
 * actions. `?sec=about` opens the full About page; `#logs` deep-links to
 * the log viewer.
 */
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { NButton, useDialog, useMessage } from 'naive-ui';
import { storeToRefs } from 'pinia';
import { http } from '@/http';
import { useT2 } from '@/composables/useT2';
import { useCrashDump } from '@/composables/useCrashDump';
import { useHostReport } from '@/composables/useHostReport';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import { useHostStore } from '@/stores/host';
import AboutView from '@/views/AboutView.vue';
import ResourceCard from '@/ResourceCard.vue';
import LogViewer from '@/components/LogViewer.vue';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import NavIcon from '@/components/shell/NavIcon.vue';

const route = useRoute();
const router = useRouter();
const t2 = useT2();
const dialog = useDialog();
const message = useMessage();
const auth = useAuthStore();
const host = useHostStore();
const configStore = useConfigStore();
const { metadata } = storeToRefs(configStore);
const {
  details: crashDetails,
  exportPending: crashExporting,
  exportBundle,
  dismiss: dismissCrash,
} = useCrashDump();
const { rows: reportRows, version: reportVersion, copy: copyReport } = useHostReport();

const isWindows = computed(
  () => String(metadata.value?.platform || '').toLowerCase() === 'windows',
);

type Section = 'health' | 'about';
const section = computed<Section>(() => (route.query['sec'] === 'about' ? 'about' : 'health'));
function go(sec: Section): void {
  const query = { ...route.query };
  if (sec === 'about') query['sec'] = 'about';
  else delete query['sec'];
  void router.replace({ path: route.path, query, hash: sec === 'health' ? route.hash : '' });
}
function scrollToHash(): void {
  const hash = route.hash;
  if (!hash || section.value !== 'health') return;
  void nextTick(() => {
    try {
      document.querySelector(hash)?.scrollIntoView({ block: 'start' });
    } catch {
      /* invalid selector; ignore */
    }
  });
}

// ---- TDR / display-stack health ---------------------------------------------
interface TdrLast {
  at: number;
  source: string;
  hresult: number;
  detail: string;
}
interface TdrIncident {
  started_at: number;
  last_at: number;
  duration_seconds: number;
  events: number;
  first_source: string;
  source: string;
  terminal: boolean;
}
const tdrCount = ref(0);
const tdrRecoveryRecent = ref(false);
const tdrStackDown = ref(false);
const tdrIncident = ref<TdrIncident | null>(null);
const tdrLast = ref<TdrLast | null>(null);
const healthRefreshing = ref(false);

const tdrIncidentDuration = computed(() => {
  const inc = tdrIncident.value;
  if (!inc || inc.duration_seconds <= 0) return '';
  const total = Math.round(inc.duration_seconds);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
});
const tdrLastText = computed(() => {
  const l = tdrLast.value;
  if (!l) return t2('diagnostics.none_recorded', 'none recorded');
  const when = new Date(l.at * 1000).toLocaleString();
  const hr = l.hresult
    ? ` · 0x${(l.hresult >>> 0).toString(16).toUpperCase().padStart(8, '0')}`
    : '';
  return `${when} · ${l.source}${hr}`;
});

async function refreshTdrHealth(): Promise<void> {
  try {
    const r = await http.get('./api/health/tdr', { validateStatus: () => true });
    const body = (r.data || {}) as Record<string, unknown>;
    if (r.status < 200 || r.status >= 300) return;
    tdrCount.value = typeof body['count'] === 'number' ? body['count'] : 0;
    tdrRecoveryRecent.value = body['recovery_recent'] === true;
    tdrStackDown.value = body['stack_down'] === true;
    const incident = body['incident'] as Partial<TdrIncident>;
    tdrIncident.value =
      incident && typeof incident.started_at === 'number'
        ? {
            started_at: incident.started_at,
            last_at: typeof incident.last_at === 'number' ? incident.last_at : incident.started_at,
            duration_seconds:
              typeof incident.duration_seconds === 'number' ? incident.duration_seconds : 0,
            events: typeof incident.events === 'number' ? incident.events : 0,
            first_source: typeof incident.first_source === 'string' ? incident.first_source : '',
            source: typeof incident.source === 'string' ? incident.source : '',
            terminal: incident.terminal === true,
          }
        : null;
    const last = body['last'] as Partial<TdrLast>;
    tdrLast.value =
      last && typeof last.at === 'number' && typeof last.source === 'string'
        ? {
            at: last.at,
            source: last.source,
            hresult: typeof last.hresult === 'number' ? last.hresult : 0,
            detail: typeof last.detail === 'string' ? last.detail : '',
          }
        : null;
  } catch {
    /* shows as no data */
  }
}

// ---- Render-stack detection (DLSS / DLAA / FG) ---------------------------------
interface RenderStackPayload {
  status?: boolean;
  has_event: boolean;
  has_dlss?: boolean;
  has_dlss_fg?: boolean;
  has_dlaa?: boolean;
  resolution_width?: number;
  resolution_height?: number;
  hdr_enabled?: boolean;
  bit_depth?: number;
  codec_label?: string;
  tip?: string;
}
const renderStack = ref<RenderStackPayload | null>(null);
const renderStackBadges = computed<string[]>(() => {
  const r = renderStack.value;
  if (!r || !r.has_event) return [];
  const out: string[] = [];
  if (r.has_dlss_fg) out.push('DLSS Frame Gen');
  else if (r.has_dlaa) out.push('DLAA');
  else if (r.has_dlss) out.push('DLSS');
  if (r.resolution_width && r.resolution_height)
    out.push(`${r.resolution_width}×${r.resolution_height}`);
  if (r.hdr_enabled) out.push('HDR');
  if (r.bit_depth) out.push(`${r.bit_depth}-bit`);
  if (r.codec_label) out.push(r.codec_label);
  return out;
});
const renderStackTip = computed(() => {
  const r = renderStack.value;
  return r && r.has_event && r.tip ? r.tip : '';
});
async function refreshRenderStack(): Promise<void> {
  if (!isWindows.value) return;
  try {
    const r = await http.get('./api/health/render-stack', { validateStatus: () => true });
    const body = (r.data || {}) as RenderStackPayload;
    if (r.status >= 200 && r.status < 300 && body.status !== false) renderStack.value = body;
  } catch {
    /* keep the previous value */
  }
}

const captureEngine = computed(
  () => metadata.value?.capture_backend_display || metadata.value?.capture_backend_active || '',
);
const captureAge = computed(() => {
  const s = metadata.value?.capture_backend_age_seconds;
  if (typeof s !== 'number' || s < 0) return '';
  const ago = t2('diagnostics.ago', 'ago');
  if (s < 60) return `${Math.round(s)}s ${ago}`;
  if (s < 3600) return `${Math.round(s / 60)}m ${ago}`;
  return `${(s / 3600).toFixed(1)}h ${ago}`;
});

const vgdLine = computed(() => {
  const md = metadata.value;
  if (!md) return '';
  const v = md.virtual_display_backend_version || md.vgd_bundled_version || '';
  return [v, md.vgd_bundled_signature ? t2('diagnostics.signed', 'signed') : '']
    .filter(Boolean)
    .join(' · ');
});
const vgdStatus = computed(() => {
  const md = metadata.value;
  if (!md?.platform || String(md.platform).toLowerCase() !== 'windows')
    return { text: '', kind: '' };
  if (
    md.virtual_display_driver_ready === true ||
    String(md.virtual_display_driver_status) === '0'
  ) {
    return { text: t2('diagnostics.ready', 'ready'), kind: 'mc-tag-ok' };
  }
  if (md.vgd_installed !== true) {
    return { text: t2('diagnostics.not_installed', 'not installed'), kind: 'mc-tag-warn' };
  }
  return { text: t2('diagnostics.degraded', 'degraded'), kind: 'mc-tag-danger' };
});
const gamepadBus = computed(() => {
  if (host.vigemInstalled == null) return t2('diagnostics.not_probed', 'not probed');
  if (!host.vigemInstalled) return t2('diagnostics.not_installed', 'not installed');
  return `ViGEmBus${host.vigemVersion ? ` ${host.vigemVersion}` : ''}`;
});
const buildLine = computed(() => {
  const md = metadata.value;
  if (!md) return '';
  return [md.version, md.commit ? md.commit.slice(0, 7) : '', md.release_date]
    .filter(Boolean)
    .join(' · ');
});

async function refreshAllHealth(): Promise<void> {
  if (healthRefreshing.value) return;
  healthRefreshing.value = true;
  try {
    await Promise.all([
      refreshTdrHealth(),
      refreshRenderStack(),
      configStore.fetchMetadata(),
      host.refreshHealth(),
    ]);
  } finally {
    healthRefreshing.value = false;
  }
}

// ---- Actions ---------------------------------------------------------------------
const logViewer = ref<{ exportLogs: () => void } | null>(null);
const restartPending = ref(false);
const closePending = ref(false);

function confirmRestart(): void {
  dialog.warning({
    title: t2('diagnostics.restart_title', 'Restart LuminalShine?'),
    content: t2(
      'diagnostics.restart_body',
      'Any active stream ends and clients need to reconnect. The service comes back in a few seconds.',
    ),
    positiveText: t2('diagnostics.restart_yes', 'Restart'),
    negativeText: t2('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      restartPending.value = true;
      try {
        await http.post('./api/restart', {}, { validateStatus: () => true });
        message.success(
          t2(
            'settings.restart_sent',
            'Restart requested. LuminalShine comes back in a few seconds.',
          ),
        );
        configStore.clearRestartPending();
      } catch {
        /* the process is going down; the request often does not answer */
      } finally {
        window.setTimeout(() => (restartPending.value = false), 5000);
      }
    },
  });
}

async function closeApp(): Promise<void> {
  closePending.value = true;
  try {
    const r = await http.post('./api/apps/close', {}, { validateStatus: () => true });
    const body = (r.data || {}) as { status?: boolean };
    if (body.status === true)
      message.success(t2('troubleshooting.force_close_success', 'Application closed.'));
    else message.error(t2('troubleshooting.force_close_error', 'No running application to close.'));
  } catch {
    message.error(t2('troubleshooting.force_close_error', 'No running application to close.'));
  } finally {
    closePending.value = false;
  }
}

// ---- Resets (inspector) ---------------------------------------------------------
interface ResetAction {
  id: string;
  label: string;
  desc: string;
  endpoint: string;
  title: string;
  body: string;
  yes: string;
  success: string;
}
const resets = computed<ResetAction[]>(() => [
  {
    id: 'admin',
    label: t2('troubleshooting.reset_admin_creds', 'Reset admin credentials'),
    desc: t2('diagnostics.reset_admin_desc', 'Clears the Web UI login; pairings stay'),
    endpoint: './api/state/reset-admin-credentials',
    title: t2('troubleshooting.reset_admin_creds_confirm_title', 'Reset Admin Credentials?'),
    body: t2(
      'troubleshooting.reset_admin_creds_confirm_body',
      'The stored admin username and password will be removed from the system credential vault. You will need to create a new admin user on the next Web UI visit. Paired Moonlight clients and trusted devices are NOT affected by this reset. Continue?',
    ),
    yes: t2('troubleshooting.reset_admin_creds_confirm_yes', 'Reset credentials'),
    success: t2('troubleshooting.reset_admin_creds_success', 'Admin credentials cleared.'),
  },
  {
    id: 'steam',
    label: t2('troubleshooting.clear_steam_library', 'Clear Steam library cache'),
    desc: t2(
      'diagnostics.reset_steam_desc',
      'Deletes steam_apps.json; re-syncs if auto-sync is on',
    ),
    endpoint: './api/state/reset-steam-library-cache',
    title: t2('troubleshooting.clear_steam_library_confirm_title', 'Clear Steam Library Cache?'),
    body: t2(
      'troubleshooting.clear_steam_library_confirm_body',
      'The steam_apps.json catalogue will be deleted from disk and the Steam game entries will disappear from Moonlight. Your hand-curated apps.json is NOT touched. If the Steam auto-sync toggle is still ON, the catalogue will be re-generated on the next sync tick. Continue?',
    ),
    yes: t2('troubleshooting.clear_steam_library_confirm_yes', 'Clear cache'),
    success: t2('troubleshooting.clear_steam_library_success', 'Steam library cache cleared.'),
  },
  {
    id: 'nonsteam',
    label: t2('troubleshooting.clear_nonsteam_shortcuts', 'Clear non-Steam shortcuts cache'),
    desc: t2(
      'diagnostics.reset_nonsteam_desc',
      'Deletes nonsg_apps.json; re-syncs if auto-sync is on',
    ),
    endpoint: './api/state/reset-nonsteam-shortcuts-cache',
    title: t2(
      'troubleshooting.clear_nonsteam_shortcuts_confirm_title',
      'Clear Non-Steam Shortcuts Cache?',
    ),
    body: t2(
      'troubleshooting.clear_nonsteam_shortcuts_confirm_body',
      'The nonsg_apps.json catalogue will be deleted from disk and the non-Steam shortcut entries will disappear from Moonlight. Your hand-curated apps.json and the Steam library catalogue are NOT touched. Continue?',
    ),
    yes: t2('troubleshooting.clear_nonsteam_shortcuts_confirm_yes', 'Clear cache'),
    success: t2(
      'troubleshooting.clear_nonsteam_shortcuts_success',
      'Non-Steam shortcuts cache cleared.',
    ),
  },
  {
    id: 'sessions',
    label: t2('troubleshooting.clear_session_history', 'Clear session history'),
    desc: t2('diagnostics.reset_sessions_desc', 'Deletes recorded session JSON files'),
    endpoint: './api/state/reset-session-history',
    title: t2('troubleshooting.clear_session_history_confirm_title', 'Clear Session History?'),
    body: t2(
      'troubleshooting.clear_session_history_confirm_body',
      'Every recorded session JSON in %ProgramData%\\LuminalShine\\sessions\\ will be deleted and the session history empties within a few seconds. A currently-active stream still shows up live. Continue?',
    ),
    yes: t2('troubleshooting.clear_session_history_confirm_yes', 'Clear history'),
    success: t2('troubleshooting.clear_session_history_success', 'Session history cleared.'),
  },
  {
    id: 'state',
    label: t2('troubleshooting.reset_state', 'Reset stored pairings'),
    desc: t2('diagnostics.reset_state_desc', 'Every client re-pairs; old state kept as .corrupt-*'),
    endpoint: './api/state/reset',
    title: t2('troubleshooting.reset_state_confirm_title', 'Reset stored pairings?'),
    body: t2(
      'troubleshooting.reset_state_confirm_body',
      'All paired Moonlight clients will need to re-pair. Current state files will be renamed with a .corrupt-TIMESTAMP suffix (kept on disk for forensics) and a fresh state will be written. Admin credentials are NOT touched. Continue?',
    ),
    yes: t2('troubleshooting.reset_state_confirm_yes', 'Reset stored state'),
    success: t2('diagnostics.reset_state_success', 'Stored pairings reset.'),
  },
]);
const resetPending = ref<string | null>(null);

function confirmReset(a: ResetAction): void {
  if (resetPending.value) return;
  dialog.warning({
    title: a.title,
    content: a.body,
    positiveText: a.yes,
    negativeText: t2('troubleshooting.cancel', 'Cancel'),
    onPositiveClick: async () => {
      resetPending.value = a.id;
      try {
        const r = await http.post(a.endpoint, {}, { validateStatus: () => true });
        const body = (r.data || {}) as {
          status?: boolean;
          message?: string;
          error?: string;
          archived?: unknown;
        };
        if (r.status >= 200 && r.status < 300 && body.status === true) {
          const archived = Array.isArray(body.archived)
            ? body.archived.filter((p): p is string => typeof p === 'string')
            : [];
          message.success(
            body.message || (archived.length ? `${a.success} ${archived.join(', ')}` : a.success),
            { duration: 6000 },
          );
          if (a.id === 'state') void host.refreshClients();
        } else {
          message.error(body.message || body.error || `HTTP ${r.status}`);
        }
      } catch (e: unknown) {
        message.error(e instanceof Error ? e.message : 'Request failed');
      } finally {
        resetPending.value = null;
      }
    },
  });
}

// ---- lifecycle -------------------------------------------------------------------
let loginDisposer: (() => void) | null = null;
onMounted(async () => {
  scrollToHash();
  loginDisposer = auth.onLogin(() => void refreshAllHealth());
  await auth.waitForAuthentication();
  if (!host.running) void host.start();
  await refreshAllHealth();
});
onBeforeUnmount(() => {
  if (loginDisposer) loginDisposer();
});
watch(() => route.hash, scrollToHash);
</script>

<template>
  <div class="flex gap-3.5">
    <div class="flex min-w-0 flex-1 flex-col gap-3.5">
      <!-- Header -->
      <div class="flex flex-wrap items-center gap-3">
        <div>
          <div class="mc-page-title">{{ t2('shell.nav_diagnostics', 'Diagnostics') }}</div>
          <div class="mc-page-sub">
            {{ t2('diagnostics.subtitle', 'Health, actions, logs and this host’s identity') }}
          </div>
        </div>
        <div class="flex-1"></div>
        <div class="mc-chips" role="tablist">
          <button
            type="button"
            class="mc-chip"
            :class="{ 'mc-chip-on': section === 'health' }"
            role="tab"
            :aria-selected="section === 'health'"
            @click="go('health')"
          >
            {{ t2('diagnostics.section_health', 'Health & logs') }}
          </button>
          <button
            type="button"
            class="mc-chip"
            :class="{ 'mc-chip-on': section === 'about' }"
            role="tab"
            :aria-selected="section === 'about'"
            @click="go('about')"
          >
            {{ t2('diagnostics.section_about', 'About this host') }}
          </button>
        </div>
        <template v-if="section === 'health'">
          <NButton size="small" @click="logViewer?.exportLogs()"
            ><NavIcon name="download" :size="13" />{{
              t2('troubleshooting.export_logs', 'Export logs')
            }}</NButton
          >
          <NButton size="small" :loading="closePending" @click="closeApp"
            ><NavIcon name="close" :size="13" />{{
              t2('troubleshooting.force_close', 'Force close app')
            }}</NButton
          >
          <NButton size="small" type="primary" :loading="restartPending" @click="confirmRestart"
            ><NavIcon name="power" :size="13" />{{
              t2('troubleshooting.restart_sunshine', 'Restart LuminalShine')
            }}</NButton
          >
        </template>
      </div>

      <template v-if="section === 'health'">
        <!-- Alerts -->
        <div v-if="tdrStackDown" class="mc-panel border-danger/40 px-4 py-3 text-xs">
          <div class="flex items-center gap-2 font-medium text-danger">
            <NavIcon name="warning" :size="14" />{{
              t2('troubleshooting.tdr_stack_down_title', 'Display stack is down')
            }}
          </div>
          <div class="mt-1 text-ink-3">
            {{
              t2(
                'troubleshooting.tdr_stack_down_body',
                'The GPU driver reset and did not come back. Streams cannot start until the host reboots or the display driver recovers.',
              )
            }}
          </div>
        </div>
        <div v-else-if="tdrRecoveryRecent" class="mc-panel border-primary/40 px-4 py-3 text-xs">
          <div class="flex items-center gap-2 font-medium text-primary">
            <NavIcon name="warning" :size="14" />{{
              t2('troubleshooting.tdr_recovery_title', 'Recent GPU recovery')
            }}
          </div>
          <div class="mt-1 text-ink-3">
            {{
              t2(
                'troubleshooting.tdr_recovery_body',
                'The GPU driver reset recently. If streams look wrong, restart LuminalShine; if it repeats, check the Windows event log and driver version.',
              )
            }}
          </div>
        </div>
        <div v-if="host.crashDumpVisible" class="mc-panel border-primary/40 px-4 py-3 text-xs">
          <div class="flex flex-wrap items-center gap-2">
            <NavIcon name="warning" :size="14" class="text-primary" />
            <span class="font-medium text-ink">{{
              t2('troubleshooting.crash_dump_title', 'Crash dump detected')
            }}</span>
            <span class="text-ink-3">{{ crashDetails }}</span>
            <div class="flex-1"></div>
            <NButton size="tiny" @click="dismissCrash()">{{
              t2('troubleshooting.crash_dump_dismiss', 'Dismiss')
            }}</NButton>
            <NButton size="tiny" type="primary" :loading="crashExporting" @click="exportBundle()"
              ><NavIcon name="download" :size="12" />{{
                t2('troubleshooting.export_crash_bundle', 'Export crash bundle')
              }}</NButton
            >
          </div>
        </div>

        <!-- Health cards -->
        <div class="grid gap-3.5 md:grid-cols-2 xl:grid-cols-3">
          <div class="mc-panel">
            <div class="mc-panel-h">
              <span class="mc-panel-title">{{
                t2('diagnostics.card_gpu', 'GPU & display stack')
              }}</span>
              <span
                class="mc-tag"
                :class="tdrStackDown ? 'mc-tag-danger' : tdrCount > 0 ? 'mc-tag-warn' : 'mc-tag-ok'"
              >
                {{
                  tdrStackDown
                    ? t2('diagnostics.down', 'down')
                    : tdrCount > 0
                      ? `${tdrCount} ${t2('diagnostics.incidents', 'incidents')}`
                      : t2('diagnostics.healthy', 'healthy')
                }}
              </span>
            </div>
            <div class="mc-kv">
              <span class="mc-kv-k">{{ t2('diagnostics.tdr_events', 'TDR events') }}</span
              ><span class="mc-kv-v"
                >{{ tdrCount }} {{ t2('diagnostics.since_start', 'since start') }}</span
              >
              <span class="mc-kv-k">{{ t2('diagnostics.stack', 'Display stack') }}</span
              ><span class="mc-kv-v" :class="tdrStackDown ? 'text-danger' : ''">{{
                tdrStackDown
                  ? t2('diagnostics.stack_down', 'down · reboot required')
                  : t2('diagnostics.stack_up', 'up')
              }}</span>
              <span class="mc-kv-k">{{ t2('diagnostics.last_reset', 'Last reset') }}</span
              ><span class="mc-kv-v break-words">{{ tdrLastText }}</span>
              <template v-if="tdrLast?.detail">
                <span class="mc-kv-k">{{ t2('diagnostics.detail', 'Detail') }}</span>
                <span class="mc-kv-v break-words">{{ tdrLast.detail }}</span>
              </template>
              <template v-if="tdrIncident">
                <span class="mc-kv-k">{{ t2('diagnostics.incident', 'Incident') }}</span>
                <span class="mc-kv-v"
                  >{{ tdrIncident.events }} {{ t2('diagnostics.events', 'events')
                  }}<template v-if="tdrIncidentDuration"> · {{ tdrIncidentDuration }}</template
                  ><template v-if="tdrIncident.terminal">
                    · {{ t2('diagnostics.terminal', 'terminal') }}</template
                  ></span
                >
              </template>
            </div>
            <div class="mc-panel-f">
              <button
                type="button"
                class="mc-panel-link"
                :disabled="healthRefreshing"
                @click="refreshAllHealth"
              >
                <NavIcon name="refresh" :size="12" />{{ t2('_common.refresh', 'Refresh') }}
              </button>
            </div>
          </div>

          <div v-if="isWindows" class="mc-panel">
            <div class="mc-panel-h">
              <span class="mc-panel-title">{{
                t2('diagnostics.card_render', 'Capture & render stack')
              }}</span>
              <span v-if="captureEngine" class="mc-tag mc-tag-ok">{{ captureEngine }}</span>
            </div>
            <div class="mc-kv">
              <span class="mc-kv-k">{{ t2('diagnostics.capture_engine', 'Capture engine') }}</span
              ><span class="mc-kv-v"
                >{{ captureEngine || t2('diagnostics.idle', 'idle')
                }}<template v-if="captureAge"> · {{ captureAge }}</template></span
              >
              <span class="mc-kv-k">{{ t2('diagnostics.detected', 'Detected') }}</span>
              <span class="mc-kv-v">
                <template v-if="renderStackBadges.length"
                  ><span v-for="b in renderStackBadges" :key="b" class="mc-tag mr-1">{{
                    b
                  }}</span></template
                >
                <template v-else>{{
                  t2('diagnostics.no_game_event', 'no game session seen yet')
                }}</template>
              </span>
            </div>
            <div
              v-if="renderStackTip"
              class="border-t border-line px-4 py-2.5 text-[11.5px] leading-snug text-primary"
            >
              {{ renderStackTip }}
            </div>
          </div>

          <div class="mc-panel">
            <div class="mc-panel-h">
              <span class="mc-panel-title">{{
                t2('diagnostics.card_drivers', 'Drivers & buses')
              }}</span>
              <span v-if="vgdStatus.text" class="mc-tag" :class="vgdStatus.kind">{{
                vgdStatus.text
              }}</span>
            </div>
            <div class="mc-kv">
              <span class="mc-kv-k">LuminalVGD</span
              ><span class="mc-kv-v">{{
                vgdLine || t2('diagnostics.not_installed', 'not installed')
              }}</span>
              <span class="mc-kv-k">{{ t2('diagnostics.gamepad_bus', 'Gamepad bus') }}</span
              ><span class="mc-kv-v">{{ gamepadBus }}</span>
              <span class="mc-kv-k">{{ t2('diagnostics.build', 'Build') }}</span
              ><span class="mc-kv-v break-words">{{ buildLine }}</span>
              <span class="mc-kv-k">{{ t2('diagnostics.signing', 'Signing') }}</span
              ><span class="mc-kv-v">NortheBridge Foundation · Authenticode</span>
            </div>
            <div class="mc-panel-f">
              <RouterLink class="mc-panel-link" to="/display">{{
                t2('diagnostics.open_display', 'Open Display')
              }}</RouterLink>
            </div>
          </div>
        </div>

        <LogViewer ref="logViewer" :height="480" />
      </template>

      <template v-else>
        <AboutView />
        <div class="mc-panel">
          <div class="mc-panel-h">
            <span class="mc-panel-title">{{ t2('resources.title', 'Web links') }}</span>
          </div>
          <div class="px-4 py-3 text-xs"><ResourceCard /></div>
        </div>
      </template>
    </div>

    <InspectorPanel
      :title="t2('diagnostics.section_about', 'About this host')"
      :subtitle="t2('diagnostics.about_sub', 'What a bug report needs')"
      :tag="reportVersion"
      tag-kind="ok"
    >
      <div class="grid grid-cols-[92px_minmax(0,1fr)] gap-x-3 gap-y-1.5 px-[18px] py-3 text-xs">
        <template v-for="[k, v] in reportRows" :key="k">
          <span class="text-ink-3">{{ k }}</span>
          <span class="break-words text-ink">{{ v }}</span>
        </template>
      </div>
      <div class="flex items-center gap-2 px-[18px] pb-3">
        <NButton size="tiny" @click="copyReport('plain')"
          ><NavIcon name="copy" :size="12" />{{ t2('about.copy_diagnostics', 'Copy') }}</NButton
        >
        <NButton size="tiny" @click="copyReport('markdown')"
          ><NavIcon name="copy" :size="12" />{{
            t2('about.copy_markdown', 'Copy as Markdown')
          }}</NButton
        >
        <div class="flex-1"></div>
        <button type="button" class="mc-panel-link" @click="go('about')">
          {{ t2('diagnostics.full_report', 'Full report') }}
        </button>
      </div>

      <div class="mc-kicker px-[18px] pb-1 pt-2">
        {{ t2('diagnostics.resets', 'Reset & clear') }}
      </div>
      <button
        v-for="a in resets"
        :key="a.id"
        type="button"
        class="mc-row mc-row-clickable w-full text-left"
        :disabled="resetPending !== null"
        @click="confirmReset(a)"
      >
        <div class="min-w-0">
          <div class="mc-row-primary truncate">{{ a.label }}</div>
          <div class="mc-row-secondary truncate">{{ a.desc }}</div>
        </div>
        <NavIcon name="chevron" :size="14" class="shrink-0 text-ink-4" />
      </button>
    </InspectorPanel>
  </div>
</template>
