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
              :value="Boolean(config[item.key])"
              @update:value="(v: boolean) => setOption(item.key, v)"
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
import { computed, onMounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { storeToRefs } from 'pinia';
import { NAlert, NSwitch, NSelect, NInputNumber } from 'naive-ui';
import { useConfigStore } from '@/stores/config';

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

function toNumber(value: unknown, fallback: number): number {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function setOption(key: string, value: unknown): void {
  config[key] = value;
}

interface PanelItem {
  key: string;
  label: string;
  hint: string;
  type: 'switch' | 'select' | 'number';
  options?: { label: string; value: string }[];
  suffix?: string;
  fallback?: string | number;
  min?: number;
  max?: number;
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
        fallback: 'disabled',
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
]);
</script>
