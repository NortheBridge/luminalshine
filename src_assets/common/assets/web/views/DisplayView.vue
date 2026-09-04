<script setup lang="ts">
/**
 * Display — the LuminalVGD virtual display and display configuration in one
 * place: driver state, outputs, the four config groups from the old control
 * panel, the golden display snapshot, and a driver diagnostic in the
 * inspector. `?sec=about` still opens the driver's About page.
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { storeToRefs } from 'pinia';
import {
  NAlert,
  NButton,
  NInputNumber,
  NSelect,
  NSlider,
  NSwitch,
  useDialog,
  useMessage,
} from 'naive-ui';
import { http } from '@/http';
import { useT2 } from '@/composables/useT2';
import { useConfigStore } from '@/stores/config';
import { useHostStore } from '@/stores/host';
import { useAuthStore } from '@/stores/auth';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import NavIcon from '@/components/shell/NavIcon.vue';
import VgdAboutView from '@/views/VgdAboutView.vue';

const t2 = useT2();
const route = useRoute();
const router = useRouter();
const store = useConfigStore();
const host = useHostStore();
const auth = useAuthStore();
const dialog = useDialog();
const message = useMessage();
const { metadata } = storeToRefs(store);
const config = store.config as Record<string, unknown>;

// ---- sections -------------------------------------------------------------
type Section = 'panel' | 'about';
const section = computed<Section>(() => (route.query['sec'] === 'about' ? 'about' : 'panel'));
function go(sec: Section): void {
  const query = { ...route.query };
  if (sec === 'about') query['sec'] = 'about';
  else delete query['sec'];
  void router.replace({ path: route.path, query });
}

// ---- driver state ---------------------------------------------------------
const isWindows = computed(
  () => String(metadata.value?.platform || '').toLowerCase() === 'windows',
);
const driverInstalled = computed(() => Boolean(metadata.value?.vgd_installed));
const backend = computed(() => String(metadata.value?.virtual_display_backend || ''));
const backendLabel = computed(() =>
  backend.value === 'luminalvgd'
    ? 'LuminalVGD'
    : backend.value
      ? backend.value
      : t2('display.no_backend', 'no virtual display backend'),
);
const driverVersion = computed(() => String(metadata.value?.virtual_display_backend_version || ''));
const bundledVersion = computed(() => String(metadata.value?.vgd_bundled_version || ''));
const driverReady = computed(() => metadata.value?.virtual_display_driver_ready);
const driverStatusText = computed(() => {
  const s = metadata.value?.virtual_display_driver_status;
  if (driverReady.value === true) return t2('display.status_ready', 'ready');
  if (typeof s === 'string' && s) return s;
  if (typeof s === 'number') return String(s);
  return driverInstalled.value
    ? t2('display.status_unknown', 'unknown')
    : t2('display.status_missing', 'not installed');
});
const showTransition = computed(() => Boolean(backend.value) && backend.value !== 'luminalvgd');

// ---- outputs (/api/display-devices?detail=full) ------------------------------
interface DeviceInfo {
  resolution?: { width?: number; height?: number } | null;
  refresh_rate?: number | { numerator?: number; denominator?: number } | null;
  primary?: boolean;
  hdr_state?: string | null;
  [k: string]: unknown;
}
interface DisplayDevice {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: DeviceInfo | null;
}
const devices = ref<DisplayDevice[]>([]);
const devicesLoading = ref(false);
const devicesError = ref('');
async function loadDevices(): Promise<void> {
  if (!isWindows.value) return;
  devicesLoading.value = true;
  devicesError.value = '';
  try {
    const r = await http.get<DisplayDevice[]>('/api/display-devices', {
      params: { detail: 'full' },
      validateStatus: () => true,
    });
    devices.value = r.status === 200 && Array.isArray(r.data) ? r.data : [];
    if (r.status !== 200) devicesError.value = `HTTP ${r.status}`;
  } catch (e) {
    devicesError.value = e instanceof Error ? e.message : 'Failed to load display devices';
    devices.value = [];
  } finally {
    devicesLoading.value = false;
  }
}
function refreshText(rr: DeviceInfo['refresh_rate']): string {
  if (typeof rr === 'number' && Number.isFinite(rr)) return `@${Math.round(rr * 1000) / 1000}`;
  if (rr && typeof rr === 'object') {
    const n = Number(rr.numerator);
    const d = Number(rr.denominator);
    if (Number.isFinite(n) && Number.isFinite(d) && d > 0)
      return `@${Math.round((n / d) * 1000) / 1000}`;
  }
  return '';
}
interface OutputRow {
  id: string;
  name: string;
  active: boolean;
  primary: boolean;
  virtual: boolean;
  mode: string;
}
const outputs = computed<OutputRow[]>(() => {
  const rows: OutputRow[] = [];
  const seen = new Set<string>();
  for (const d of devices.value) {
    const id = d.device_id || d.display_name || '';
    if (!id || seen.has(id)) continue;
    seen.add(id);
    const name = d.friendly_name || d.display_name || 'Display';
    const info = d.info && typeof d.info === 'object' ? d.info : null;
    const active = !!info;
    const primary = info?.primary === true;
    const res = info?.resolution;
    const modeParts: string[] = [];
    if (res && typeof res.width === 'number' && typeof res.height === 'number') {
      modeParts.push(
        `${res.width}×${res.height}${refreshText(info?.refresh_rate) ? ` ${refreshText(info?.refresh_rate)}` : ''}`,
      );
    }
    const hdr = String(info?.hdr_state || '').toLowerCase();
    if (hdr === 'enabled') modeParts.push('HDR');
    const virtual = /luminal|vgd|virtual/i.test(`${name} ${id}`);
    rows.push({ id, name, active, primary, virtual, mode: modeParts.join(' · ') });
  }
  rows.sort((a, b) => Number(b.virtual) - Number(a.virtual) || Number(b.active) - Number(a.active));
  return rows;
});

// ---- config groups (ported from the LuminalVGD control panel) --------------
interface PanelItem {
  key: string;
  label: string;
  hint: string;
  type: 'switch' | 'select' | 'number' | 'slider';
  options?: { label: string; value: string }[];
  suffix?: string;
  fallback?: string | number;
  boolEncoding?: 'string';
  min?: number;
  max?: number;
  marks?: Record<number, string>;
  hdrZoneFrom?: number;
}
interface PanelGroup {
  id: string;
  title: string;
  items: PanelItem[];
  footnote?: string;
}
function toNumber(value: unknown, fallback: number): number {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}
function setOption(key: string, value: unknown): void {
  config[key] = value;
}
// Some boolean options are stored as 'enabled'/'disabled' strings (matching
// the config-file encoding the rest of the UI uses); Boolean() on those
// strings is always true, so switches must honor the declared encoding.
function boolFromConfig(item: PanelItem): boolean {
  const value = config[item.key];
  if (item.boolEncoding === 'string') return value === 'enabled';
  return Boolean(value);
}
function setBool(item: PanelItem, v: boolean): void {
  setOption(item.key, item.boolEncoding === 'string' ? (v ? 'enabled' : 'disabled') : v);
}
function selectValue(item: PanelItem): string {
  return String(config[item.key] ?? item.fallback ?? '');
}
function numberValue(item: PanelItem): number {
  return toNumber(config[item.key], Number(item.fallback ?? 0));
}

// Only options with real config backing appear here; driver-internal
// tunables (ring depth, mutex timeouts, EDID fields, watchdog cadence) are
// deliberately not exposed — the driver's defaults are the contract.
const groups = computed<PanelGroup[]>(() => [
  {
    id: 'backend',
    title: t2('vgd.group_backend', 'Backend & sessions'),
    items: [
      {
        key: 'virtual_display_backend',
        label: t2('vgd.backend', 'Virtual display backend'),
        hint: t2(
          'vgd.backend_hint',
          'LuminalVGD is the first-party driver and the automatic choice when installed.',
        ),
        type: 'select',
        fallback: 'auto',
        options: [
          { label: t2('vgd.backend_auto', 'Automatic (recommended)'), value: 'auto' },
          { label: 'LuminalVGD', value: 'luminalvgd' },
        ],
      },
      {
        key: 'virtual_display_mode',
        label: t2('vgd.mode', 'Virtual display mode'),
        hint: t2(
          'vgd.mode_hint',
          'Per client gives every client its own virtual monitor; shared reuses one.',
        ),
        type: 'select',
        fallback: 'per_client',
        options: [
          { label: t2('vgd.mode_disabled', 'Disabled'), value: 'disabled' },
          { label: t2('vgd.mode_per_client', 'Per client'), value: 'per_client' },
          { label: t2('vgd.mode_shared', 'Shared'), value: 'shared' },
        ],
      },
      {
        key: 'virtual_display_layout',
        label: t2('vgd.layout', 'Display layout'),
        hint: t2(
          'vgd.layout_hint',
          'How the virtual display joins the desktop while streaming; exclusive turns physical monitors off.',
        ),
        type: 'select',
        fallback: 'exclusive',
        options: [
          { label: t2('vgd.layout_exclusive', 'Exclusive'), value: 'exclusive' },
          { label: t2('vgd.layout_extended', 'Extended'), value: 'extended' },
          {
            label: t2('vgd.layout_extended_primary', 'Extended (primary)'),
            value: 'extended_primary',
          },
          {
            label: t2('vgd.layout_extended_isolated', 'Extended (isolated)'),
            value: 'extended_isolated',
          },
          {
            label: t2('vgd.layout_extended_primary_isolated', 'Extended (primary, isolated)'),
            value: 'extended_primary_isolated',
          },
        ],
      },
      {
        key: 'dd_activate_virtual_display',
        label: t2('vgd.activate', 'Activate virtual display'),
        hint: t2(
          'vgd.activate_hint',
          'Bring the virtual display into the desktop topology when a session starts.',
        ),
        type: 'switch',
      },
    ],
  },
  {
    id: 'lifetime',
    title: t2('vgd.group_lifetime', 'Disconnect & pause behavior'),
    items: [
      {
        key: 'dd_config_revert_on_disconnect',
        label: t2('vgd.revert_on_disconnect', 'Revert display config on disconnect'),
        hint: t2(
          'vgd.revert_on_disconnect_hint',
          'Restore physical monitors as soon as the client disconnects instead of keeping the session paused.',
        ),
        type: 'switch',
        boolEncoding: 'string',
      },
      {
        key: 'dd_paused_virtual_display_timeout_secs',
        label: t2('vgd.keep_paused', 'Paused display timeout'),
        hint: t2(
          'vgd.keep_paused_hint',
          'How long a paused stream keeps its virtual display alive; 0 keeps it until resume.',
        ),
        type: 'number',
        fallback: 0,
        min: 0,
        suffix: 's',
      },
    ],
  },
  {
    id: 'modes',
    title: t2('vgd.group_modes', 'Modes & refresh'),
    items: [
      {
        key: 'dd_wa_virtual_double_refresh',
        label: t2('vgd.refresh_doubling', 'Refresh doubling'),
        hint: t2(
          'vgd.refresh_doubling_hint',
          'Advertise 2× the client refresh rate on the virtual display (frame-generation workaround).',
        ),
        type: 'switch',
      },
    ],
    footnote: t2(
      'vgd.modes_footnote',
      'The virtual display advertises the client-native mode automatically (up to four exact modes per monitor, millihertz-precise refresh, HDR10 when the client and driver support it).',
    ),
  },
  {
    id: 'hdr',
    title: t2('vgd.group_hdr', 'HDR'),
    items: [
      {
        key: 'vgd_hdr_peak_nits',
        label: t2('config.vgd_hdr_peak_nits', 'HDR peak brightness (nits)'),
        hint: t2(
          'vgd.hdr_peak_nits_hint',
          'Peak luminance the virtual display advertises to Windows and HDR-aware games. Saves automatically; applies to the next streaming session.',
        ),
        type: 'slider',
        fallback: 800,
        min: 0,
        max: 1000,
        suffix: 'nits',
        hdrZoneFrom: 790,
        marks: { 790: 'HDR', 1000: '1000' },
      },
    ],
    footnote: t2(
      'vgd.hdr_footnote',
      'Requires LuminalVGD driver build 15 or newer (older drivers use their built-in 993 nits). The EDID stores brightness on a ~2% logarithmic scale — 800 encodes exactly; 1000 encodes as 993.',
    ),
  },
]);
const columns = computed(() => [
  groups.value.filter((g) => g.id === 'backend' || g.id === 'modes'),
  groups.value.filter((g) => g.id === 'lifetime' || g.id === 'hdr'),
]);

// ---- transition -----------------------------------------------------------
const transitionPending = ref(false);
const transitionResult = ref('');
const metadataTimers: Array<ReturnType<typeof setTimeout>> = [];
function refreshMetadataLater(delays: number[]): void {
  for (const delay of delays)
    metadataTimers.push(setTimeout(() => void store.fetchMetadata(), delay));
}
async function forceTransition(): Promise<void> {
  transitionPending.value = true;
  transitionResult.value = '';
  try {
    const r = await http.post('./api/state/vgd-transition', {}, { validateStatus: () => true });
    const body = (r.data || {}) as { status?: boolean; result?: string };
    const result = typeof body.result === 'string' ? body.result : '';
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      transitionResult.value =
        result === 'already_vgd'
          ? t2('vgd.transition_already', 'Already on the LuminalVGD backend.')
          : t2(
              'vgd.transition_started',
              'Transition attempt started — this page refreshes as it completes.',
            );
    } else {
      const reasons: Record<string, string> = {
        busy: t2('vgd.transition_busy', 'A transition or driver rebind is already in progress.'),
        stack_down: t2(
          'vgd.transition_stack_down',
          'The display stack is down (reboot required); transition deferred.',
        ),
        shutting_down: t2('vgd.transition_shutdown', 'The service is shutting down.'),
      };
      transitionResult.value =
        reasons[result] ??
        `${t2('vgd.transition_failed', 'Could not start a transition attempt')} (${result || `HTTP ${r.status}`}).`;
    }
  } catch (e) {
    transitionResult.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    transitionPending.value = false;
    // The attempt runs in the background; refresh the metadata snapshot a
    // few times so a successful flip updates this page unprompted.
    refreshMetadataLater([3000, 10000, 30000]);
  }
}

// ---- golden snapshot ------------------------------------------------------
interface GoldenStatus {
  exists?: boolean;
  snapshot_version?: number | null;
  latest_snapshot_version?: number;
  has_layout?: boolean;
  needs_layout_upgrade?: boolean;
}
const golden = ref<GoldenStatus | null>(null);
const goldenBusy = ref(false);
async function loadGolden(): Promise<void> {
  if (!isWindows.value) return;
  try {
    const r = await http.get('/api/display/golden_status', { validateStatus: () => true });
    golden.value = r.status === 200 && r.data ? (r.data as GoldenStatus) : null;
  } catch {
    golden.value = null;
  }
}
const goldenText = computed(() => {
  const g = golden.value;
  if (!g) return t2('display.golden_unknown', 'status unknown');
  if (!g.exists) return t2('display.golden_missing', 'not captured');
  if (g.needs_layout_upgrade)
    return t2('display.golden_upgrade', 'needs an update (predates layout support)');
  const v = g.snapshot_version != null ? ` · v${g.snapshot_version}` : '';
  return `${t2('display.golden_current', 'current')}${v}`;
});
async function captureGolden(): Promise<void> {
  if (goldenBusy.value) return;
  goldenBusy.value = true;
  try {
    const r = await http.post('/api/display/export_golden', {}, { validateStatus: () => true });
    const body = (r.data || {}) as { status?: boolean; error?: string; message?: string };
    if (r.status >= 200 && r.status < 300 && body.status === true) {
      message.success(
        t2('troubleshooting.dd_export_golden_success', 'Golden display snapshot captured.'),
      );
    } else {
      message.error(
        body.error ||
          body.message ||
          t2('troubleshooting.dd_export_golden_error', 'Failed to capture the golden snapshot.'),
      );
    }
  } catch (e) {
    message.error(
      e instanceof Error
        ? e.message
        : t2('troubleshooting.dd_export_golden_error', 'Failed to capture the golden snapshot.'),
    );
  } finally {
    goldenBusy.value = false;
    await loadGolden();
    await host.refreshHealth();
  }
}
function confirmDeleteGolden(): void {
  dialog.warning({
    title: t2('troubleshooting.dd_golden_delete', 'Delete golden snapshot'),
    content: t2(
      'display.golden_delete_body',
      'The saved display topology is removed; the next capture starts from the current layout.',
    ),
    positiveText: t2('troubleshooting.dd_golden_delete', 'Delete'),
    negativeText: t2('_common.cancel', 'Cancel'),
    onPositiveClick: async () => {
      try {
        const r = await http.delete('/api/display/golden', { validateStatus: () => true });
        const body = (r.data || {}) as { deleted?: boolean };
        if (r.status >= 200 && r.status < 300 && body.deleted !== false) {
          message.success(t2('troubleshooting.dd_golden_deleted', 'Golden snapshot deleted.'));
        } else {
          message.error(
            t2('troubleshooting.dd_golden_delete_error', 'Failed to delete the golden snapshot.'),
          );
        }
      } catch (e) {
        message.error(
          e instanceof Error
            ? e.message
            : t2('troubleshooting.dd_golden_delete_error', 'Failed to delete the golden snapshot.'),
        );
      }
      await loadGolden();
      await host.refreshHealth();
    },
  });
}

// ---- driver diagnostic / restart (inspector) -------------------------------
interface VddDiagnostic {
  status?: boolean;
  driver_reachable?: boolean;
  handshake_ok?: boolean;
  driver_version?: string;
  last_error?: string;
  device_present?: boolean;
  instance_id?: string;
  hardware_ids?: string[] | string;
  status_string?: string;
  problem_code?: number | string;
  last_recovery_at?: string | number;
  last_recovery_level?: string;
  last_recovery_message?: string;
}
const diag = ref<VddDiagnostic | null>(null);
const diagLoading = ref(false);
const diagError = ref('');
const diagAt = ref<Date | null>(null);
async function runDiagnostic(): Promise<void> {
  if (diagLoading.value) return;
  diagLoading.value = true;
  diagError.value = '';
  try {
    const r = await http.get('./api/state/vdd-diagnostic', { validateStatus: () => true });
    if (r.status >= 200 && r.status < 300 && r.data && typeof r.data === 'object') {
      diag.value = r.data as VddDiagnostic;
      diagAt.value = new Date();
    } else {
      diagError.value = `HTTP ${r.status}`;
    }
  } catch (e) {
    diagError.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    diagLoading.value = false;
  }
}
const diagOk = computed(
  () => !!diag.value && diag.value.driver_reachable === true && diag.value.handshake_ok !== false,
);
const restartPending = ref(false);
function confirmRestartDriver(): void {
  dialog.warning({
    title: t2('display.restart_title', 'Restart the LuminalVGD driver?'),
    content: t2(
      'display.restart_body',
      'The virtual display is re-created. An active stream will drop for a moment.',
    ),
    positiveText: t2('display.restart', 'Restart driver'),
    negativeText: t2('_common.cancel', 'Cancel'),
    onPositiveClick: async () => {
      restartPending.value = true;
      try {
        const r = await http.post('./api/state/vdd-restart', {}, { validateStatus: () => true });
        const body = (r.data || {}) as { status?: boolean; message?: string; level?: string };
        if (r.status >= 200 && r.status < 300 && body.status !== false) {
          message.success(body.message || t2('display.restart_done', 'Driver restart requested.'));
        } else {
          message.error(body.message || t2('display.restart_failed', 'Driver restart failed.'));
        }
      } catch (e) {
        message.error(
          e instanceof Error ? e.message : t2('display.restart_failed', 'Driver restart failed.'),
        );
      } finally {
        restartPending.value = false;
        refreshMetadataLater([3000, 10000]);
        void host.refreshHealth();
      }
    },
  });
}
function yesNo(v: unknown): string {
  return v === true ? t2('display.yes', 'yes') : v === false ? t2('display.no', 'no') : '';
}
const diagRows = computed<Array<[string, string]>>(() => {
  const d = diag.value;
  if (!d) return [];
  const rows: Array<[string, string]> = [];
  const push = (k: string, v: unknown) => {
    if (v === undefined || v === null || v === '') return;
    rows.push([k, String(v)]);
  };
  push(t2('display.d_reachable', 'Driver reachable'), yesNo(d.driver_reachable));
  push(t2('display.d_handshake', 'Handshake'), yesNo(d.handshake_ok));
  push(t2('display.d_device', 'Device present'), yesNo(d.device_present));
  push(t2('display.d_version', 'Driver version'), d.driver_version);
  push(t2('display.d_status', 'Device status'), d.status_string);
  push(t2('display.d_problem', 'Problem code'), d.problem_code);
  push(t2('display.d_instance', 'Instance'), d.instance_id);
  push(
    t2('display.d_hwid', 'Hardware IDs'),
    Array.isArray(d.hardware_ids) ? d.hardware_ids.join(', ') : d.hardware_ids,
  );
  push(t2('display.d_error', 'Last error'), d.last_error);
  push(
    t2('display.d_recovery', 'Last recovery'),
    [d.last_recovery_level, d.last_recovery_message, d.last_recovery_at]
      .filter(Boolean)
      .join(' · '),
  );
  return rows;
});
const captureRows = computed<Array<[string, string]>>(() => {
  const m = metadata.value ?? {};
  const rows: Array<[string, string]> = [];
  if (m.capture_backend_active)
    rows.push([t2('display.c_active', 'Capture'), String(m.capture_backend_active)]);
  if (m.capture_backend_display)
    rows.push([t2('display.c_display', 'Display'), String(m.capture_backend_display)]);
  if (typeof m.capture_backend_age_seconds === 'number')
    rows.push([t2('display.c_age', 'Since'), `${m.capture_backend_age_seconds} s`]);
  if (m.vgd_handshake) rows.push([t2('display.c_handshake', 'Handshake'), String(m.vgd_handshake)]);
  if (typeof m.vgd_hdr10 === 'boolean') rows.push(['HDR10', yesNo(m.vgd_hdr10)]);
  return rows;
});

onMounted(async () => {
  await auth.waitForAuthentication();
  if (!host.running) void host.start();
  await store.fetchMetadata();
  await Promise.all([loadDevices(), loadGolden()]);
});
onBeforeUnmount(() => {
  for (const tmr of metadataTimers) clearTimeout(tmr);
});
</script>

<template>
  <div class="flex flex-col gap-3.5">
    <div class="flex flex-wrap items-center gap-3">
      <div>
        <div class="mc-page-title">{{ t2('shell.nav_display', 'Display') }}</div>
        <div class="mc-page-sub">
          {{ backendLabel }}<span v-if="driverVersion"> {{ driverVersion }}</span> ·
          {{ driverStatusText }}
        </div>
      </div>
      <div class="flex-1"></div>
      <div class="mc-chips" role="tablist">
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'panel' }"
          role="tab"
          :aria-selected="section === 'panel'"
          @click="go('panel')"
        >
          {{ t2('vgd.nav_control_panel', 'Control panel') }}
        </button>
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'about' }"
          role="tab"
          :aria-selected="section === 'about'"
          @click="go('about')"
        >
          {{ t2('vgd.nav_about', 'About the driver') }}
        </button>
      </div>
      <template v-if="section === 'panel' && isWindows">
        <NButton size="small" :loading="diagLoading" @click="runDiagnostic"
          ><NavIcon name="diagnostics" :size="14" />{{
            t2('display.run_diagnostic', 'Run diagnostic')
          }}</NButton
        >
        <NButton
          size="small"
          :loading="restartPending"
          :disabled="!driverInstalled"
          @click="confirmRestartDriver"
          ><NavIcon name="refresh" :size="14" />{{
            t2('display.restart', 'Restart driver')
          }}</NButton
        >
      </template>
    </div>

    <VgdAboutView v-if="section === 'about'" />
    <template v-else>
      <NAlert v-if="isWindows && !driverInstalled" type="warning" :show-icon="true">
        {{
          t2(
            'vgd.panel_driver_missing',
            'The LuminalVGD driver is not detected — these settings take effect once it is installed.',
          )
        }}
      </NAlert>
      <NAlert v-if="showTransition" type="info" :show-icon="true">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <span class="min-w-0">{{
            t2(
              'vgd.transition_prompt',
              'The host is not using the LuminalVGD backend (WGC fallback capture). It transitions automatically once the driver is available — or force an attempt now.',
            )
          }}</span>
          <NButton
            size="small"
            type="primary"
            :loading="transitionPending"
            @click="forceTransition"
            >{{ t2('vgd.transition_now', 'Transition to VGD now') }}</NButton
          >
        </div>
        <p v-if="transitionResult" class="mt-1 text-xs opacity-70">{{ transitionResult }}</p>
      </NAlert>

      <div class="grid grid-cols-1 gap-3.5 lg:grid-cols-2">
        <section class="mc-panel">
          <div class="mc-panel-h">
            <span class="mc-panel-title">{{ t2('display.driver', 'Driver') }}</span>
            <button type="button" class="mc-panel-link" @click="go('about')">
              {{ t2('vgd.nav_about', 'About the driver') }}
            </button>
          </div>
          <div class="mc-check">
            <span class="mc-check-label"
              ><span class="mc-dot" :class="driverInstalled ? '' : 'mc-dot-grey'"></span
              >{{ t2('display.installed', 'Installed') }}</span
            >
            <span class="mc-check-value">{{
              driverInstalled
                ? driverVersion || t2('display.yes', 'yes')
                : t2('display.status_missing', 'not installed')
            }}</span>
          </div>
          <div v-if="bundledVersion" class="mc-check">
            <span class="mc-check-label"
              ><span
                class="mc-dot"
                :class="driverVersion && driverVersion !== bundledVersion ? 'mc-dot-gold' : ''"
              ></span
              >{{ t2('display.shipped', 'Shipped with LuminalShine') }}</span
            >
            <span class="mc-check-value"
              >{{ bundledVersion
              }}{{
                driverVersion && driverVersion === bundledVersion
                  ? ` · ${t2('display.match', 'match')}`
                  : ''
              }}</span
            >
          </div>
          <div class="mc-check">
            <span class="mc-check-label"
              ><span
                class="mc-dot"
                :class="
                  driverReady === true ? '' : driverReady === false ? 'mc-dot-red' : 'mc-dot-grey'
                "
              ></span
              >{{ t2('display.status', 'Status') }}</span
            >
            <span class="mc-check-value">{{ driverStatusText }}</span>
          </div>
          <div class="mc-check">
            <span class="mc-check-label"
              ><span class="mc-dot" :class="backend === 'luminalvgd' ? '' : 'mc-dot-gold'"></span
              >{{ t2('display.backend_active', 'Active backend') }}</span
            >
            <span class="mc-check-value">{{ backendLabel }}</span>
          </div>
          <div v-if="metadata?.vgd_bundled_signature" class="mc-check">
            <span class="mc-check-label"
              ><span class="mc-dot"></span>{{ t2('display.signature', 'Signature') }}</span
            >
            <span class="mc-check-value">{{ metadata.vgd_bundled_signature }}</span>
          </div>
        </section>

        <section class="mc-panel">
          <div class="mc-panel-h">
            <span class="mc-panel-title">{{ t2('display.outputs', 'Outputs') }}</span>
            <button
              type="button"
              class="mc-panel-link"
              :disabled="devicesLoading"
              @click="loadDevices"
            >
              {{ t2('_common.refresh', 'Refresh') }}
            </button>
          </div>
          <div v-if="!isWindows" class="px-4 py-3 text-xs text-ink-4">
            {{ t2('display.windows_only', 'Display management is available on Windows hosts.') }}
          </div>
          <div v-else-if="devicesError" class="px-4 py-3 text-xs text-danger">
            {{ devicesError }}
          </div>
          <div
            v-else-if="!devicesLoading && outputs.length === 0"
            class="px-4 py-3 text-xs text-ink-4"
          >
            {{ t2('display.no_outputs', 'No display outputs reported.') }}
          </div>
          <div v-for="o in outputs" :key="o.id" class="mc-row">
            <div class="min-w-0">
              <div class="mc-row-primary truncate">
                {{ o.name
                }}<span v-if="o.primary" class="ml-1.5 text-ink-4"
                  >· {{ t2('display.primary', 'primary') }}</span
                >
              </div>
              <div class="mc-row-secondary truncate">{{ o.mode || o.id }}</div>
            </div>
            <span
              class="mc-tag"
              :class="o.virtual && o.active ? 'mc-tag-live' : o.active ? 'mc-tag-ok' : ''"
            >
              {{
                o.virtual
                  ? `${t2('display.virtual', 'Virtual')}${o.active ? '' : ` · ${t2('display.inactive', 'inactive')}`}`
                  : o.active
                    ? t2('display.active', 'Active')
                    : t2('display.inactive', 'Inactive')
              }}
            </span>
          </div>
        </section>
      </div>

      <div class="grid grid-cols-1 gap-3.5 lg:grid-cols-2">
        <div v-for="(col, ci) in columns" :key="ci" class="flex flex-col gap-3.5">
          <section v-for="g in col" :key="g.id" class="mc-panel">
            <div class="mc-panel-h">
              <span class="mc-panel-title">{{ g.title }}</span
              ><span v-if="g.id === 'hdr'" class="mc-tag mc-tag-warn">HDR</span>
            </div>
            <div v-for="item in g.items" :key="item.key" class="mc-field">
              <template v-if="item.type === 'switch'">
                <div class="flex items-center justify-between gap-3">
                  <span class="mc-field-label">{{ item.label }}</span>
                  <NSwitch
                    :value="boolFromConfig(item)"
                    size="small"
                    @update:value="(v: boolean) => setBool(item, v)"
                  />
                </div>
                <span class="mc-field-hint">{{ item.hint }}</span>
              </template>
              <template v-else-if="item.type === 'select'">
                <span class="mc-field-label">{{ item.label }}</span>
                <NSelect
                  :value="selectValue(item)"
                  :options="item.options ?? []"
                  size="small"
                  @update:value="(v: string) => setOption(item.key, v)"
                />
                <span class="mc-field-hint">{{ item.hint }}</span>
              </template>
              <template v-else-if="item.type === 'number'">
                <span class="mc-field-label">{{ item.label }}</span>
                <NInputNumber
                  :value="numberValue(item)"
                  :min="item.min ?? 0"
                  :max="item.max ?? Number.MAX_SAFE_INTEGER"
                  size="small"
                  @update:value="
                    (v: number | null) => setOption(item.key, v ?? Number(item.fallback ?? 0))
                  "
                >
                  <template v-if="item.suffix" #suffix>{{ item.suffix }}</template>
                </NInputNumber>
                <span class="mc-field-hint">{{ item.hint }}</span>
              </template>
              <template v-else>
                <div class="flex items-center justify-between gap-3">
                  <span class="mc-field-label">{{ item.label }}</span>
                  <span class="font-mono text-xs"
                    >{{ numberValue(item) }} {{ item.suffix
                    }}<span
                      v-if="item.hdrZoneFrom !== undefined && numberValue(item) >= item.hdrZoneFrom"
                      class="mc-tag mc-tag-warn ml-2"
                      >HDR</span
                    ></span
                  >
                </div>
                <NSlider
                  :value="numberValue(item)"
                  :min="item.min ?? 0"
                  :max="item.max ?? 100"
                  :step="1"
                  :marks="item.marks ?? {}"
                  @update:value="(v: number) => setOption(item.key, v)"
                />
                <span class="mc-field-hint">{{ item.hint }}</span>
              </template>
            </div>
            <p v-if="g.footnote" class="px-[18px] pb-3 pt-1 text-[11px] leading-snug text-ink-4">
              {{ g.footnote }}
            </p>
          </section>
        </div>
      </div>

      <section v-if="isWindows" class="mc-panel">
        <div class="mc-panel-h">
          <span class="mc-panel-title">{{
            t2('troubleshooting.dd_golden_title', 'Golden display snapshot')
          }}</span>
          <div class="flex items-center gap-3">
            <button
              v-if="golden?.exists"
              type="button"
              class="mc-panel-link text-danger"
              @click="confirmDeleteGolden"
            >
              {{ t2('troubleshooting.dd_golden_delete', 'Delete') }}
            </button>
            <button
              type="button"
              class="mc-panel-link"
              :disabled="goldenBusy"
              @click="captureGolden"
            >
              {{
                golden?.exists
                  ? t2('troubleshooting.dd_golden_recreate', 'Recapture')
                  : t2('troubleshooting.dd_golden_create', 'Capture now')
              }}
            </button>
          </div>
        </div>
        <div class="mc-check">
          <span class="mc-check-label"
            ><span
              class="mc-dot"
              :class="
                !golden || !golden.exists
                  ? 'mc-dot-grey'
                  : golden.needs_layout_upgrade
                    ? 'mc-dot-gold'
                    : ''
              "
            ></span
            >{{ goldenText }}</span
          >
          <span class="mc-check-value">{{
            t2(
              'troubleshooting.dd_golden_help',
              'The saved topology LuminalShine restores after a session.',
            )
          }}</span>
        </div>
      </section>

      <p class="text-[11px] text-ink-4">
        {{
          t2(
            'vgd.panel_autosave_note',
            'Changes save automatically and apply to the next streaming session.',
          )
        }}
      </p>
    </template>

    <InspectorPanel
      v-if="section === 'panel' && isWindows"
      :title="t2('display.inspector_title', 'Driver diagnostic')"
      :subtitle="
        diagAt
          ? `${t2('display.last_run', 'last run')} ${diagAt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`
          : t2('display.not_run', 'not run yet')
      "
      :tag="diag ? (diagOk ? t2('display.ok', 'OK') : t2('display.attention', 'Attention')) : ''"
      :tag-kind="diag ? (diagOk ? 'ok' : 'warn') : ''"
    >
      <div v-if="diagError" class="px-[18px] pt-3 text-xs text-danger">{{ diagError }}</div>
      <div v-else-if="!diag" class="px-[18px] pt-3 text-xs text-ink-4">
        {{
          t2(
            'display.diag_hint',
            'Run a diagnostic to query the driver: reachability, handshake, device state and the last recovery.',
          )
        }}
      </div>
      <div
        v-else
        class="grid grid-cols-[118px_minmax(0,1fr)] gap-x-3 gap-y-1.5 px-[18px] pb-2 pt-3 text-xs"
      >
        <template v-for="[k, v] in diagRows" :key="k">
          <span class="text-ink-3">{{ k }}</span>
          <span class="break-words text-ink">{{ v }}</span>
        </template>
      </div>
      <template v-if="captureRows.length">
        <div class="mc-kicker px-[18px] pb-1 pt-3">{{ t2('display.capture', 'Capture') }}</div>
        <div class="grid grid-cols-[118px_minmax(0,1fr)] gap-x-3 gap-y-1.5 px-[18px] pb-3 text-xs">
          <template v-for="[k, v] in captureRows" :key="k">
            <span class="text-ink-3">{{ k }}</span>
            <span class="break-words text-ink">{{ v }}</span>
          </template>
        </div>
      </template>
      <template #footer>
        <NButton
          size="small"
          :loading="restartPending"
          :disabled="!driverInstalled"
          @click="confirmRestartDriver"
          >{{ t2('display.restart', 'Restart driver') }}</NButton
        >
        <NButton size="small" type="primary" :loading="diagLoading" @click="runDiagnostic">{{
          t2('display.run_diagnostic', 'Run diagnostic')
        }}</NButton>
      </template>
    </InspectorPanel>
  </div>
</template>
