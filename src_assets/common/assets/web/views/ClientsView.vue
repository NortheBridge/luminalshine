<script setup lang="ts">
/**
 * Clients — paired devices as a table with display and HDR overrides visible
 * at a glance; the per-client editor lives in the inspector. Trusted web
 * sessions and API tokens keep their home here as sections.
 */
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { useI18n } from 'vue-i18n';
import {
  NButton,
  NInput,
  NModal,
  NRadio,
  NRadioGroup,
  NSelect,
  NSwitch,
  useDialog,
  useMessage,
} from 'naive-ui';
import { http } from '@/http';
import { useT2 } from '@/composables/useT2';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import { useHostStore } from '@/stores/host';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import NavIcon from '@/components/shell/NavIcon.vue';
import TrustedDevicesCard from '@/components/TrustedDevicesCard.vue';
import ApiTokenManager from '@/ApiTokenManager.vue';
import AppEditConfigOverridesSection from '@/components/app-edit/AppEditConfigOverridesSection.vue';

const { t } = useI18n();
const t2 = useT2();
const route = useRoute();
const router = useRouter();
const auth = useAuthStore();
const configStore = useConfigStore();
const host = useHostStore();
const dialog = useDialog();
const message = useMessage();

// ---- types ----------------------------------------------------------------
type DisplaySelection = 'virtual' | 'physical';
type VirtualMode = 'disabled' | 'per_client' | 'shared' | 'global' | null;
type VirtualLayout =
  | 'exclusive'
  | 'extended'
  | 'extended_primary'
  | 'extended_isolated'
  | 'extended_primary_isolated'
  | null;
type Prefer10Bit = 'enabled' | 'disabled' | null;

interface ClientApiEntry {
  uuid?: string;
  name?: string;
  connected?: boolean;
  last_seen?: number | string | null;
  hdr_profile?: string;
  display_mode?: string;
  output_name_override?: string;
  always_use_virtual_display?: boolean | string | number;
  virtual_display_mode?: string;
  virtual_display_layout?: string;
  prefer_10bit_sdr?: boolean | string | number | null;
  config_overrides?: Record<string, unknown> | null;
}
interface ClientsListResponse {
  status?: boolean;
  named_certs?: ClientApiEntry[];
  platform?: string;
}
interface HdrProfileEntry {
  filename?: string;
  added_ms?: number;
}
interface DisplayDevice {
  device_id?: string;
  display_name?: string;
  friendly_name?: string;
  info?: unknown;
}

interface Client {
  uuid: string;
  name: string;
  connected: boolean;
  lastSeen: number | null;
  hdrProfile: string;
  displayMode: string;
  outputOverride: string;
  alwaysUseVirtualDisplay: boolean;
  prefer10BitSdr: Prefer10Bit;
  virtualDisplayMode: VirtualMode;
  virtualDisplayLayout: VirtualLayout;
  configOverrides: Record<string, unknown>;
}

interface Draft {
  uuid: string;
  name: string;
  displayMode: string;
  overrideEnabled: boolean;
  selection: DisplaySelection;
  physicalOutput: string | null;
  virtualMode: VirtualMode;
  virtualLayout: VirtualLayout;
  hdrProfile: string;
  prefer10BitSdr: Prefer10Bit;
  overrides: Record<string, unknown>;
}

// ---- helpers --------------------------------------------------------------
function toBool(value: unknown, fallback = false): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'number') return value !== 0;
  if (typeof value === 'string') {
    const v = value.trim().toLowerCase();
    if (['1', 'true', 'yes', 'on', 'enabled'].includes(v)) return true;
    if (['0', 'false', 'no', 'off', 'disabled', ''].includes(v)) return false;
  }
  return fallback;
}
function parseVirtualMode(value: unknown): VirtualMode {
  const v = String(value ?? '')
    .trim()
    .toLowerCase();
  if (v === 'disabled' || v === 'per_client' || v === 'shared' || v === 'global') return v;
  return null;
}
function parseVirtualLayout(value: unknown): VirtualLayout {
  const v = String(value ?? '')
    .trim()
    .toLowerCase();
  if (
    v === 'exclusive' ||
    v === 'extended' ||
    v === 'extended_primary' ||
    v === 'extended_isolated' ||
    v === 'extended_primary_isolated'
  )
    return v;
  return null;
}
function parseLastSeen(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value) && value > 0) return value;
  if (typeof value === 'string') {
    const n = Number(value);
    if (Number.isFinite(n) && n > 0) return n;
  }
  return null;
}
function clone<T>(v: T): T {
  return JSON.parse(JSON.stringify(v)) as T;
}

function toClient(entry: ClientApiEntry): Client {
  const overrides =
    entry.config_overrides &&
    typeof entry.config_overrides === 'object' &&
    !Array.isArray(entry.config_overrides)
      ? clone(entry.config_overrides)
      : {};
  return {
    uuid: entry.uuid ?? '',
    name: (entry.name ?? '').trim(),
    connected: !!entry.connected,
    lastSeen: parseLastSeen(entry.last_seen),
    hdrProfile: String(entry.hdr_profile ?? '').trim(),
    displayMode: entry.display_mode ?? '',
    outputOverride: entry.output_name_override ?? '',
    alwaysUseVirtualDisplay: toBool(entry.always_use_virtual_display, false),
    prefer10BitSdr:
      entry.prefer_10bit_sdr === undefined || entry.prefer_10bit_sdr === null
        ? null
        : toBool(entry.prefer_10bit_sdr, false)
          ? 'enabled'
          : 'disabled',
    virtualDisplayMode: parseVirtualMode(entry.virtual_display_mode ?? ''),
    virtualDisplayLayout: parseVirtualLayout(entry.virtual_display_layout ?? ''),
    configOverrides: overrides,
  };
}

function applySelection(d: Draft, selection: DisplaySelection): void {
  d.selection = selection;
  if (selection === 'physical') {
    d.virtualMode = 'disabled';
    d.virtualLayout = null;
    return;
  }
  d.physicalOutput = null;
  if (d.virtualMode === null || d.virtualMode === 'disabled') d.virtualMode = 'global';
}
function applyOverrideEnabled(d: Draft, enabled: boolean): void {
  d.overrideEnabled = enabled;
  if (!enabled) {
    d.selection = 'physical';
    d.physicalOutput = null;
    d.virtualMode = null;
    d.virtualLayout = null;
    return;
  }
  applySelection(d, d.selection);
}
function toDraft(c: Client): Draft {
  const overrideEnabled =
    c.alwaysUseVirtualDisplay ||
    !!c.outputOverride.trim() ||
    c.virtualDisplayMode !== null ||
    c.virtualDisplayLayout !== null;
  const selection: DisplaySelection =
    c.alwaysUseVirtualDisplay ||
    (c.virtualDisplayMode !== null && c.virtualDisplayMode !== 'disabled')
      ? 'virtual'
      : 'physical';
  const d: Draft = {
    uuid: c.uuid,
    name: c.name,
    displayMode: c.displayMode,
    overrideEnabled,
    selection,
    physicalOutput: c.outputOverride || null,
    virtualMode: c.virtualDisplayMode,
    virtualLayout: c.virtualDisplayLayout,
    hdrProfile: c.hdrProfile,
    prefer10BitSdr: c.prefer10BitSdr,
    overrides: clone(c.configOverrides || {}),
  };
  if (d.overrideEnabled) applySelection(d, d.selection);
  return d;
}

// ---- state ----------------------------------------------------------------
const isWindows = computed(
  () => String(configStore.metadata?.platform || '').toLowerCase() === 'windows',
);
const clients = ref<Client[]>([]);
const loaded = ref(false);
let refreshTimer: ReturnType<typeof setInterval> | null = null;

type Section = 'clients' | 'devices' | 'tokens';
const section = computed<Section>(() => {
  const s = route.query['sec'];
  return s === 'tokens' ? 'tokens' : s === 'devices' ? 'devices' : 'clients';
});
function go(sec: Section): void {
  const query = { ...route.query };
  if (sec === 'clients') delete query['sec'];
  else query['sec'] = sec;
  void router.replace({ path: route.path, query });
}

type SortMode = 'recent' | 'name';
const sortMode = ref<SortMode>('recent');
function compareByName(a: Client, b: Client): number {
  const na = a.name.toLowerCase();
  const nb = b.name.toLowerCase();
  if (na === nb) return a.uuid.localeCompare(b.uuid);
  if (na === '') return 1;
  if (nb === '') return -1;
  return na.localeCompare(nb);
}
const sorted = computed(() => {
  const list = [...clients.value];
  if (sortMode.value === 'recent') {
    list.sort((a, b) => {
      if (a.connected !== b.connected) return a.connected ? -1 : 1;
      const la = a.lastSeen ?? 0;
      const lb = b.lastSeen ?? 0;
      if (la !== lb) return lb - la;
      return compareByName(a, b);
    });
    return list;
  }
  return list.sort(compareByName);
});
const connectedCount = computed(() => clients.value.filter((c) => c.connected).length);

/** Returns true when the list was replaced by a fresh server response. */
async function refreshClients(): Promise<boolean> {
  if (!auth.isAuthenticated) return false;
  try {
    const r = await http.get<ClientsListResponse>('./api/clients/list', {
      validateStatus: () => true,
    });
    if (r.status !== 200) return false;
    const body = r.data ?? {};
    if (body.status === true && Array.isArray(body.named_certs)) {
      clients.value = body.named_certs
        .filter((e) => typeof e.uuid === 'string' && e.uuid)
        .map(toClient);
      return true;
    }
  } catch {
    /* keep the previous list */
  } finally {
    loaded.value = true;
  }
  return false;
}

// ---- formatting -----------------------------------------------------------
const timeFormatter = new Intl.DateTimeFormat(undefined, {
  dateStyle: 'medium',
  timeStyle: 'short',
});
function lastSeenText(c: Client): string {
  if (!c.lastSeen) return t('clients.last_seen_unknown');
  return timeFormatter.format(new Date(c.lastSeen * 1000));
}
function displayText(c: Client): string {
  const overrideEnabled =
    c.alwaysUseVirtualDisplay ||
    !!c.outputOverride.trim() ||
    c.virtualDisplayMode !== null ||
    c.virtualDisplayLayout !== null;
  if (!overrideEnabled) return t2('clients.follow_global', 'Follow global');
  const virtual =
    c.alwaysUseVirtualDisplay ||
    (c.virtualDisplayMode !== null && c.virtualDisplayMode !== 'disabled');
  if (!virtual) {
    return `${t('config.app_display_override_physical')}${c.outputOverride ? ` · ${c.outputOverride}` : ''}`;
  }
  const mode =
    c.virtualDisplayMode && c.virtualDisplayMode !== 'global'
      ? c.virtualDisplayMode.replace('_', ' ')
      : t2('clients.follow_global', 'Follow global').toLowerCase();
  const layout = c.virtualDisplayLayout ? ` · ${c.virtualDisplayLayout.replace(/_/g, ' ')}` : '';
  return `${t('config.app_display_override_virtual')} · ${mode}${layout}`;
}
function hdrText(c: Client): string {
  return c.hdrProfile || t('clients.hdr_profile_auto');
}
function nameOf(c: Client): string {
  return c.name || t('troubleshooting.unpair_single_unknown');
}

// ---- selection + draft ----------------------------------------------------
const selectedUuid = ref<string | null>(null);
const selected = computed(() => clients.value.find((c) => c.uuid === selectedUuid.value) ?? null);
const draft = reactive<{ value: Draft | null }>({ value: null });
function select(c: Client): void {
  selectedUuid.value = c.uuid;
  draft.value = toDraft(c);
  ensureDisplayDevicesLoaded();
  ensureHdrProfilesLoaded();
}
function resetDraft(): void {
  if (selected.value) draft.value = toDraft(selected.value);
}
watch(selected, (c) => {
  if (!c) draft.value = null;
});

const draftValid = computed(() => {
  const d = draft.value;
  if (!d || !d.overrideEnabled || d.selection !== 'virtual') return true;
  return d.virtualMode === 'global' || d.virtualMode === 'per_client' || d.virtualMode === 'shared';
});

// ---- option lists ---------------------------------------------------------
const virtualModeOptions = computed(() => [
  { label: t('config.app_virtual_display_mode_follow_global'), value: 'global' },
  { label: t('config.virtual_display_mode_per_client'), value: 'per_client' },
  { label: t('config.virtual_display_mode_shared'), value: 'shared' },
]);
const globalVirtualLayout = computed<VirtualLayout>(() =>
  parseVirtualLayout((configStore.config as Record<string, unknown>)?.['virtual_display_layout']),
);
const LAYOUTS: Array<Exclude<VirtualLayout, null>> = [
  'exclusive',
  'extended',
  'extended_primary',
  'extended_isolated',
  'extended_primary_isolated',
];
const layoutOptions = computed(() =>
  LAYOUTS.map((value) => ({
    value,
    label: t(`config.virtual_display_layout_${value}`),
    desc: t(`config.virtual_display_layout_${value}_desc`),
  })),
);
const prefer10Options = computed(() => [
  { label: t('_common.enabled'), value: 'enabled' },
  { label: t('_common.disabled'), value: 'disabled' },
]);
const globalPrefer10 = computed(() =>
  toBool((configStore.config as Record<string, unknown>)?.['prefer_10bit_sdr'], false),
);

const hdrProfiles = ref<HdrProfileEntry[]>([]);
const hdrLoading = ref(false);
const hdrError = ref('');
const hdrOptions = computed(() => {
  const list = [...hdrProfiles.value].sort(
    (a, b) => (Number(b.added_ms || 0) || 0) - (Number(a.added_ms || 0) || 0),
  );
  const options: Array<{ label: string; value: string }> = [
    { label: t('clients.hdr_profile_auto'), value: '' },
  ];
  for (const p of list) {
    const filename = String(p?.filename || '').trim();
    if (filename) options.push({ label: filename, value: filename });
  }
  return options;
});
async function loadHdrProfiles(): Promise<void> {
  if (!isWindows.value) return;
  hdrLoading.value = true;
  hdrError.value = '';
  try {
    const r = await http.get('./api/clients/hdr-profiles', { validateStatus: () => true });
    const body = (r.data ?? {}) as {
      status?: boolean;
      profiles?: HdrProfileEntry[];
      error?: string;
    };
    if (r.status >= 200 && r.status < 300 && body.status === true && Array.isArray(body.profiles)) {
      hdrProfiles.value = body.profiles;
    } else {
      hdrProfiles.value = [];
      hdrError.value = body.error || t('clients.hdr_profile_load_failed');
    }
  } catch (e) {
    hdrProfiles.value = [];
    hdrError.value = e instanceof Error ? e.message : t('clients.hdr_profile_load_failed');
  } finally {
    hdrLoading.value = false;
  }
}
function ensureHdrProfilesLoaded(): void {
  if (isWindows.value && !hdrLoading.value && hdrProfiles.value.length === 0)
    void loadHdrProfiles();
}

const displayDevices = ref<DisplayDevice[]>([]);
const devicesLoading = ref(false);
const devicesError = ref('');
async function loadDisplayDevices(): Promise<void> {
  if (!isWindows.value) return;
  devicesLoading.value = true;
  devicesError.value = '';
  try {
    const r = await http.get<DisplayDevice[]>('/api/display-devices', {
      params: { detail: 'full' },
    });
    displayDevices.value = Array.isArray(r.data) ? r.data : [];
  } catch (e) {
    devicesError.value = e instanceof Error ? e.message : 'Failed to load display devices';
    displayDevices.value = [];
  } finally {
    devicesLoading.value = false;
  }
}
function ensureDisplayDevicesLoaded(): void {
  if (isWindows.value && !devicesLoading.value && displayDevices.value.length === 0)
    void loadDisplayDevices();
}
const deviceOptions = computed(() => {
  const opts: Array<{ label: string; value: string }> = [];
  const seen = new Set<string>();
  for (const d of displayDevices.value) {
    const value = d.device_id || d.display_name || '';
    if (!value || seen.has(value)) continue;
    const name = d.friendly_name || d.display_name || 'Display';
    const info = d.info as { active?: unknown } | null | undefined;
    let active: boolean | null = null;
    if (info && typeof info === 'object' && 'active' in info) active = !!info.active;
    else if (info) active = true;
    const suffix =
      active === null
        ? ''
        : active
          ? ` (${t('config.app_display_status_active')})`
          : ` (${t('config.app_display_status_inactive')})`;
    opts.push({ label: `${name} - ${value}${suffix}`, value });
    seen.add(value);
  }
  return opts;
});

// ---- actions --------------------------------------------------------------
const saving = ref(false);
async function saveDraft(): Promise<void> {
  const d = draft.value;
  if (!d || saving.value) return;
  if (!draftValid.value) {
    message.error(t('clients.update_failed'));
    return;
  }
  saving.value = true;
  try {
    const payload: Record<string, unknown> = {
      uuid: d.uuid,
      name: d.name.trim(),
      hdr_profile: d.hdrProfile.trim(),
      display_mode: d.displayMode.trim(),
    };
    if (!d.overrideEnabled) {
      payload['output_name_override'] = '';
      payload['always_use_virtual_display'] = false;
      payload['virtual_display_mode'] = '';
      payload['virtual_display_layout'] = '';
    } else if (d.selection === 'physical') {
      payload['output_name_override'] = String(d.physicalOutput || '').trim();
      payload['always_use_virtual_display'] = false;
      payload['virtual_display_mode'] = 'disabled';
      payload['virtual_display_layout'] = '';
    } else {
      payload['output_name_override'] = '';
      if (d.virtualMode === 'global' || d.virtualMode === null) {
        payload['always_use_virtual_display'] = false;
        payload['virtual_display_mode'] = 'global';
      } else {
        payload['always_use_virtual_display'] = true;
        payload['virtual_display_mode'] = d.virtualMode;
      }
      payload['virtual_display_layout'] = d.virtualLayout ?? '';
    }
    payload['config_overrides'] = Object.fromEntries(
      Object.entries(d.overrides || {}).filter(
        ([k, v]) => typeof k === 'string' && k.length > 0 && v !== undefined && v !== null,
      ),
    );
    if (d.prefer10BitSdr !== null) payload['prefer_10bit_sdr'] = d.prefer10BitSdr === 'enabled';

    const r = await http.post('./api/clients/update', payload, { validateStatus: () => true });
    const body = r.data as { status?: boolean } | undefined;
    if (r.status >= 200 && r.status < 300 && body?.status === true) {
      message.success(t('clients.update_success'));
      const replaced = await refreshClients();
      await host.refreshClients();
      // Only rebuild the draft from a fresh list; on a transient refresh
      // failure the edited values stay in the inspector.
      if (replaced) resetDraft();
    } else {
      message.error(t('clients.update_failed'));
    }
  } catch (e) {
    message.error(e instanceof Error ? e.message : t('clients.update_failed'));
  } finally {
    saving.value = false;
  }
}

const busy = ref<Record<string, boolean>>({});
async function disconnectClient(c: Client): Promise<void> {
  if (busy.value[c.uuid]) return;
  busy.value = { ...busy.value, [c.uuid]: true };
  try {
    const r = await http.post(
      './api/clients/disconnect',
      { uuid: c.uuid },
      { validateStatus: () => true },
    );
    const body = r.data as { status?: boolean } | undefined;
    if (r.status >= 200 && r.status < 300 && body?.status === true)
      message.success(t('clients.disconnect_success'));
    else message.error(t('clients.disconnect_failed'));
  } catch (e) {
    message.error(e instanceof Error ? e.message : t('clients.disconnect_failed'));
  } finally {
    busy.value = { ...busy.value, [c.uuid]: false };
    await refreshClients();
    await host.refreshClients();
  }
}
function confirmUnpair(c: Client): void {
  const name = c.name;
  dialog.warning({
    title: name
      ? t('clients.confirm_remove_title_named', { name })
      : t('clients.confirm_remove_title'),
    content: name
      ? t('clients.confirm_remove_message_named', { name })
      : t('clients.confirm_remove_message'),
    positiveText: t('clients.remove'),
    negativeText: t('_common.cancel'),
    onPositiveClick: async () => {
      try {
        await http.post('./api/clients/unpair', { uuid: c.uuid }, { validateStatus: () => true });
      } catch {
        /* the refresh below shows the real state */
      }
      if (selectedUuid.value === c.uuid) selectedUuid.value = null;
      await refreshClients();
      await host.refreshClients();
    },
  });
}
function confirmUnpairAll(): void {
  const count = clients.value.length;
  dialog.warning({
    title: t('clients.confirm_unpair_all_title'),
    content:
      count > 0
        ? t('clients.confirm_unpair_all_message_count', { count })
        : t('clients.confirm_unpair_all_message'),
    positiveText: t('troubleshooting.unpair_all'),
    negativeText: t('_common.cancel'),
    onPositiveClick: async () => {
      try {
        const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true });
        const body = r.data as { status?: boolean } | undefined;
        if (body?.status === true) message.success(t('troubleshooting.unpair_all_success'));
        else message.error(t('troubleshooting.unpair_all_error'));
      } catch {
        message.error(t('troubleshooting.unpair_all_error'));
      }
      selectedUuid.value = null;
      await refreshClients();
      await host.refreshClients();
    },
  });
}

// ---- pairing --------------------------------------------------------------
const pairOpen = ref(false);
const pin = ref('');
const deviceName = ref('');
const pairing = ref(false);
const pairResult = ref<'ok' | 'fail' | null>(null);
function openPair(): void {
  pairResult.value = null;
  pairOpen.value = true;
}
async function pair(): Promise<void> {
  if (pairing.value || pin.value.trim().length !== 4) return;
  pairing.value = true;
  pairResult.value = null;
  try {
    const name = deviceName.value.trim();
    const r = await http.post(
      './api/pin',
      { pin: pin.value.trim(), name },
      { validateStatus: () => true },
    );
    const body = r.data as { status?: unknown } | undefined;
    const ok =
      r.status >= 200 &&
      r.status < 300 &&
      (body?.status === true || body?.status === 'true' || body?.status === 1);
    pairResult.value = ok ? 'ok' : 'fail';
    if (ok) {
      const before = clients.value.length;
      const deadline = Date.now() + 5000;
      const target = name.toLowerCase();
      do {
        await refreshClients();
        if (
          clients.value.some((c) => c.name.toLowerCase() === target) ||
          clients.value.length > before
        )
          break;
        await new Promise((res) => setTimeout(res, 400));
      } while (Date.now() < deadline);
      await host.refreshClients();
      pin.value = '';
      deviceName.value = '';
    }
  } catch {
    pairResult.value = 'fail';
  } finally {
    pairing.value = false;
  }
}

// ---- lifecycle -------------------------------------------------------------
onMounted(async () => {
  await configStore.fetchConfig().catch(() => null);
  await auth.waitForAuthentication();
  if (!host.running) void host.start();
  await refreshClients();
  const wanted = route.query['id'];
  if (typeof wanted === 'string') {
    const c = clients.value.find((x) => x.uuid === wanted);
    if (c) select(c);
  }
  refreshTimer = setInterval(() => void refreshClients(), 5000);
});
watch(
  () => route.query['id'],
  (wanted) => {
    if (typeof wanted !== 'string') return;
    const c = clients.value.find((x) => x.uuid === wanted);
    if (c) select(c);
  },
);
onBeforeUnmount(() => {
  if (refreshTimer) clearInterval(refreshTimer);
});
</script>

<template>
  <div class="flex flex-col gap-3.5">
    <div class="flex flex-wrap items-center gap-3">
      <div>
        <div class="mc-page-title">{{ t('clients.nav') }}</div>
        <div class="mc-page-sub">
          {{ clients.length }} {{ t2('clients.paired', 'paired') }} · {{ connectedCount }}
          {{ t2('clients.connected_count', 'connected') }}
        </div>
      </div>
      <div class="flex-1"></div>
      <div class="mc-chips" role="tablist">
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'clients' }"
          role="tab"
          :aria-selected="section === 'clients'"
          @click="go('clients')"
        >
          {{ t('clients.nav') }}
        </button>
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'devices' }"
          role="tab"
          :aria-selected="section === 'devices'"
          @click="go('devices')"
        >
          {{ t2('auth.sessions_heading', 'Trusted devices') }}
        </button>
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'tokens' }"
          role="tab"
          :aria-selected="section === 'tokens'"
          @click="go('tokens')"
        >
          {{ t('navbar.api_tokens') }}
        </button>
      </div>
      <template v-if="section === 'clients'">
        <div class="mc-chips">
          <button
            type="button"
            class="mc-chip"
            :class="{ 'mc-chip-on': sortMode === 'recent' }"
            @click="sortMode = 'recent'"
          >
            {{ t('clients.sort_recent') }}
          </button>
          <button
            type="button"
            class="mc-chip"
            :class="{ 'mc-chip-on': sortMode === 'name' }"
            @click="sortMode = 'name'"
          >
            {{ t('clients.sort_name') }}
          </button>
        </div>
        <NButton
          size="small"
          type="error"
          :disabled="clients.length === 0"
          @click="confirmUnpairAll"
          >{{ t('troubleshooting.unpair_all') }}</NButton
        >
        <NButton size="small" type="primary" @click="openPair"
          ><NavIcon name="plus" :size="14" />{{
            t2('clients.pair_button', 'Pair with PIN')
          }}</NButton
        >
      </template>
    </div>

    <template v-if="section === 'clients'">
      <section class="mc-panel">
        <div
          class="grid grid-cols-[1.3fr_0.9fr_1.3fr_1.5fr_1.1fr_150px] gap-3 border-b border-line px-4 py-2 text-[10.5px] font-medium uppercase tracking-[0.06em] text-ink-4"
        >
          <span>{{ t2('clients.col_device', 'Device') }}</span
          ><span>{{ t2('clients.col_status', 'Status') }}</span
          ><span>{{ t2('clients.col_last_seen', 'Last seen') }}</span
          ><span>{{ t2('clients.col_display', 'Display') }}</span
          ><span>HDR</span><span></span>
        </div>
        <div v-if="loaded && clients.length === 0" class="px-4 py-4 text-center text-xs text-ink-4">
          {{ t('troubleshooting.unpair_single_no_devices') }}
        </div>
        <div
          v-for="c in sorted"
          :key="c.uuid"
          class="mc-row-clickable grid cursor-pointer grid-cols-[1.3fr_0.9fr_1.3fr_1.5fr_1.1fr_150px] items-center gap-3 border-b border-line px-4 py-2.5 text-[12.5px] last:border-b-0"
          :class="{ 'mc-row-selected': c.uuid === selectedUuid }"
          role="button"
          tabindex="0"
          @click="select(c)"
          @keydown.enter.prevent="select(c)"
        >
          <span class="truncate font-medium">{{ nameOf(c) }}</span>
          <span
            ><span class="mc-tag" :class="c.connected ? 'mc-tag-ok' : ''">{{
              c.connected ? t('clients.connected') : t2('overview.idle', 'Idle')
            }}</span></span
          >
          <span class="truncate text-[11.5px] text-ink-3">{{ lastSeenText(c) }}</span>
          <span class="truncate text-[11.5px] text-ink-3"
            >{{ displayText(c) }}{{ c.displayMode ? ` · ${c.displayMode}` : '' }}</span
          >
          <span class="truncate text-[11.5px] text-ink-3">{{ hdrText(c) }}</span>
          <span class="flex justify-end gap-1.5" @click.stop>
            <NButton
              v-if="c.connected"
              size="tiny"
              type="error"
              :loading="busy[c.uuid] === true"
              @click="disconnectClient(c)"
              >{{ t2('stream.disconnect', 'Disconnect') }}</NButton
            >
            <NButton size="tiny" @click="confirmUnpair(c)">{{ t('clients.remove') }}</NButton>
          </span>
        </div>
      </section>
    </template>
    <TrustedDevicesCard v-else-if="section === 'devices'" />
    <ApiTokenManager v-else />

    <!-- Pair with PIN -->
    <NModal v-model:show="pairOpen">
      <div class="mc-panel w-[420px] max-w-[92vw]">
        <div class="mc-panel-h">
          <span class="mc-panel-title">{{ t('clients.pair_title') }}</span>
        </div>
        <div class="space-y-3 px-4 py-4">
          <p class="text-xs text-ink-3">{{ t('clients.pair_desc') }}</p>
          <div>
            <div class="mc-field-label mb-1">{{ t2('pin.pin', 'PIN') }}</div>
            <NInput
              v-model:value="pin"
              size="small"
              inputmode="numeric"
              maxlength="4"
              placeholder="0000"
              @keydown.enter="pair"
            />
          </div>
          <div>
            <div class="mc-field-label mb-1">{{ t('pin.device_name') }}</div>
            <NInput
              v-model:value="deviceName"
              size="small"
              :placeholder="t('pin.device_name')"
              @keydown.enter="pair"
            />
          </div>
          <div v-if="pairResult === 'ok'" class="mc-tag mc-tag-ok">
            {{ t2('pin.success', 'Paired.') }}
          </div>
          <div v-else-if="pairResult === 'fail'" class="mc-tag mc-tag-danger">
            {{ t2('pin.failure', 'Pairing failed. Check the PIN and try again.') }}
          </div>
        </div>
        <div class="flex justify-end gap-2 border-t border-line px-4 py-3">
          <NButton size="small" @click="pairOpen = false">{{ t('_common.cancel') }}</NButton>
          <NButton
            size="small"
            type="primary"
            :loading="pairing"
            :disabled="pin.trim().length !== 4"
            @click="pair"
            >{{ t2('pin.send', 'Pair') }}</NButton
          >
        </div>
      </div>
    </NModal>

    <!-- Inspector: selected client -->
    <InspectorPanel
      v-if="section === 'clients' && selected && draft.value"
      :title="nameOf(selected)"
      :subtitle="`${lastSeenText(selected)}${selected.connected ? ` · ${t2('clients.streaming_now', 'streaming now')}` : ''}`"
      :tag="selected.connected ? t('clients.connected') : t2('overview.idle', 'Idle')"
      :tag-kind="selected.connected ? 'ok' : ''"
    >
      <div class="mc-field">
        <span class="mc-field-label">{{ t('pin.device_name') }}</span>
        <NInput v-model:value="draft.value.name" size="small" />
      </div>
      <div class="mc-field">
        <span class="mc-field-label">{{ t('pin.display_mode_override') }}</span>
        <NInput v-model:value="draft.value.displayMode" size="small" placeholder="1920x1080x60" />
        <span class="mc-field-hint">{{ t('pin.display_mode_override_desc') }}</span>
      </div>

      <template v-if="isWindows">
        <div class="mc-field">
          <div class="flex items-center justify-between gap-3">
            <span class="mc-field-label">{{ t('config.client_display_override_label') }}</span>
            <NSwitch
              :value="draft.value.overrideEnabled"
              size="small"
              @update:value="(v: boolean) => applyOverrideEnabled(draft.value!, v)"
            />
          </div>
          <span class="mc-field-hint">{{ t('config.client_display_override_hint') }}</span>
        </div>
        <template v-if="draft.value.overrideEnabled">
          <div class="mc-field">
            <div class="flex gap-2">
              <button
                type="button"
                class="flex-1 rounded-lg border px-2.5 py-2 text-left"
                :class="
                  draft.value.selection === 'virtual'
                    ? 'border-primary/60 bg-primary/5'
                    : 'border-line-strong'
                "
                @click="applySelection(draft.value!, 'virtual')"
              >
                <div class="text-xs font-medium">
                  {{ t('config.app_display_override_virtual') }}
                </div>
              </button>
              <button
                type="button"
                class="flex-1 rounded-lg border px-2.5 py-2 text-left"
                :class="
                  draft.value.selection === 'physical'
                    ? 'border-primary/60 bg-primary/5'
                    : 'border-line-strong'
                "
                @click="applySelection(draft.value!, 'physical')"
              >
                <div class="text-xs font-medium">
                  {{ t('config.app_display_override_physical') }}
                </div>
              </button>
            </div>
          </div>
          <div v-if="draft.value.selection === 'physical'" class="mc-field">
            <div class="flex items-center justify-between gap-3">
              <span class="mc-field-label">{{ t('config.app_display_physical_label') }}</span>
              <NButton size="tiny" tertiary :loading="devicesLoading" @click="loadDisplayDevices">{{
                t('_common.refresh')
              }}</NButton>
            </div>
            <NSelect
              v-model:value="draft.value.physicalOutput"
              size="small"
              :options="deviceOptions"
              :loading="devicesLoading"
              :placeholder="t('config.app_display_physical_placeholder')"
              filterable
              clearable
              tag
              @focus="ensureDisplayDevicesLoaded"
            />
            <span class="mc-field-hint" :class="{ 'text-danger': devicesError }">{{
              devicesError || t('config.app_display_physical_status_hint')
            }}</span>
          </div>
          <template v-else>
            <div class="mc-field">
              <span class="mc-field-label">{{ t('config.virtual_display_mode_label') }}</span>
              <NSelect
                v-model:value="draft.value.virtualMode"
                size="small"
                :options="virtualModeOptions"
              />
              <span class="mc-field-hint">{{ t('config.virtual_display_mode_step_hint') }}</span>
            </div>
            <div class="mc-field">
              <div class="flex items-center justify-between gap-3">
                <span class="mc-field-label">{{ t('config.virtual_display_layout_label') }}</span>
                <NButton
                  v-if="draft.value.virtualLayout !== null"
                  size="tiny"
                  tertiary
                  @click="draft.value.virtualLayout = null"
                  >{{ t('config.app_virtual_display_layout_reset') }}</NButton
                >
              </div>
              <NRadioGroup
                :value="draft.value.virtualLayout ?? globalVirtualLayout ?? 'exclusive'"
                class="flex flex-col gap-1.5"
                @update:value="
                  (v: string) =>
                    (draft.value!.virtualLayout =
                      v === globalVirtualLayout ? null : (v as VirtualLayout))
                "
              >
                <NRadio v-for="o in layoutOptions" :key="o.value" :value="o.value" size="small">
                  <span class="text-xs font-medium">{{ o.label }}</span>
                  <span class="block text-[10.5px] text-ink-4">{{ o.desc }}</span>
                </NRadio>
              </NRadioGroup>
              <span v-if="draft.value.virtualLayout === null" class="mc-field-hint">{{
                t('config.app_virtual_display_layout_follow_global')
              }}</span>
            </div>
          </template>
        </template>
        <div class="mc-field">
          <span class="mc-field-label">{{ t('clients.hdr_profile_label') }}</span>
          <NSelect
            v-model:value="draft.value.hdrProfile"
            size="small"
            :options="hdrOptions"
            :loading="hdrLoading"
            :placeholder="t('clients.hdr_profile_placeholder')"
            filterable
            @focus="ensureHdrProfilesLoaded"
          />
          <span class="mc-field-hint" :class="{ 'text-danger': hdrError }">{{
            hdrError || t('clients.hdr_profile_desc')
          }}</span>
        </div>
      </template>

      <div class="mc-field">
        <span class="mc-field-label">{{ t('config.prefer_10bit_sdr') }}</span>
        <NSelect
          v-model:value="draft.value.prefer10BitSdr"
          size="small"
          :options="prefer10Options"
          clearable
          :placeholder="t('config.prefer_10bit_sdr_follow_global')"
        />
        <span class="mc-field-hint">
          {{ t('config.prefer_10bit_sdr_desc') }}
          <span v-if="draft.value.prefer10BitSdr === null">
            · {{ t('config.prefer_10bit_sdr_follow_global') }} ({{
              globalPrefer10 ? t('_common.enabled') : t('_common.disabled')
            }})</span
          >
        </span>
      </div>

      <div class="px-[18px] py-3">
        <AppEditConfigOverridesSection
          v-model:overrides="draft.value.overrides"
          scope-label="client"
        />
      </div>

      <template #footer>
        <NButton size="small" :disabled="saving" @click="resetDraft">{{
          t('_common.cancel')
        }}</NButton>
        <NButton
          size="small"
          type="primary"
          :loading="saving"
          :disabled="!draftValid"
          @click="saveDraft"
          >{{ t('_common.save') }}</NButton
        >
      </template>
    </InspectorPanel>
  </div>
</template>
