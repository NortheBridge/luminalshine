<template>
  <div class="space-y-6">
    <div>
      <h1 class="text-2xl font-semibold tracking-tight text-dark dark:text-light">
        {{ t('vgd.control_panel_title') }}
      </h1>
      <p class="text-xs opacity-70 leading-snug mt-1">
        {{ t('vgd.control_panel_subtitle') }}
      </p>
    </div>

    <n-alert v-if="!driverReady" type="warning" :show-icon="true">
      {{
        tr(
          'vgd.panel_driver_missing',
          'The LuminalVGD driver is not detected — these settings take effect once it is installed.',
        )
      }}
    </n-alert>

    <n-alert v-if="showTransition" type="info" :show-icon="true">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <span class="min-w-0">
          {{
            tr(
              'vgd.transition_prompt',
              'The host is not using the LuminalVGD backend (WGC fallback capture). It transitions automatically once the driver is available — or force an attempt now.',
            )
          }}
        </span>
        <n-button size="small" type="primary" :loading="transitionPending" @click="forceTransition">
          {{ tr('vgd.transition_now', 'Transition to VGD now') }}
        </n-button>
      </div>
      <p v-if="transitionResult" class="text-xs opacity-70 mt-1">{{ transitionResult }}</p>
    </n-alert>

    <section
      v-for="group in groups"
      :key="group.title"
      class="rounded-xl border border-white/10 bg-white/[0.03] p-4 sm:p-5 space-y-4"
    >
      <div class="flex items-center gap-2">
        <i :class="group.icon + ' text-primary'" />
        <h2 class="text-base font-semibold text-dark dark:text-light">{{ group.title }}</h2>
      </div>
      <div class="grid grid-cols-1 lg:grid-cols-2 gap-x-8 gap-y-4">
        <div
          v-for="item in group.items"
          :key="item.key"
          class="flex items-start justify-between gap-4"
        >
          <div class="min-w-0">
            <div class="text-sm font-medium text-dark dark:text-light">{{ item.label }}</div>
            <p class="text-xs opacity-60 leading-snug mt-0.5">{{ item.hint }}</p>
          </div>
          <div class="shrink-0 pt-0.5">
            <n-switch
              v-if="item.type === 'switch'"
              :value="boolFromConfig(item)"
              @update:value="(v: boolean) => setBool(item, v)"
            />
            <n-select
              v-else-if="item.type === 'select'"
              :value="String(config[item.key] ?? item.fallback ?? '')"
              :options="item.options"
              size="small"
              class="w-56"
              @update:value="(v: string) => setOption(item.key, v)"
            />
            <n-input-number
              v-else-if="item.type === 'number'"
              :value="toNumber(config[item.key], Number(item.fallback ?? 0))"
              :min="item.min"
              :max="item.max"
              size="small"
              class="w-36"
              @update:value="
                (v: number | null) => setOption(item.key, v ?? Number(item.fallback ?? 0))
              "
            >
              <template v-if="item.suffix" #suffix>{{ item.suffix }}</template>
            </n-input-number>
            <div v-else-if="item.type === 'slider'" class="w-56 sm:w-72">
              <n-slider
                :value="toNumber(config[item.key], Number(item.fallback ?? 0))"
                :min="item.min ?? 0"
                :max="item.max ?? 100"
                :step="1"
                :marks="item.marks"
                @update:value="(v: number) => setOption(item.key, v)"
              />
              <div class="mt-1 flex items-center justify-end gap-2 text-xs opacity-80">
                <span>
                  {{ toNumber(config[item.key], Number(item.fallback ?? 0)) }}
                  {{ item.suffix }}
                </span>
                <n-tag
                  v-if="
                    item.hdrZoneFrom !== undefined &&
                    toNumber(config[item.key], Number(item.fallback ?? 0)) >= item.hdrZoneFrom
                  "
                  size="small"
                  type="warning"
                >
                  HDR
                </n-tag>
              </div>
            </div>
          </div>
        </div>
      </div>
      <p v-if="group.footnote" class="text-[11px] opacity-50 leading-snug">
        {{ group.footnote }}
      </p>
    </section>

    <p class="text-[11px] opacity-50 leading-snug">
      {{
        tr(
          'vgd.panel_autosave_note',
          'Changes save automatically and apply to the next streaming session.',
        )
      }}
    </p>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { storeToRefs } from 'pinia';
import { NAlert, NButton, NSwitch, NSelect, NInputNumber, NSlider, NTag } from 'naive-ui';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';

const { t } = useI18n();

const tr = (key: string, fallback: string) => {
  const value = t(key);
  return value === key ? fallback : value;
};

const store = useConfigStore();
const config = store.config as Record<string, unknown>;
const { metadata } = storeToRefs(store);

onMounted(() => {
  void store.fetchMetadata();
});

const driverReady = computed(() => {
  const md = (metadata.value || {}) as { vgd_installed?: boolean };
  return Boolean(md.vgd_installed);
});

// WGC->VGD transition affordance: shown while the backend is anything but
// LuminalVGD. The button kicks /api/state/vgd-transition (the same attempt
// the periodic watcher runs: devnode ensure -> backend re-selection ->
// driver-status init); the result lands in the LuminalShine log with the
// "LuminalShine has transitioned LuminalVGD into VGD Mode" sentence.
const showTransition = computed(() => {
  const md = (metadata.value || {}) as { virtual_display_backend?: string };
  return Boolean(md.virtual_display_backend) && md.virtual_display_backend !== 'luminalvgd';
});
const transitionPending = ref(false);
const transitionResult = ref('');

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
          ? tr('vgd.transition_already', 'Already on the LuminalVGD backend.')
          : tr(
              'vgd.transition_started',
              'Transition attempt started — this panel refreshes as it completes.',
            );
    } else {
      const reasons: Record<string, string> = {
        busy: tr('vgd.transition_busy', 'A transition or driver rebind is already in progress.'),
        stack_down: tr(
          'vgd.transition_stack_down',
          'The display stack is down (reboot required); transition deferred.',
        ),
        shutting_down: tr('vgd.transition_shutdown', 'The service is shutting down.'),
      };
      transitionResult.value =
        reasons[result] ??
        `${tr('vgd.transition_failed', 'Could not start a transition attempt')} (${result || `HTTP ${r.status}`}).`;
    }
  } catch (e: unknown) {
    transitionResult.value = e instanceof Error ? e.message : 'Request failed';
  } finally {
    transitionPending.value = false;
    // The attempt runs in the background; refresh the metadata snapshot a
    // few times so a successful flip updates this panel unprompted.
    [3000, 10000, 30000].forEach((delay) => {
      setTimeout(() => void store.fetchMetadata(), delay);
    });
  }
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
  if (item.boolEncoding === 'string') {
    return value === 'enabled';
  }
  return Boolean(value);
}

function setBool(item: PanelItem, v: boolean): void {
  setOption(item.key, item.boolEncoding === 'string' ? (v ? 'enabled' : 'disabled') : v);
}

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
  /// Slider tick labels ({value: label}).
  marks?: Record<number, string>;
  /// Values >= this render the "HDR" tag next to the readout.
  hdrZoneFrom?: number;
}

interface PanelGroup {
  title: string;
  icon: string;
  items: PanelItem[];
  footnote?: string;
}

// Only options with real config backing appear here; driver-internal
// tunables (ring depth, mutex timeouts, EDID fields, watchdog cadence)
// are deliberately not exposed — the driver's defaults are the contract.
const groups = computed<PanelGroup[]>(() => [
  {
    title: tr('vgd.group_backend', 'Backend & sessions'),
    icon: 'fas fa-microchip',
    items: [
      {
        key: 'virtual_display_backend',
        label: tr('vgd.backend', 'Virtual display backend'),
        hint: tr(
          'vgd.backend_hint',
          'LuminalVGD is the first-party driver and the automatic choice when installed.',
        ),
        type: 'select',
        fallback: 'auto',
        options: [
          { label: tr('vgd.backend_auto', 'Automatic (recommended)'), value: 'auto' },
          { label: 'LuminalVGD', value: 'luminalvgd' },
        ],
      },
      {
        key: 'virtual_display_mode',
        label: tr('vgd.mode', 'Virtual display mode'),
        hint: tr(
          'vgd.mode_hint',
          'Per client gives every client its own virtual monitor; shared reuses one.',
        ),
        type: 'select',
        fallback: 'per_client',
        options: [
          { label: tr('vgd.mode_disabled', 'Disabled'), value: 'disabled' },
          { label: tr('vgd.mode_per_client', 'Per client'), value: 'per_client' },
          { label: tr('vgd.mode_shared', 'Shared'), value: 'shared' },
        ],
      },
      {
        key: 'virtual_display_layout',
        label: tr('vgd.layout', 'Display layout'),
        hint: tr(
          'vgd.layout_hint',
          'How the virtual display joins the desktop while streaming; exclusive turns physical monitors off.',
        ),
        type: 'select',
        fallback: 'exclusive',
        options: [
          { label: tr('vgd.layout_exclusive', 'Exclusive'), value: 'exclusive' },
          { label: tr('vgd.layout_extended', 'Extended'), value: 'extended' },
          {
            label: tr('vgd.layout_extended_primary', 'Extended (primary)'),
            value: 'extended_primary',
          },
          {
            label: tr('vgd.layout_extended_isolated', 'Extended (isolated)'),
            value: 'extended_isolated',
          },
          {
            label: tr('vgd.layout_extended_primary_isolated', 'Extended (primary, isolated)'),
            value: 'extended_primary_isolated',
          },
        ],
      },
      {
        key: 'dd_activate_virtual_display',
        label: tr('vgd.activate', 'Activate virtual display'),
        hint: tr(
          'vgd.activate_hint',
          'Bring the virtual display into the desktop topology when a session starts.',
        ),
        type: 'switch',
      },
    ],
  },
  {
    title: tr('vgd.group_lifetime', 'Disconnect & pause behavior'),
    icon: 'fas fa-fingerprint',
    items: [
      {
        key: 'dd_config_revert_on_disconnect',
        label: tr('vgd.revert_on_disconnect', 'Revert display config on disconnect'),
        hint: tr(
          'vgd.revert_on_disconnect_hint',
          'Restore physical monitors as soon as the client disconnects instead of keeping the session paused.',
        ),
        type: 'switch',
        boolEncoding: 'string',
      },
      {
        key: 'dd_paused_virtual_display_timeout_secs',
        label: tr('vgd.keep_paused', 'Paused display timeout'),
        hint: tr(
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
    title: tr('vgd.group_modes', 'Modes & refresh'),
    icon: 'fas fa-display',
    items: [
      {
        key: 'dd_wa_virtual_double_refresh',
        label: tr('vgd.refresh_doubling', 'Refresh doubling'),
        hint: tr(
          'vgd.refresh_doubling_hint',
          'Advertise 2× the client refresh rate on the virtual display (frame-generation workaround).',
        ),
        type: 'switch',
      },
    ],
    footnote: tr(
      'vgd.modes_footnote',
      'The virtual display advertises the client-native mode automatically (up to four exact modes per monitor, millihertz-precise refresh, HDR10 when the client and driver support it).',
    ),
  },
  {
    title: tr('vgd.group_hdr', 'HDR'),
    icon: 'fas fa-sun',
    items: [
      {
        key: 'vgd_hdr_peak_nits',
        label: tr('config.vgd_hdr_peak_nits', 'HDR peak brightness (nits)'),
        hint: tr(
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
    footnote: tr(
      'vgd.hdr_footnote',
      'Requires LuminalVGD driver build 15 or newer (older drivers use their built-in 993 nits). The EDID stores brightness on a ~2% logarithmic scale — 800 encodes exactly; 1000 encodes as 993.',
    ),
  },
]);
</script>
