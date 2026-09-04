<script setup lang="ts">
/**
 * Host strip — the always-on status bar under the top edge of every page.
 * Six facts about the host (host, encoder, display, network, monitor,
 * incidents) read from the host store, plus the global save state.
 */
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import { useHostStore } from '@/stores/host';
import { useConfigStore } from '@/stores/config';
import { useConnectivityStore } from '@/stores/connectivity';
import { useT2 } from '@/composables/useT2';
import SavingStatus from '@/components/SavingStatus.vue';
import GlobalSearch from '@/components/shell/GlobalSearch.vue';

const t2 = useT2();

const host = useHostStore();
const configStore = useConfigStore();
const connectivity = useConnectivityStore();
const { metadata, config } = storeToRefs(configStore);

const version = computed(() => metadata.value?.version || '');
const channel = computed(() => {
  const branch = String(metadata.value?.branch || '');
  if (branch && !['main', 'master'].includes(branch)) return branch;
  const pre = String(metadata.value?.prerelease || '').toLowerCase();
  if (pre === 'true' || pre === '1') return t2('shell.channel_prerelease', 'pre-release');
  return t2('shell.channel_stable', 'stable');
});

const ENCODER_LABELS: Record<string, string> = {
  nvenc: 'NVENC',
  amf: 'AMF',
  quicksync: 'QuickSync',
  qsv: 'QuickSync',
  software: 'Software',
  vaapi: 'VA-API',
  videotoolbox: 'VideoToolbox',
};
const encoderLabel = computed(() => {
  const raw = String((config.value as Record<string, unknown>)?.['encoder'] || '').toLowerCase();
  return raw ? (ENCODER_LABELS[raw] ?? raw.toUpperCase()) : t2('shell.encoder_auto', 'Auto');
});
const codecs = computed(() => {
  const probe = metadata.value?.encoder_probe;
  if (!probe?.probed) return '';
  const out: string[] = [];
  if (probe.h264_available) out.push('H.264');
  if (probe.hevc_available) out.push('HEVC');
  if (probe.av1_available) out.push('AV1');
  return out.join(' · ');
});

const displayText = computed(() => {
  const s = host.activeSession;
  if (s) {
    const m = s.metadata ?? {};
    const res =
      m.resolution_w && m.resolution_h
        ? `${m.resolution_w}×${m.resolution_h}${m.fps ? ` @${m.fps}` : ''}`
        : '';
    const backend = metadata.value?.capture_backend_display || '';
    return [backend, res].filter(Boolean).join(' · ') || t2('shell.display_streaming', 'Streaming');
  }
  const backend = String(metadata.value?.virtual_display_backend || '');
  if (backend) {
    const ver = metadata.value?.virtual_display_backend_version || '';
    const ready = metadata.value?.virtual_display_driver_ready;
    const state =
      ready === true
        ? t2('shell.display_ready', 'ready')
        : ready === false
          ? t2('shell.display_not_ready', 'not ready')
          : '';
    return [backend, ver, state].filter(Boolean).join(' · ');
  }
  return t2('shell.display_physical', 'Physical');
});

const networkText = computed(() => {
  const cfg = config.value as Record<string, unknown>;
  const port = String(cfg?.['port'] ?? 47989);
  const fam = String(cfg?.['address_family'] || 'ipv4').toLowerCase();
  const famLabel = fam === 'both' ? 'IPv4 + IPv6' : fam === 'ipv6' ? 'IPv6' : 'IPv4';
  return `${t2('shell.port', 'Port')} ${port} · ${famLabel}`;
});

const hostOnline = computed(() => !connectivity.offline);
</script>

<template>
  <div class="hidden h-14 shrink-0 items-center border-b border-line bg-chrome px-1.5 md:flex">
    <div class="strip-item">
      <span class="mc-kicker">{{ t2('shell.strip_host', 'Host') }}</span>
      <span class="strip-value">
        <span class="mc-dot" :class="hostOnline ? '' : 'mc-dot-red'"></span>
        <span>{{
          hostOnline ? t2('shell.host_online', 'Online') : t2('shell.host_offline', 'Offline')
        }}</span>
        <span v-if="version" class="text-ink-3">· {{ version }} {{ channel }}</span>
      </span>
    </div>
    <div class="strip-item">
      <span class="mc-kicker">{{ t2('shell.strip_encoder', 'Encoder') }}</span>
      <span class="strip-value"
        >{{ encoderLabel }}<span v-if="codecs" class="text-ink-3"> · {{ codecs }}</span></span
      >
    </div>
    <div class="strip-item">
      <span class="mc-kicker">{{ t2('shell.strip_display', 'Display') }}</span>
      <span class="strip-value">{{ displayText }}</span>
    </div>
    <div class="strip-item">
      <span class="mc-kicker">{{ t2('shell.strip_network', 'Network') }}</span>
      <span class="strip-value">{{ networkText }}</span>
    </div>
    <div class="strip-item">
      <span class="mc-kicker">{{ t2('shell.strip_monitor', 'Monitor') }}</span>
      <span class="strip-value">
        <span
          class="mc-dot"
          :class="host.monitorOffline ? 'mc-dot-red' : host.sessionsLoaded ? '' : 'mc-dot-grey'"
        ></span>
        <span>{{
          host.monitorOffline
            ? t2('shell.monitor_offline', 'Offline')
            : host.sessionsLoaded
              ? t2('shell.monitor_ok', 'OK · 5 s')
              : '…'
        }}</span>
      </span>
    </div>
    <div v-if="!host.statsOnly" class="strip-item border-r-0">
      <span class="mc-kicker">{{ t2('shell.strip_incidents', 'Incidents') }}</span>
      <span class="strip-value">{{ host.incidentSummary || '—' }}</span>
    </div>
    <div class="flex-1"></div>
    <div class="flex items-center gap-2.5 pr-3">
      <GlobalSearch v-if="!host.statsOnly" />
      <SavingStatus />
    </div>
  </div>
</template>

<style scoped>
.strip-item {
  display: flex;
  flex-direction: column;
  gap: 1px;
  padding: 0 18px;
  border-right: 1px solid rgba(255, 255, 255, 0.07);
  min-width: 0;
}
.strip-value {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12.5px;
  font-weight: 500;
  color: var(--mc-ink);
  white-space: nowrap;
}
</style>
