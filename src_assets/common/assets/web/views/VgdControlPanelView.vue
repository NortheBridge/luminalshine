<template>
  <!-- Prototype preview: every control is intentionally disabled and the
       accent is forced grey (instead of sun-gold) via the local theme
       override below until the LuminalVGD driver ships. -->
  <n-config-provider :theme-overrides="greyOverrides">
    <div class="space-y-6">
      <div>
        <h1 class="text-2xl font-semibold tracking-tight text-dark dark:text-light">
          {{ t('vgd.control_panel_title') }}
        </h1>
        <p class="text-xs opacity-70 leading-snug mt-1">
          {{ t('vgd.control_panel_subtitle') }}
        </p>
      </div>

      <n-alert type="warning" :show-icon="true">
        <span class="font-semibold">{{ t('vgd.proto_notice') }}</span>
        <i18n-t keypath="vgd.proto_visit" tag="span" class="block mt-0.5">
          <template #link>
            <a
              :href="VGD_SITE_URL"
              target="_blank"
              rel="noopener noreferrer"
              class="underline underline-offset-2 hover:opacity-80 break-all"
              >{{ VGD_SITE_URL }}</a
            >
          </template>
        </i18n-t>
      </n-alert>

      <section
        v-for="group in groups"
        :key="group.title"
        class="rounded-xl border border-white/10 bg-white/[0.03] p-4 sm:p-5 space-y-4 opacity-90"
      >
        <div class="flex items-center gap-2">
          <i :class="group.icon + ' text-neutral-400'" />
          <h2 class="text-base font-semibold text-dark dark:text-light">{{ group.title }}</h2>
        </div>
        <div class="grid grid-cols-1 lg:grid-cols-2 gap-x-8 gap-y-4">
          <div
            v-for="item in group.items"
            :key="item.label"
            class="flex items-start justify-between gap-4"
          >
            <div class="min-w-0">
              <div class="text-sm font-medium text-neutral-400">{{ item.label }}</div>
              <p class="text-xs opacity-60 leading-snug mt-0.5">{{ item.hint }}</p>
            </div>
            <div class="shrink-0 pt-0.5">
              <n-switch
                v-if="item.type === 'switch'"
                :value="Boolean(item.value)"
                :disabled="true"
              />
              <n-select
                v-else-if="item.type === 'select'"
                :value="String(item.value)"
                :options="item.options"
                :disabled="true"
                size="small"
                class="w-56"
              />
              <n-input-number
                v-else-if="item.type === 'number'"
                :value="Number(item.value)"
                :disabled="true"
                size="small"
                class="w-36"
              >
                <template v-if="item.suffix" #suffix>{{ item.suffix }}</template>
              </n-input-number>
              <n-input
                v-else
                :value="String(item.value)"
                :placeholder="item.placeholder"
                :disabled="true"
                size="small"
                class="w-56"
              />
            </div>
          </div>
        </div>
        <p v-if="group.footnote" class="text-[11px] opacity-50 leading-snug">
          {{ group.footnote }}
        </p>
      </section>
    </div>
  </n-config-provider>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { NConfigProvider, NAlert, NSwitch, NSelect, NInputNumber, NInput } from 'naive-ui';
import type { GlobalThemeOverrides } from 'naive-ui';

const { t } = useI18n();

const VGD_SITE_URL = 'https://apps.northebridge.com/en/LuminalVGD';

// Neutral greys stand in for the sun-gold primary while the driver is in
// prototype; delete this override when the panel goes live.
const greyOverrides: GlobalThemeOverrides = {
  common: {
    primaryColor: '#9CA3AF',
    primaryColorHover: '#9CA3AF',
    primaryColorPressed: '#6B7280',
    primaryColorSuppl: '#9CA3AF',
  },
};

const tr = (key: string, fallback: string) => {
  const value = t(key);
  return value === key ? fallback : value;
};

interface PanelItem {
  label: string;
  hint: string;
  type: 'switch' | 'select' | 'number' | 'text';
  value: string | number | boolean;
  options?: { label: string; value: string }[];
  suffix?: string;
  placeholder?: string;
}

interface PanelGroup {
  title: string;
  icon: string;
  items: PanelItem[];
  footnote?: string;
}

// Values shown are the design-document defaults (LuminalVGD proto v0.3 +
// the LuminalShine host-side VgdConfig); nothing here is wired to config.
const groups = computed<PanelGroup[]>(() => [
  {
    title: tr('vgd.group_backend', 'Backend & sessions'),
    icon: 'fas fa-microchip',
    items: [
      {
        label: tr('vgd.backend', 'Virtual display backend'),
        hint: tr(
          'vgd.backend_hint',
          'LuminalVGD becomes the default backend once the driver ships; SudoVDA remains available.',
        ),
        type: 'select',
        value: 'luminalvgd',
        options: [
          { label: tr('vgd.backend_auto', 'Automatic'), value: 'auto' },
          { label: 'SudoVDA', value: 'sudovda' },
          { label: 'LuminalVGD', value: 'luminalvgd' },
        ],
      },
      {
        label: tr('vgd.max_monitors', 'Maximum virtual monitors'),
        hint: tr('vgd.max_monitors_hint', 'Global cap on concurrent virtual monitors (up to 16).'),
        type: 'number',
        value: 10,
      },
      {
        label: tr('vgd.watchdog', 'Lease watchdog'),
        hint: tr(
          'vgd.watchdog_hint',
          'Reaps a virtual monitor when its stream stops pinging; 0 disables the watchdog.',
        ),
        type: 'number',
        value: 3,
        suffix: 's',
      },
      {
        label: tr('vgd.lease_timeout', 'Per-stream lease timeout'),
        hint: tr(
          'vgd.lease_timeout_hint',
          'Per-monitor override of the watchdog (3–300 seconds, or driver default).',
        ),
        type: 'text',
        value: '',
        placeholder: tr('vgd.driver_default', 'Driver default'),
      },
    ],
  },
  {
    title: tr('vgd.group_identity', 'Display identity & lifetime'),
    icon: 'fas fa-fingerprint',
    items: [
      {
        label: tr('vgd.stable_identity', 'Stable display identity'),
        hint: tr(
          'vgd.stable_identity_hint',
          'Reuse the same monitor identity across reconnects so Windows restores resolution, HDR, and scaling.',
        ),
        type: 'switch',
        value: true,
      },
      {
        label: tr('vgd.keep_paused', 'Keep virtual display while paused'),
        hint: tr(
          'vgd.keep_paused_hint',
          'How long a paused stream keeps its virtual display alive; 0 keeps it until resume.',
        ),
        type: 'number',
        value: 0,
        suffix: 's',
      },
      {
        label: tr('vgd.permanent_pool', 'Permanent display pool'),
        hint: tr(
          'vgd.permanent_pool_hint',
          'Always-on virtual displays that exist outside any stream and survive driver restarts (up to 4).',
        ),
        type: 'number',
        value: 0,
      },
      {
        label: tr('vgd.permanent_mode', 'Permanent display mode'),
        hint: tr(
          'vgd.permanent_mode_hint',
          'Resolution and refresh rate for pool members; validated against the mode envelope.',
        ),
        type: 'text',
        value: '',
        placeholder: '1920×1080 @ 60 Hz',
      },
    ],
  },
  {
    title: tr('vgd.group_modes', 'Modes & refresh'),
    icon: 'fas fa-display',
    items: [
      {
        label: tr('vgd.advertised_modes', 'Advertised modes'),
        hint: tr(
          'vgd.advertised_modes_hint',
          'Up to four exact modes per monitor, native first — switching between them needs no monitor recreate.',
        ),
        type: 'text',
        value: '',
        placeholder: tr('vgd.advertised_modes_placeholder', 'Native mode (automatic)'),
      },
      {
        label: tr('vgd.refresh_doubling', 'Refresh doubling for frame generation'),
        hint: tr(
          'vgd.refresh_doubling_hint',
          'Advertise 2× the client refresh rate while frame generation is active.',
        ),
        type: 'switch',
        value: true,
      },
    ],
    footnote: tr(
      'vgd.modes_footnote',
      'Mode envelope: 320×200 to 7680×4320, even dimensions, millihertz-precise refresh (59.94 Hz supported), no refresh ceiling.',
    ),
  },
  {
    title: tr('vgd.group_color', 'Color & HDR'),
    icon: 'fas fa-palette',
    items: [
      {
        label: tr('vgd.bit_depth', 'Bit depth'),
        hint: tr(
          'vgd.bit_depth_hint',
          'Advertised dynamic range and depth; gated by negotiated driver capabilities.',
        ),
        type: 'select',
        value: '8',
        options: [
          { label: tr('vgd.depth_sdr8', '8-bit SDR (default)'), value: '8' },
          { label: tr('vgd.depth_sdr10', '10-bit SDR'), value: '10' },
          { label: tr('vgd.depth_hdr10', '10-bit HDR (HDR10)'), value: '110' },
          { label: tr('vgd.depth_hdr12', '12-bit HDR'), value: '112' },
        ],
      },
      {
        label: tr('vgd.hdr', 'HDR'),
        hint: tr('vgd.hdr_hint', 'Requires Windows 11 24H2 and an HDR-capable bit depth.'),
        type: 'switch',
        value: false,
      },
    ],
  },
  {
    title: tr('vgd.group_gpu', 'GPU & performance'),
    icon: 'fas fa-gauge-high',
    items: [
      {
        label: tr('vgd.adapter', 'Preferred render adapter'),
        hint: tr(
          'vgd.adapter_hint',
          'Match a GPU by name for hybrid-GPU systems; empty selects the adapter with the most VRAM.',
        ),
        type: 'text',
        value: '',
        placeholder: tr('vgd.adapter_placeholder', 'Largest VRAM (automatic)'),
      },
      {
        label: tr('vgd.ring_slots', 'Frame ring slots'),
        hint: tr(
          'vgd.ring_slots_hint',
          'Depth of the shared keyed-mutex texture ring between driver and host (1–8).',
        ),
        type: 'number',
        value: 3,
      },
      {
        label: tr('vgd.mutex_timeout', 'Keyed-mutex acquire timeout'),
        hint: tr(
          'vgd.mutex_timeout_hint',
          'Bounded wait when acquiring a shared frame — the driver never waits unbounded.',
        ),
        type: 'number',
        value: 100,
        suffix: 'ms',
      },
      {
        label: tr('vgd.dda_fallback', 'Desktop Duplication fallback'),
        hint: tr(
          'vgd.dda_fallback_hint',
          'Allow DXGI Desktop Duplication as the last-resort capture path.',
        ),
        type: 'switch',
        value: true,
      },
    ],
  },
  {
    title: tr('vgd.group_edid', 'EDID & physical'),
    icon: 'fas fa-ruler-combined',
    items: [
      {
        label: tr('vgd.friendly_name', 'Monitor name'),
        hint: tr('vgd.friendly_name_hint', 'Name shown by Windows in the EDID descriptor.'),
        type: 'text',
        value: 'Luminal VGD',
      },
      {
        label: tr('vgd.physical_size', 'Physical size'),
        hint: tr(
          'vgd.physical_size_hint',
          'Reported panel dimensions drive Windows DPI scaling; default is 600×340 mm (≈27″).',
        ),
        type: 'text',
        value: '',
        placeholder: '600 × 340 mm',
      },
      {
        label: tr('vgd.edid_serial', 'EDID serial override'),
        hint: tr(
          'vgd.edid_serial_hint',
          'Explicit EDID serial; 0 derives it from the display identity.',
        ),
        type: 'number',
        value: 0,
      },
    ],
  },
  {
    title: tr('vgd.group_recovery', 'Recovery & restore'),
    icon: 'fas fa-rotate-left',
    items: [
      {
        label: tr('vgd.restore_backoff', 'Restore probe backoff'),
        hint: tr(
          'vgd.restore_backoff_hint',
          'First delay before probing a return to direct capture after a fallback.',
        ),
        type: 'number',
        value: 1000,
        suffix: 'ms',
      },
      {
        label: tr('vgd.restore_backoff_max', 'Restore backoff ceiling'),
        hint: tr('vgd.restore_backoff_max_hint', 'Upper bound of the doubling probe backoff.'),
        type: 'number',
        value: 30000,
        suffix: 'ms',
      },
      {
        label: tr('vgd.restore_frames', 'Stable frames before teardown'),
        hint: tr(
          'vgd.restore_frames_hint',
          'Direct-capture frames required before the warm fallback path is torn down.',
        ),
        type: 'number',
        value: 120,
      },
      {
        label: tr('vgd.heartbeat', 'Driver heartbeat / stale threshold'),
        hint: tr(
          'vgd.heartbeat_hint',
          'Liveness cadence and the age at which the host treats the driver as wedged.',
        ),
        type: 'text',
        value: '500 / 2000 ms',
      },
    ],
  },
]);
</script>
