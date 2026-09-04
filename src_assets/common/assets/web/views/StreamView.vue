<script setup lang="ts">
/**
 * Stream — every recorded session on the left, the selected session's
 * telemetry in the middle (three single-series charts with one range
 * selector), and its identity, connection quality and host utilization in
 * the inspector. This is also the whole UI for the stats-only role.
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { storeToRefs } from 'pinia';
import { NButton, useDialog, useMessage } from 'naive-ui';
import { http } from '@/http';
import { useT2 } from '@/composables/useT2';
import { useHostStore, type SessionSummary } from '@/stores/host';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import { RANGES, useSessionDetail, type RangeKey } from '@/composables/useSessionDetail';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import LinkButton from '@/components/shell/LinkButton.vue';
import LineChart from '@/components/charts/LineChart.vue';

const t2 = useT2();
const route = useRoute();
const router = useRouter();
const host = useHostStore();
const auth = useAuthStore();
const configStore = useConfigStore();
const { metadata } = storeToRefs(configStore);
const dialog = useDialog();
const message = useMessage();

const statsOnly = computed(() => auth.isStatsOnly());

// ---- clock ----------------------------------------------------------------
const nowSec = ref(Math.floor(Date.now() / 1000));
let clock: ReturnType<typeof setInterval> | null = null;

// ---- selection (URL-addressable) -----------------------------------------
const selectedId = computed<string | null>(() => {
  const q = route.query['id'];
  const fromQuery = typeof q === 'string' && q ? q : null;
  if (fromQuery) return fromQuery;
  return host.activeSession?.id ?? host.sessions[0]?.id ?? null;
});
function select(id: string): void {
  void router.replace({ path: route.path, query: { ...route.query, id } });
}

const range = ref<RangeKey>('full');
const detail = useSessionDetail(selectedId, range);

const RANGE_LABELS: Record<RangeKey, [string, string]> = {
  full: ['stream.range_full', 'Full session'],
  '3m': ['stream.range_3m', '3 min'],
  '5m': ['stream.range_5m', '5 min'],
  '15m': ['stream.range_15m', '15 min'],
};

// ---- formatting -----------------------------------------------------------
function fmtNum(v: number | null, decimals = 1): string {
  if (v == null || !Number.isFinite(v)) return '—';
  return Number.isInteger(v) ? String(v) : v.toFixed(decimals);
}
function fmtDuration(startSec: number, endSec: number | null): string {
  const end = endSec ?? nowSec.value;
  const secs = Math.max(0, end - startSec);
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  const s = secs % 60;
  if (h > 0) return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}
function fmtDurationShort(startSec: number, endSec: number | null): string {
  const end = endSec ?? nowSec.value;
  const secs = Math.max(0, end - startSec);
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  return h > 0 ? `${h} h ${m} min` : `${m} min`;
}
function fmtStart(epochSec: number): string {
  const d = new Date(epochSec * 1000);
  const today = new Date();
  const sameDay =
    d.getFullYear() === today.getFullYear() &&
    d.getMonth() === today.getMonth() &&
    d.getDate() === today.getDate();
  return sameDay
    ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    : d.toLocaleDateString([], { month: 'short', day: 'numeric' });
}
function sessionApp(s: SessionSummary): string {
  return s.metadata?.application?.trim() || t2('overview.streaming', 'Streaming');
}
function sessionClient(s: SessionSummary): string {
  return s.metadata?.client_name?.trim() || t2('overview.unknown_client', 'unknown client');
}
function sessionFormat(m: SessionSummary['metadata'] | undefined): string {
  const parts: string[] = [];
  if (m?.codec) parts.push(m.codec);
  if (m?.resolution_w && m.resolution_h) {
    parts.push(`${m.resolution_w}×${m.resolution_h}${m.fps ? ` @${m.fps}` : ''}`);
  }
  return parts.join(' · ');
}

// ---- header numbers -------------------------------------------------------
const current = computed(() => detail.session.value);
const headerApp = computed(
  () => current.value?.metadata?.application?.trim() || t2('overview.streaming', 'Streaming'),
);
const headerClient = computed(
  () =>
    current.value?.metadata?.client_name?.trim() || t2('overview.unknown_client', 'unknown client'),
);
const formatLine = computed(() => {
  const m = current.value?.metadata;
  const parts = [sessionFormat(m)];
  if (m?.hdr === true) parts.push('HDR');
  if (m?.yuv444 === true || m?.yuv444 === 'true') parts.push('4:4:4');
  if (m?.protocol) parts.push(String(m.protocol));
  return parts.filter(Boolean).join(' · ');
});
const healthTag = computed(() => {
  const h = detail.health.value;
  if (h === 'healthy') return { text: t2('stream.health_healthy', 'Healthy'), cls: 'mc-tag-ok' };
  if (h === 'poor') return { text: t2('stream.health_poor', 'Poor'), cls: 'mc-tag-warn' };
  return null;
});

const metrics = computed(() => [
  {
    key: 'fps',
    label: t2('overview.metric_fps', 'Actual FPS'),
    value: fmtNum(detail.last('actual_fps'), 0),
    unit: '',
  },
  {
    key: 'mbps',
    label: t2('stream.metric_throughput', 'Throughput'),
    value: fmtNum(detail.last('network_throughput_mbps'), 0),
    unit: 'Mbps',
  },
  {
    key: 'enc',
    label: t2('overview.metric_encode', 'Encode'),
    value: fmtNum(detail.last('encode_latency_ms')),
    unit: 'ms',
  },
  {
    key: 'target',
    label: t2('overview.metric_target', 'Target'),
    value: fmtNum(current.value?.metadata?.bitrate_mbps_target ?? null, 0),
    unit: 'Mbps',
  },
  {
    key: 'avgfps',
    label: t2('stream.metric_avg_fps', 'Avg FPS'),
    value: fmtNum(detail.avg('actual_fps'), 0),
    unit: '',
  },
  {
    key: 'avgmbps',
    label: t2('stream.metric_avg_throughput', 'Avg throughput'),
    value: fmtNum(detail.avg('network_throughput_mbps'), 0),
    unit: 'Mbps',
  },
]);

const charts = computed(() => [
  {
    key: 'encode_latency_ms',
    title: t2('stream.chart_encode', 'Encode latency (ms)'),
    unit: 'ms',
    stat: `${t2('stream.now', 'now')} ${fmtNum(detail.last('encode_latency_ms'))} ms · ${t2('stream.avg', 'avg')} ${fmtNum(detail.avg('encode_latency_ms'))} · ${t2('stream.peak', 'peak')} ${fmtNum(detail.peak('encode_latency_ms'))}`,
    decimals: 1,
  },
  {
    key: 'actual_fps',
    title: t2('stream.chart_fps', 'Actual FPS'),
    unit: 'fps',
    stat: `${t2('stream.now', 'now')} ${fmtNum(detail.last('actual_fps'), 0)} · ${t2('stream.target', 'target')} ${fmtNum(current.value?.metadata?.fps ?? null, 0)} · ${t2('stream.avg', 'avg')} ${fmtNum(detail.avg('actual_fps'), 0)}`,
    decimals: 0,
  },
  {
    key: 'network_throughput_mbps',
    title: t2('stream.chart_throughput', 'Network throughput (Mbps)'),
    unit: 'Mbps',
    stat: `${t2('stream.now', 'now')} ${fmtNum(detail.last('network_throughput_mbps'), 0)} Mbps · ${t2('stream.target', 'target')} ${fmtNum(current.value?.metadata?.bitrate_mbps_target ?? null, 0)} · ${t2('stream.peak', 'peak')} ${fmtNum(detail.peak('network_throughput_mbps'), 0)}`,
    decimals: 0,
  },
]);

// ---- inspector data -------------------------------------------------------
const identityRows = computed(() => {
  const m = current.value?.metadata ?? {};
  const meta = metadata.value ?? {};
  const rows: Array<[string, string]> = [];
  const push = (k: string, v: unknown) => {
    if (v == null || v === '') return;
    rows.push([k, String(v)]);
  };
  push(t2('stream.id_host', 'Host'), meta.host_name);
  push(t2('stream.id_device', 'Device'), m.client_name || m.device);
  push(
    t2('stream.id_resolution', 'Resolution'),
    m.resolution_w && m.resolution_h
      ? `${m.resolution_w}×${m.resolution_h}${m.fps ? ` @${m.fps}` : ''}`
      : undefined,
  );
  push(t2('stream.id_codec', 'Codec'), m.codec);
  push(
    t2('stream.id_hdr', 'HDR'),
    m.hdr === true ? t2('stream.on', 'on') : m.hdr === false ? t2('stream.off', 'off') : undefined,
  );
  push(t2('stream.id_audio', 'Audio'), m.audio_channels ? `${m.audio_channels} ch` : undefined);
  push(t2('stream.id_app', 'Running app'), m.application);
  push(t2('stream.id_cpu', 'CPU'), m.cpu_model || meta.cpu_model);
  push(t2('stream.id_gpu', 'GPU'), m.gpu_model);
  push(t2('stream.id_vgd', 'LuminalVGD'), meta.virtual_display_backend_version);
  push(t2('stream.id_version', 'LuminalShine'), m.luminalshine_version || meta.version);
  return rows;
});

const qualityCounters = computed(() => [
  { label: t2('stream.q_losses', 'Losses'), value: fmtNum(detail.sum('client_losses'), 0) },
  { label: t2('stream.q_idr', 'IDR requests'), value: fmtNum(detail.sum('idr_requests'), 0) },
  {
    label: t2('stream.q_ref', 'Ref invalidations'),
    value: fmtNum(detail.sum('ref_invalidations'), 0),
  },
  { label: t2('stream.q_gpu', 'GPU resets'), value: fmtNum(detail.sum('gpu_resets'), 0) },
]);

const METER_KEYS: Array<[string, string]> = [
  ['CPU', 'host_cpu_pct'],
  ['GPU', 'host_gpu_pct'],
  ['Encoder', 'host_gpu_encoder_pct'],
  ['RAM', 'host_ram_pct'],
  ['VRAM', 'host_vram_pct'],
];
const meters = computed(() =>
  METER_KEYS.map(([label, key]) => {
    const v = detail.last(key);
    return { label, pct: v == null ? null : Math.max(0, Math.min(100, Math.round(v))) };
  }),
);

// ---- actions --------------------------------------------------------------
function exportSession(): void {
  const id = selectedId.value;
  if (!id) return;
  // The proxy endpoint sets Content-Disposition: attachment, so a plain
  // navigation triggers the browser's download flow.
  window.location.href = `./api/sessions/${encodeURIComponent(id)}/export.json`;
}

function disconnectSession(): void {
  const s = current.value;
  if (!s || !detail.isActive.value) return;
  dialog.warning({
    title: t2('stream.disconnect_title', 'Disconnect this streaming session?'),
    content: t2(
      'stream.disconnect_body',
      'The connected client will be dropped immediately. The session history and recorded telemetry stay here.',
    ),
    positiveText: t2('stream.disconnect', 'Disconnect'),
    negativeText: t2('_common.cancel', 'Cancel'),
    onPositiveClick: async () => {
      const m = s.metadata ?? {};
      // client_uuid is the pairing UUID the disconnect endpoint expects;
      // device/client_name are display strings kept only as a fallback for
      // sessions recorded before the uuid landed in the metadata.
      const clientUuid = (m.client_uuid || m.device || m.client_name || '').trim();
      try {
        const r = await http.post(
          './api/clients/disconnect',
          { uuid: clientUuid },
          { validateStatus: () => true },
        );
        if (r.status >= 200 && r.status < 300)
          message.success(t2('stream.disconnect_sent', 'Disconnect request sent.'));
        else
          message.error(
            `${t2('stream.disconnect_failed', 'Disconnect failed')} (HTTP ${r.status}).`,
          );
      } catch (e) {
        message.error(
          e instanceof Error ? e.message : t2('stream.disconnect_failed', 'Disconnect failed'),
        );
      }
    },
  });
}

function deleteSession(): void {
  const id = selectedId.value;
  if (!id || detail.isActive.value) return;
  dialog.warning({
    title: t2('stream.delete_title', 'Delete this session?'),
    content: t2(
      'stream.delete_body',
      'The recorded telemetry is removed from the host and the entry disappears from the list. Only ended sessions can be deleted.',
    ),
    positiveText: t2('stream.delete', 'Delete'),
    negativeText: t2('_common.cancel', 'Cancel'),
    onPositiveClick: async () => {
      try {
        const r = await http.delete(`./api/sessions/${encodeURIComponent(id)}`, {
          validateStatus: () => true,
        });
        if (r.status >= 200 && r.status < 300) {
          message.success(t2('stream.deleted', 'Session deleted.'));
          host.removeSession(id);
          const next = host.sessions[0]?.id;
          void router.replace({ path: route.path, query: next ? { id: next } : {} });
        } else if (r.status === 400) {
          message.warning(
            t2('stream.delete_active', 'Cannot delete an active session. Disconnect it first.'),
          );
        } else {
          message.error(`${t2('stream.delete_failed', 'Delete failed')} (HTTP ${r.status}).`);
        }
      } catch (e) {
        message.error(e instanceof Error ? e.message : t2('stream.delete_failed', 'Delete failed'));
      }
    },
  });
}

onMounted(async () => {
  clock = setInterval(() => (nowSec.value = Math.floor(Date.now() / 1000)), 1000);
  await auth.waitForAuthentication();
  if (!host.running) void host.start();
  if (!metadata.value?.version) void configStore.fetchMetadata();
});
onBeforeUnmount(() => {
  if (clock) clearInterval(clock);
});
</script>

<template>
  <div class="flex gap-3.5">
    <!-- Sessions list -->
    <section class="mc-panel w-[280px] shrink-0 self-start">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('stream.sessions', 'Sessions') }}</span>
        <span class="text-[11px] text-ink-4">{{ host.sessions.length }}</span>
      </div>
      <div v-if="host.monitorOffline" class="px-4 py-3 text-xs text-ink-4">
        {{
          t2(
            'overview.monitor_offline',
            'The session monitor service is offline; live telemetry will appear once LuminalShineSessionMonitor is running.',
          )
        }}
      </div>
      <div
        v-else-if="host.sessionsLoaded && host.sessions.length === 0"
        class="px-4 py-3 text-xs text-ink-4"
      >
        {{ t2('overview.no_sessions', 'No sessions recorded yet.') }}
      </div>
      <button
        v-for="s in host.sessions"
        :key="s.id"
        type="button"
        class="mc-row mc-row-clickable w-full text-left"
        :class="{ 'mc-row-selected': s.id === selectedId }"
        @click="select(s.id)"
      >
        <div class="min-w-0">
          <div class="mc-row-primary truncate">{{ sessionApp(s) }} · {{ sessionClient(s) }}</div>
          <div class="mc-row-secondary truncate">
            {{
              [
                sessionFormat(s.metadata),
                fmtDurationShort(s.started_at, s.stream_ended_at),
                s.stream_ended_at != null ? fmtStart(s.started_at) : '',
              ]
                .filter(Boolean)
                .join(' · ')
            }}
          </div>
        </div>
        <span class="mc-tag" :class="s.stream_ended_at == null ? 'mc-tag-live' : ''">
          {{
            s.stream_ended_at == null
              ? t2('overview.live', 'Live')
              : t2('overview.ended_tag', 'Ended')
          }}
        </span>
      </button>
      <div class="border-t border-line px-4 py-2.5 text-[11px] text-ink-4">
        {{ t2('stream.list_footer', 'List refreshes every 5 s') }}
      </div>
    </section>

    <!-- Telemetry -->
    <div class="flex min-w-0 flex-1 flex-col gap-3.5">
      <section v-if="!selectedId" class="mc-panel px-4 py-6 text-center text-xs text-ink-4">
        {{ t2('stream.select_hint', 'Select a session to see its telemetry.') }}
      </section>
      <template v-else>
        <section class="mc-panel">
          <div class="mc-panel-h">
            <div class="flex min-w-0 items-center gap-2.5">
              <span
                v-if="current"
                class="mc-tag"
                :class="detail.isActive.value ? 'mc-tag-live' : ''"
              >
                <span v-if="detail.isActive.value" class="mc-dot mc-dot-gold mc-dot-pulse"></span>
                {{
                  detail.isActive.value
                    ? t2('overview.live', 'Live')
                    : t2('overview.ended_tag', 'Ended')
                }}
              </span>
              <span class="truncate text-sm font-semibold">{{
                current
                  ? headerApp
                  : detail.monitorOffline.value
                    ? t2('overview.monitor_offline_short', 'Session monitor offline.')
                    : detail.errorText.value || '…'
              }}</span>
              <template v-if="current">
                <span class="text-ink-4">→</span>
                <span class="truncate text-[13px] text-ink-2">{{ headerClient }}</span>
                <span class="font-mono text-xs text-ink-3">{{
                  fmtDuration(current.started_at, current.stream_ended_at)
                }}</span>
                <span v-if="healthTag" class="mc-tag" :class="healthTag.cls">{{
                  healthTag.text
                }}</span>
              </template>
            </div>
            <div class="flex shrink-0 items-center gap-2">
              <NButton size="small" :disabled="!current" @click="exportSession">{{
                t2('stream.export', 'Export JSON')
              }}</NButton>
              <NButton
                v-if="!statsOnly && current && detail.isActive.value"
                size="small"
                type="error"
                @click="disconnectSession"
                >{{ t2('stream.disconnect', 'Disconnect') }}</NButton
              >
              <NButton
                v-if="!statsOnly && current && !detail.isActive.value"
                size="small"
                type="error"
                @click="deleteSession"
                >{{ t2('stream.delete', 'Delete') }}</NButton
              >
            </div>
          </div>
          <div class="grid grid-cols-3 gap-3 px-4 pb-3.5 pt-3.5 sm:grid-cols-6">
            <div v-for="m in metrics" :key="m.key">
              <div class="mc-kicker">{{ m.label }}</div>
              <div class="mc-metric-value">
                {{ m.value
                }}<span v-if="m.unit && m.value !== '—'" class="mc-metric-unit">{{ m.unit }}</span>
              </div>
            </div>
          </div>
          <div v-if="formatLine" class="px-4 pb-3.5 text-[11.5px] text-ink-3">{{ formatLine }}</div>
        </section>

        <div class="flex flex-wrap items-center justify-between gap-2">
          <div class="mc-chips" role="tablist">
            <button
              v-for="r in RANGES"
              :key="r.key"
              type="button"
              class="mc-chip"
              :class="{ 'mc-chip-on': range === r.key }"
              role="tab"
              :aria-selected="range === r.key"
              @click="range = r.key"
            >
              {{ t2(RANGE_LABELS[r.key][0], RANGE_LABELS[r.key][1]) }}
            </button>
          </div>
          <span class="text-[11.5px] text-ink-4">
            {{
              detail.isActive.value
                ? t2(
                    'stream.polling_note',
                    'Polled every 2 s while live · full session decimated to 900 buckets',
                  )
                : t2('stream.static_note', 'Ended session · full session decimated to 900 buckets')
            }}
          </span>
        </div>

        <section v-for="c in charts" :key="c.key" class="mc-panel">
          <div class="mc-panel-h">
            <span class="mc-panel-title">{{ c.title }}</span>
            <span class="font-mono text-xs text-ink-3">{{ c.stat }}</span>
          </div>
          <div class="px-3 pb-2 pt-2">
            <LineChart
              v-if="detail.points(c.key).length > 0"
              :points="detail.points(c.key)"
              :label="c.title"
              :unit="c.unit"
              :decimals="c.decimals"
              :height="120"
            />
            <div v-else class="flex h-[120px] items-center justify-center text-xs text-ink-4">
              {{ detail.loading.value ? '…' : t2('stream.waiting', 'Waiting for telemetry…') }}
            </div>
          </div>
        </section>
      </template>
    </div>

    <!-- Inspector -->
    <InspectorPanel
      v-if="selectedId"
      :title="t2('stream.inspector_title', 'Session details')"
      :subtitle="
        current
          ? `${headerApp} · ${headerClient} · ${t2('stream.started', 'started')} ${fmtStart(current.started_at)}`
          : ''
      "
      :tag="
        current
          ? detail.isActive.value
            ? t2('overview.live', 'Live')
            : t2('overview.ended_tag', 'Ended')
          : ''
      "
      :tag-kind="current && detail.isActive.value ? 'live' : ''"
    >
      <div class="mc-kicker px-[18px] pb-1 pt-3">{{ t2('stream.identity', 'Identity') }}</div>
      <div class="grid grid-cols-[118px_minmax(0,1fr)] gap-x-3 gap-y-1 px-[18px] pb-2 text-xs">
        <template v-for="[k, v] in identityRows" :key="k">
          <span class="text-ink-3">{{ k }}</span>
          <span class="truncate text-ink">{{ v }}</span>
        </template>
        <span v-if="identityRows.length === 0" class="col-span-2 text-ink-4">—</span>
      </div>

      <div class="mc-kicker px-[18px] pb-1 pt-3">
        {{ t2('stream.quality', 'Connection quality') }} · {{ t2('stream.in_view', 'in view') }}
      </div>
      <div class="grid grid-cols-2 gap-2 px-[18px] pb-2">
        <div
          v-for="q in qualityCounters"
          :key="q.label"
          class="rounded-lg border border-line px-2.5 py-2"
        >
          <div class="mc-kicker">{{ q.label }}</div>
          <div class="text-lg font-semibold leading-tight">{{ q.value }}</div>
        </div>
      </div>

      <div class="mc-kicker px-[18px] pb-1 pt-3">
        {{ t2('stream.host_util', 'Host utilization') }}
      </div>
      <div
        v-for="m in meters"
        :key="m.label"
        class="grid grid-cols-[64px_minmax(0,1fr)_36px] items-center gap-2.5 px-[18px] py-[3px] text-[11.5px]"
      >
        <span class="text-ink-3">{{ m.label }}</span>
        <span class="relative h-1 rounded bg-[#22272D]"
          ><span
            class="absolute left-0 top-0 h-1 rounded bg-primary"
            :style="{ width: `${m.pct ?? 0}%` }"
          ></span
        ></span>
        <span class="text-right font-mono text-ink-2">{{ m.pct == null ? '—' : `${m.pct}%` }}</span>
      </div>

      <template #footer>
        <NButton size="small" :disabled="!current" @click="exportSession">{{
          t2('stream.export', 'Export JSON')
        }}</NButton>
        <LinkButton v-if="!statsOnly" to="/clients" size="small">{{
          t2('shell.nav_clients', 'Clients')
        }}</LinkButton>
      </template>
    </InspectorPanel>
  </div>
</template>
