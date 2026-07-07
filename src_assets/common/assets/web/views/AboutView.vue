<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { NCard, NButton, NTag, NAlert, useMessage } from 'naive-ui';
import { storeToRefs } from 'pinia';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';

// About page — read-only diagnostics snapshot intended for support
// screenshots. All data is sourced from the existing /api/metadata
// endpoint (extended to cover GPU drivers, HDR state, encoder probe
// results, Insider channel, etc.) plus a one-shot /api/config fetch
// for the network port.
//
// Design idiom matches Dashboard / Troubleshooting: bordered cards
// with rounded-xl, dark/light theme aware, label/value rows in the
// body. Each card represents one logical category (LuminalShine /
// OS / Graphics / Encoders / Display / Virtual Display / Network).

const { t } = useI18n();
const message = useMessage();
const store = useConfigStore();
const { metadata, config } = storeToRefs(store);

const refreshing = ref(false);
const networkPort = ref<number | null>(null);

async function refreshDiagnostics() {
  if (refreshing.value) return;
  refreshing.value = true;
  try {
    await Promise.all([store.fetchMetadata(), fetchNetworkPort()]);
  } finally {
    refreshing.value = false;
  }
}

async function fetchNetworkPort() {
  try {
    const r = await http.get('/api/config');
    if (r.status === 200 && r.data && typeof r.data.port !== 'undefined') {
      const p = Number(r.data.port);
      networkPort.value = Number.isFinite(p) ? p : null;
    }
  } catch (_) {
    /* non-fatal */
  }
}

onMounted(() => {
  void refreshDiagnostics();
});

// ---------- Computed accessors -------------------------------------------

const isWindows = computed(() => metadata.value?.platform === 'windows');

const luminalShineVersion = computed(() => metadata.value?.version || '');
const commitShort = computed(() => {
  const c = metadata.value?.commit || '';
  return c.length > 7 ? c.substring(0, 7) : c;
});
const branch = computed(() => metadata.value?.branch || '');
const releaseDate = computed(() => metadata.value?.release_date || '');
const prereleaseLabel = computed(() => {
  const p = metadata.value?.prerelease;
  if (!p) return '';
  return p; // e.g. "alpha.7", "beta.2"
});

// OS — Insider takes precedence over GA display
const osChannelLabel = computed(() => {
  const ins = metadata.value?.windows_insider;
  if (ins?.is_insider && ins.branch_name) {
    return ins.branch_name; // "Canary", "Dev", "Beta", "ReleasePreview"
  }
  return t('about.channel_release');
});
const osIsInsider = computed(() => !!metadata.value?.windows_insider?.is_insider);
const osVersionLabel = computed(() => {
  const md = metadata.value;
  if (!md) return '';
  // Insider hosts: prefer build number (e.g. "Build 27842") because
  // display_version (24H2) is only meaningful on stable channels.
  if (osIsInsider.value && md.windows_build_number) {
    return `${t('about.build_prefix')} ${md.windows_build_number}`;
  }
  // GA hosts: prefer display_version (24H2 etc.) with build in parens.
  const dv = md.windows_display_version;
  const bn = md.windows_build_number;
  if (dv && bn) return `${dv} (${t('about.build_prefix')} ${bn})`;
  if (dv) return dv;
  if (bn) return `${t('about.build_prefix')} ${bn}`;
  return '';
});
const osProductName = computed(() => metadata.value?.windows_product_name || '');

// GPUs — combine metadata.gpus[] (vendor/name/VRAM from DXGI) with
// metadata.gpu_drivers[] (driver version/date from registry) by matching
// on vendor_id + device_id. The first GPU in the gpus[] list is the one
// LuminalShine treats as the primary capture/encode adapter.
type GpuRow = {
  description: string;
  vendor: 'nvidia' | 'amd' | 'intel' | 'other';
  vendor_id?: number;
  device_id?: number;
  vram_bytes?: number;
  driver_version: string;
  driver_date: string;
  api_label: string;
  is_active: boolean;
};

function vendorOf(vendor_id?: number | string): GpuRow['vendor'] {
  const v = typeof vendor_id === 'string' ? Number(vendor_id) : vendor_id;
  if (v === 0x10de) return 'nvidia';
  if (v === 0x1002 || v === 0x1022) return 'amd';
  if (v === 0x8086) return 'intel';
  return 'other';
}
function vendorApiLabel(vendor: GpuRow['vendor']): string {
  // Reflect what LuminalShine actually links against, not aspirational
  // SDKs. NVAPI is in the build for nvprefs; AGS / IGCL are not.
  if (vendor === 'nvidia') return 'NVAPI · Direct3D 11 / DXGI';
  return 'Direct3D 11 / DXGI';
}

const gpus = computed<GpuRow[]>(() => {
  const md = metadata.value;
  if (!md || !md.gpus) return [];
  const drivers = md.gpu_drivers || [];
  return md.gpus.map((g, idx) => {
    const ven = vendorOf(g.vendor_id);
    // Match driver entry by vendor+device id. Numeric coercion guards
    // against the metadata serializer occasionally stringifying ints.
    const v = typeof g.vendor_id === 'string' ? Number(g.vendor_id) : g.vendor_id;
    const d = typeof g.device_id === 'string' ? Number(g.device_id) : g.device_id;
    const drv = drivers.find(
      (x) => x.vendor_id === v && x.device_id === d
    );
    const vram_raw = g.dedicated_video_memory;
    const vram_n = typeof vram_raw === 'string' ? Number(vram_raw) : vram_raw;
    return {
      description: g.description || drv?.description || '',
      vendor: ven,
      vendor_id: v as number | undefined,
      device_id: d as number | undefined,
      vram_bytes: typeof vram_n === 'number' && Number.isFinite(vram_n) ? vram_n : undefined,
      driver_version: drv?.driver_version || '',
      driver_date: drv?.driver_date || '',
      api_label: vendorApiLabel(ven),
      is_active: idx === 0,
    };
  });
});

function formatVram(bytes?: number): string {
  if (!bytes || !Number.isFinite(bytes)) return '';
  const gb = bytes / (1024 * 1024 * 1024);
  if (gb >= 1) return `${gb.toFixed(gb >= 10 ? 0 : 1)} GB`;
  const mb = bytes / (1024 * 1024);
  return `${Math.round(mb)} MB`;
}

// Encoders — the probe persists three booleans plus YUV444 flags.
// Until probed == true, every entry shows as "Probing…" rather than
// "Unavailable" so a cold-start screenshot doesn't lie about the host.
type EncoderRow = {
  name: string;
  available: boolean | null; // null = probe not yet completed
  yuv444: boolean;
};
const encoders = computed<EncoderRow[]>(() => {
  const md = metadata.value;
  const probe = md?.encoder_probe;
  const probed = !!probe?.probed;
  return [
    {
      name: 'H.264',
      available: probed ? !!probe?.h264_available : null,
      yuv444: !!probe?.h264_yuv444,
    },
    {
      name: 'HEVC',
      available: probed ? !!probe?.hevc_available : null,
      yuv444: !!probe?.hevc_yuv444,
    },
    {
      name: 'AV1',
      available: probed ? !!probe?.av1_available : null,
      yuv444: !!probe?.av1_yuv444,
    },
  ];
});
const refFramesInvalidation = computed(
  () => !!metadata.value?.encoder_probe?.ref_frames_invalidation,
);

// Displays — HDR support and current state per active path.
type DisplayRow = {
  display_name: string;
  friendly_name: string;
  hdr_supported: boolean;
  hdr_enabled: boolean;
};
const displays = computed<DisplayRow[]>(() => {
  const arr = metadata.value?.displays || [];
  return arr.map((d) => ({
    display_name: d.display_name || '',
    friendly_name: d.friendly_name || '',
    hdr_supported: !!d.advanced_color_supported,
    hdr_enabled: !!d.advanced_color_enabled,
  }));
});

// Virtual display
const virtualDisplayBackend = computed(() => {
  const b = metadata.value?.virtual_display_backend || 'none';
  switch (b.toLowerCase()) {
    case 'sudovda':
      return 'SudoVDA';
    case 'none':
      return t('about.value_none');
    default:
      return b;
  }
});
const virtualDisplayVersion = computed(() => metadata.value?.virtual_display_backend_version || '');
const virtualDisplayStatusLabel = computed(() => {
  const code = metadata.value?.virtual_display_driver_status;
  const s = code != null ? String(code) : '';
  switch (s) {
    case '0':
      return t('about.vdd_status_ready');
    case '1':
      return t('about.vdd_status_unknown');
    case '-1':
      return t('about.vdd_status_failed');
    case '-2':
      return t('about.vdd_status_version_incompatible');
    case '-3':
      return t('about.vdd_status_watchdog_failed');
    default:
      return s || t('about.value_unknown');
  }
});

// Network
const hostname = computed(() => {
  const c = (config.value as any) || {};
  return c.sunshine_name || '';
});
const sessionCount = computed(() => metadata.value?.active_session_count ?? 0);

// HAGS-related runtime hint (config setting that's relevant when
// triaging NVENC issues). Sources from the config payload, not metadata.
const realtimeHags = computed(() => {
  const c = (config.value as any) || {};
  const v = String(c.nvenc_realtime_hags ?? '').toLowerCase();
  if (v === 'enabled' || v === 'true' || v === '1') return t('about.value_enabled');
  if (v === 'disabled' || v === 'false' || v === '0') return t('about.value_disabled');
  return t('about.value_unknown');
});

// ---------- Diagnostics export ------------------------------------------

function buildDiagnosticsText(format: 'plain' | 'markdown'): string {
  const md = metadata.value || {};
  const lines: string[] = [];
  const head = format === 'markdown' ? '## ' : '';
  const sub = format === 'markdown' ? '### ' : '';
  const fence = format === 'markdown' ? '```' : '';

  lines.push(`${head}LuminalShine Diagnostics`);
  lines.push('');
  if (format === 'markdown') lines.push(fence);
  lines.push(`Generated: ${new Date().toISOString()}`);
  lines.push('');

  lines.push(`${sub}LuminalShine`);
  lines.push(`Version:        ${luminalShineVersion.value}`);
  if (commitShort.value) lines.push(`Commit:         ${commitShort.value}`);
  if (branch.value) lines.push(`Branch:         ${branch.value}`);
  if (releaseDate.value) lines.push(`Release:        ${releaseDate.value}`);
  if (prereleaseLabel.value) lines.push(`Prerelease:     ${prereleaseLabel.value}`);
  lines.push('');

  lines.push(`${sub}Operating System`);
  if (osProductName.value) lines.push(`Edition:        ${osProductName.value}`);
  lines.push(`Channel:        ${osChannelLabel.value}${osIsInsider.value ? ' (Insider)' : ''}`);
  lines.push(`Version:        ${osVersionLabel.value}`);
  lines.push('');

  lines.push(`${sub}Graphics`);
  for (const g of gpus.value) {
    lines.push(`  - ${g.description}${g.is_active ? '  [active]' : ''}`);
    if (g.vram_bytes) lines.push(`    VRAM:    ${formatVram(g.vram_bytes)}`);
    if (g.driver_version) {
      lines.push(
        `    Driver:  ${g.driver_version}${g.driver_date ? ` (released ${g.driver_date})` : ''}`,
      );
    }
    lines.push(`    API:     ${g.api_label}`);
  }
  lines.push('');

  lines.push(`${sub}Encoders`);
  for (const e of encoders.value) {
    const stateLabel =
      e.available === null
        ? 'Probing'
        : e.available
          ? `Available${e.yuv444 ? ' (YUV444)' : ''}`
          : 'Unavailable';
    lines.push(`  ${e.name.padEnd(6)} ${stateLabel}`);
  }
  if (refFramesInvalidation.value) lines.push('  Reference frame invalidation: yes');
  lines.push('');

  if (displays.value.length) {
    lines.push(`${sub}Display & HDR`);
    for (const d of displays.value) {
      const name =
        d.friendly_name && d.friendly_name !== d.display_name
          ? `${d.display_name} (${d.friendly_name})`
          : d.display_name;
      lines.push(
        `  ${name}: HDR ${d.hdr_supported ? 'supported' : 'not supported'}, ${
          d.hdr_enabled ? 'enabled' : 'off'
        }`,
      );
    }
    lines.push('');
  }

  lines.push(`${sub}Virtual Display`);
  lines.push(`Backend:        ${virtualDisplayBackend.value}`);
  if (virtualDisplayVersion.value) lines.push(`Version:        ${virtualDisplayVersion.value}`);
  lines.push(`Status:         ${virtualDisplayStatusLabel.value}`);
  lines.push('');

  lines.push(`${sub}Runtime`);
  if (hostname.value) lines.push(`Hostname:       ${hostname.value}`);
  if (networkPort.value !== null) lines.push(`Port:           ${networkPort.value}`);
  lines.push(`Active streams: ${sessionCount.value}`);
  lines.push(`HAGS hint:      ${realtimeHags.value}`);

  if (format === 'markdown') lines.push(fence);
  return lines.join('\n');
}

async function copyToClipboard(format: 'plain' | 'markdown') {
  const text = buildDiagnosticsText(format);
  try {
    await navigator.clipboard.writeText(text);
    message.success(
      format === 'markdown' ? t('about.copied_markdown') : t('about.copied_diagnostics'),
    );
  } catch (e: any) {
    message.error(t('about.copy_failed'));
  }
}
</script>

<template>
  <div class="about-page space-y-6 px-2 sm:space-y-8 md:px-4">
    <!-- Header / actions -->
    <section
      class="rounded-xl border border-dark/10 bg-light/70 p-4 shadow-sm backdrop-blur dark:border-light/10 dark:bg-surface/70 sm:p-5 md:p-6"
    >
      <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-4">
        <div class="min-w-0">
          <h2 class="text-xl md:text-2xl font-semibold tracking-tight">
            {{ $t('about.title') }}
          </h2>
          <p class="text-sm opacity-80 mt-1 leading-relaxed">
            {{ $t('about.description') }}
          </p>
        </div>
        <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center shrink-0">
          <n-button
            type="primary"
            strong
            :disabled="refreshing"
            :loading="refreshing"
            @click="refreshDiagnostics"
          >
            <i class="fas fa-arrows-rotate mr-2" />{{ $t('about.refresh') }}
          </n-button>
          <n-button @click="copyToClipboard('plain')">
            <i class="fas fa-copy mr-2" />{{ $t('about.copy_button') }}
          </n-button>
          <n-button @click="copyToClipboard('markdown')">
            <i class="fab fa-markdown mr-2" />{{ $t('about.copy_markdown_button') }}
          </n-button>
        </div>
      </div>
    </section>

    <!-- LuminalShine -->
    <n-card :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fas fa-bolt mr-2 opacity-70" />{{ $t('about.section_luminalshine') }}
        </h3>
      </template>
      <dl class="space-y-2">
        <div class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_version') }}</dt>
          <dd class="font-medium text-right break-all">
            {{ luminalShineVersion || $t('about.value_unknown') }}
          </dd>
        </div>
        <div v-if="prereleaseLabel" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_prerelease') }}</dt>
          <dd class="text-right">
            <n-tag size="small" type="warning">{{ prereleaseLabel }}</n-tag>
          </dd>
        </div>
        <div v-if="commitShort" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_commit') }}</dt>
          <dd class="font-mono text-right break-all">{{ commitShort }}</dd>
        </div>
        <div v-if="branch" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_branch') }}</dt>
          <dd class="font-mono text-right break-all">{{ branch }}</dd>
        </div>
        <div v-if="releaseDate" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_release_date') }}</dt>
          <dd class="text-right">{{ releaseDate }}</dd>
        </div>
      </dl>
    </n-card>

    <!-- Operating System -->
    <n-card v-if="isWindows" :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fab fa-windows mr-2 opacity-70" />{{ $t('about.section_os') }}
        </h3>
      </template>
      <dl class="space-y-2">
        <div v-if="osProductName" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_edition') }}</dt>
          <dd class="text-right break-all">{{ osProductName }}</dd>
        </div>
        <div class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_channel') }}</dt>
          <dd class="text-right">
            <n-tag size="small" :type="osIsInsider ? 'warning' : 'success'">
              {{ osChannelLabel }}{{ osIsInsider ? ` · ${$t('about.value_insider')}` : '' }}
            </n-tag>
          </dd>
        </div>
        <div v-if="osVersionLabel" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_os_version') }}</dt>
          <dd class="text-right break-all">{{ osVersionLabel }}</dd>
        </div>
      </dl>
    </n-card>

    <!-- Graphics — one card per detected GPU -->
    <n-card v-if="gpus.length" :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fas fa-microchip mr-2 opacity-70" />{{ $t('about.section_graphics') }}
        </h3>
      </template>
      <div class="space-y-4">
        <div
          v-for="(g, idx) in gpus"
          :key="idx"
          class="rounded-md border border-dark/10 dark:border-light/10 p-3"
        >
          <div class="flex items-center justify-between gap-2 mb-2 flex-wrap">
            <h4 class="font-semibold text-sm sm:text-base break-all">
              {{ g.description || $t('about.value_unknown') }}
            </h4>
            <n-tag v-if="g.is_active" size="small" type="primary">
              {{ $t('about.value_active') }}
            </n-tag>
          </div>
          <dl class="space-y-1.5 text-sm">
            <div v-if="g.vendor !== 'other'" class="flex justify-between gap-4">
              <dt class="opacity-70">{{ $t('about.label_vendor') }}</dt>
              <dd class="text-right uppercase">{{ g.vendor }}</dd>
            </div>
            <div v-if="g.vram_bytes" class="flex justify-between gap-4">
              <dt class="opacity-70">{{ $t('about.label_vram') }}</dt>
              <dd class="text-right">{{ formatVram(g.vram_bytes) }}</dd>
            </div>
            <div v-if="g.driver_version" class="flex justify-between gap-4">
              <dt class="opacity-70">{{ $t('about.label_driver_version') }}</dt>
              <dd class="font-mono text-right break-all">{{ g.driver_version }}</dd>
            </div>
            <div v-if="g.driver_date" class="flex justify-between gap-4">
              <dt class="opacity-70">{{ $t('about.label_driver_date') }}</dt>
              <dd class="text-right">{{ g.driver_date }}</dd>
            </div>
            <div class="flex justify-between gap-4">
              <dt class="opacity-70">{{ $t('about.label_api') }}</dt>
              <dd class="text-right break-all">{{ g.api_label }}</dd>
            </div>
          </dl>
        </div>
      </div>
    </n-card>

    <!-- Encoders -->
    <n-card v-if="isWindows" :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fas fa-film mr-2 opacity-70" />{{ $t('about.section_encoders') }}
        </h3>
      </template>
      <dl class="space-y-2">
        <div
          v-for="e in encoders"
          :key="e.name"
          class="flex justify-between gap-4 text-sm items-center"
        >
          <dt class="opacity-70 font-mono">{{ e.name }}</dt>
          <dd class="text-right">
            <n-tag
              v-if="e.available === null"
              size="small"
              type="default"
            >{{ $t('about.value_probing') }}</n-tag>
            <n-tag
              v-else-if="e.available"
              size="small"
              type="success"
            >
              {{ $t('about.value_available')
              }}{{ e.yuv444 ? ` · ${$t('about.value_yuv444')}` : '' }}
            </n-tag>
            <n-tag v-else size="small" type="error">{{ $t('about.value_unavailable') }}</n-tag>
          </dd>
        </div>
        <div
          v-if="refFramesInvalidation"
          class="flex justify-between gap-4 text-sm pt-2 border-t border-dark/10 dark:border-light/10"
        >
          <dt class="opacity-70">{{ $t('about.label_ref_frame_invalidation') }}</dt>
          <dd class="text-right">{{ $t('about.value_supported') }}</dd>
        </div>
      </dl>
    </n-card>

    <!-- Display & HDR -->
    <n-card v-if="isWindows && displays.length" :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fas fa-display mr-2 opacity-70" />{{ $t('about.section_display') }}
        </h3>
      </template>
      <div class="space-y-3">
        <div
          v-for="(d, idx) in displays"
          :key="idx"
          class="rounded-md border border-dark/10 dark:border-light/10 p-3"
        >
          <div class="flex items-center justify-between gap-2 mb-1 flex-wrap">
            <div class="text-sm font-mono break-all">{{ d.display_name }}</div>
          </div>
          <div v-if="d.friendly_name" class="text-xs opacity-70 mb-2 break-all">
            {{ d.friendly_name }}
          </div>
          <div class="flex flex-wrap gap-2 text-xs">
            <n-tag size="small" :type="d.hdr_supported ? 'success' : 'default'">
              {{
                d.hdr_supported
                  ? $t('about.value_hdr_supported')
                  : $t('about.value_hdr_not_supported')
              }}
            </n-tag>
            <n-tag size="small" :type="d.hdr_enabled ? 'primary' : 'default'">
              {{ d.hdr_enabled ? $t('about.value_hdr_on') : $t('about.value_hdr_off') }}
            </n-tag>
          </div>
        </div>
      </div>
    </n-card>

    <!-- Virtual Display -->
    <n-card v-if="isWindows" :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fas fa-tv mr-2 opacity-70" />{{ $t('about.section_virtual_display') }}
        </h3>
      </template>
      <dl class="space-y-2">
        <div class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_vdd_backend') }}</dt>
          <dd class="text-right break-all">{{ virtualDisplayBackend }}</dd>
        </div>
        <div v-if="virtualDisplayVersion" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_vdd_version') }}</dt>
          <dd class="font-mono text-right break-all">{{ virtualDisplayVersion }}</dd>
        </div>
        <div class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_vdd_status') }}</dt>
          <dd class="text-right">{{ virtualDisplayStatusLabel }}</dd>
        </div>
      </dl>
    </n-card>

    <!-- Runtime / Network -->
    <n-card :segmented="{ content: true }">
      <template #header>
        <h3 class="text-base sm:text-lg font-semibold tracking-tight">
          <i class="fas fa-network-wired mr-2 opacity-70" />{{ $t('about.section_runtime') }}
        </h3>
      </template>
      <dl class="space-y-2">
        <div v-if="hostname" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_hostname') }}</dt>
          <dd class="text-right break-all">{{ hostname }}</dd>
        </div>
        <div v-if="networkPort !== null" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_port') }}</dt>
          <dd class="text-right">{{ networkPort }}</dd>
        </div>
        <div class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_active_sessions') }}</dt>
          <dd class="text-right">{{ sessionCount }}</dd>
        </div>
        <div v-if="isWindows" class="flex justify-between gap-4 text-sm">
          <dt class="opacity-70">{{ $t('about.label_realtime_hags') }}</dt>
          <dd class="text-right">{{ realtimeHags }}</dd>
        </div>
      </dl>
    </n-card>

    <n-alert
      v-if="!isWindows"
      type="info"
      :show-icon="true"
      class="mt-2"
    >
      {{ $t('about.notice_windows_only') }}
    </n-alert>
  </div>
</template>

<style scoped>
.about-page :deep(.n-card-header__main) {
  width: 100%;
}
</style>
