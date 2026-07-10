<script setup lang="ts">
/**
 * Session Details slide-out drawer. Opens from the right edge of
 * the viewport, hosts a sticky compact header (status + identity +
 * key numbers + action bar) above a tabbed chart region. Charts are
 * uPlot canvases coloured with a CVD-validated categorical palette
 * anchored on the LuminalShine sun-gold brand hue (see PAL below).
 *
 * Distinct from the source mockup:
 *   - Header is compact and sticky (not a 12-cell metadata grid).
 *   - Charts are split across three tabs (Stream / Connection /
 *     Host) instead of one tall scroll-stack of six, so the
 *     dominant chart for the current concern fills the viewport
 *     instead of every metric competing for attention at once.
 *   - "Disconnect Session" lives in the drawer header (top-right)
 *     as a prominent rose action; it's only visible while the
 *     session is active. Export / Delete are footer actions.
 *
 * Polls `GET /api/sessions/<id>` every 2 seconds while the panel
 * is open for an active session; ended sessions load once and
 * stay static.
 */
import { computed, onBeforeUnmount, onMounted, ref, watch, nextTick } from 'vue';
import { NDrawer, NDrawerContent, NButton, NTabs, NTabPane, NTag, useDialog, useMessage } from 'naive-ui';
import uPlot, { type Options as UPlotOptions, type AlignedData } from 'uplot';
import 'uplot/dist/uPlot.min.css';
import { http } from '@/http';

type Series = number[];
interface SessionPayload {
  id: string;
  started_at: number;
  stream_ended_at: number | null;
  metadata: {
    client_name?: string;
    device?: string;
    protocol?: string;
    codec?: string;
    resolution_w?: number;
    resolution_h?: number;
    fps?: number;
    bitrate_mbps_target?: number;
    hdr?: boolean;
    // Boolean at session start; may become the string "false" after a
    // metadata_update patch (the patch channel only carries strings).
    yuv444?: boolean | string;
    audio_channels?: number;
    application?: string;
    cpu_model?: string;
    gpu_model?: string;
    luminalshine_version?: string;
  };
  series: Record<string, Array<[number, number]>>;
}

const props = defineProps<{ show: boolean; sessionId: string | null }>();
const emit = defineEmits<{
  (e: 'update:show', v: boolean): void;
  (e: 'session-deleted', id: string): void;
}>();

const dialog = useDialog();
const message = useMessage();

const session = ref<SessionPayload | null>(null);
const loading = ref(false);
const errorText = ref('');
let pollTimer: ReturnType<typeof setTimeout> | null = null;

const isActive = computed(() => session.value?.stream_ended_at == null);

const headerLine = computed(() => {
  const m = session.value?.metadata ?? {};
  const app = m.application?.trim() || 'Streaming';
  const client = m.client_name?.trim() || 'unknown client';
  return `${app} · ${client}`;
});

const chips = computed(() => {
  const m = session.value?.metadata ?? {};
  const out: { label: string; tone: 'info' | 'primary' | 'success' | 'warning' | 'default' }[] = [];
  if (m.protocol) out.push({ label: m.protocol, tone: 'info' });
  if (m.codec) out.push({ label: m.codec, tone: 'primary' });
  if (m.resolution_w && m.resolution_h) {
    const fps = m.fps ? `@${m.fps}` : '';
    out.push({ label: `${m.resolution_w}×${m.resolution_h}${fps}`, tone: 'default' });
  }
  if (m.hdr) out.push({ label: 'HDR', tone: 'warning' });
  if (m.yuv444 === true || m.yuv444 === 'true') out.push({ label: 'YUV444', tone: 'success' });
  if (m.audio_channels) out.push({ label: `${m.audio_channels}ch`, tone: 'default' });
  return out;
});

const durationText = computed(() => {
  if (!session.value) return '';
  const end = session.value.stream_ended_at ?? Math.floor(Date.now() / 1000);
  const start = session.value.started_at;
  const secs = Math.max(0, end - start);
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  const s = secs % 60;
  return h > 0 ? `${h}h ${m}m ${s}s` : m > 0 ? `${m}m ${s}s` : `${s}s`;
});

const avgBitrate = computed(() => avgOf('network_throughput_mbps'));
const avgFps = computed(() => avgOf('actual_fps'));

function avgOf(seriesName: string): number | null {
  const pts = session.value?.series?.[seriesName];
  if (!pts || pts.length === 0) return null;
  let sum = 0;
  for (const [, v] of pts) sum += v;
  return sum / pts.length;
}

async function loadSession() {
  if (!props.sessionId) return;
  loading.value = true;
  errorText.value = '';
  try {
    const r = await http.get(`./api/sessions/${encodeURIComponent(props.sessionId)}`, {
      validateStatus: () => true,
    });
    if (r.status === 503) {
      errorText.value = 'Session monitor service is offline.';
      session.value = null;
    } else if (r.status >= 200 && r.status < 300) {
      session.value = r.data as SessionPayload;
    } else {
      errorText.value = `HTTP ${r.status}`;
      session.value = null;
    }
  } catch (e) {
    errorText.value = e instanceof Error ? e.message : 'Request failed';
    session.value = null;
  } finally {
    loading.value = false;
    if (session.value) {
      await nextTick();
      redrawAllCharts();
    }
  }
}

function stopPolling() {
  if (pollTimer != null) {
    clearTimeout(pollTimer);
    pollTimer = null;
  }
}

function startPolling() {
  stopPolling();
  if (!props.show || !props.sessionId) return;
  const tick = async () => {
    await loadSession();
    if (props.show && props.sessionId && isActive.value) {
      pollTimer = setTimeout(tick, 2000);
    }
  };
  pollTimer = setTimeout(tick, 0);
}

watch(
  () => [props.show, props.sessionId],
  () => {
    if (props.show && props.sessionId) {
      startPolling();
    } else {
      stopPolling();
      session.value = null;
      destroyAllCharts();
    }
  },
);

onBeforeUnmount(() => {
  stopPolling();
  destroyAllCharts();
});

// ----------------------------------------------------------- uPlot bridge
//
// Categorical chart palette — validated (lightness band, chroma floor,
// CVD separation, contrast) against the warm near-black glass surface.
// Fixed hue order; the gold slot is the sunshine-brand anchor. Colors
// follow the entity: CPU/latency/IDR wear gold, GPU/FPS/ref-inv wear
// blue, losses/RAM wear flare, throughput/VRAM wear teal, encoder
// wears violet — consistent everywhere a metric appears.
const PAL = {
  gold: '#c68010',
  blue: '#3d8fd6',
  flare: '#e05537',
  teal: '#2fa89a',
  violet: '#9a6ae0',
} as const;

const charts: Map<string, uPlot> = new Map();
const chartRefs: Record<string, HTMLDivElement | null> = {};

function setChartRef(id: string, el: HTMLDivElement | null) {
  // NaiveUI's <NTabs> lazy-mounts inactive panes, so the chart hosts
  // for the Connection and Host tabs don't exist when loadSession()
  // first runs redrawAllCharts() — those redraw calls bail because
  // chartRefs[id] is null. When the user later switches tabs the
  // host <div> mounts (this callback fires with a non-null el); the
  // poll loop only re-fires for live sessions, so without this hook
  // ended sessions would never paint anything on the freshly-mounted
  // tab. Conversely on unmount (el == null) we tear down the uPlot
  // instance so a subsequent re-mount creates a fresh one against
  // the new DOM node instead of trying to size a chart whose canvas
  // parent has been detached.
  if (el == null) {
    const existing = charts.get(id);
    if (existing) {
      existing.destroy();
      charts.delete(id);
    }
    chartRefs[id] = null;
    return;
  }
  chartRefs[id] = el;
  if (session.value) {
    const spec = [...STREAM_CHARTS, ...CONNECTION_CHARTS, ...HOST_CHARTS].find((s) => s.id === id);
    if (spec) {
      void nextTick(() => redrawChart(spec));
    }
  }
}

function destroyAllCharts() {
  for (const c of charts.values()) c.destroy();
  charts.clear();
}

interface SeriesSpec {
  key: string;        // key in session.series map
  label: string;
  stroke: string;
  fill?: string;
}

interface ChartSpec {
  id: string;
  title: string;
  yLabel: string;
  series: SeriesSpec[];
  yScaleMin?: number;
  yScaleMax?: number;
}

// Three tabs of chart specs. The grouping is the main differentiator
// from the source mockup: instead of six tall charts stacked, the
// user picks the concern they care about (Stream / Connection / Host)
// and sees only the relevant data.
const STREAM_CHARTS: ChartSpec[] = [
  { id: 'encode_latency', title: 'Encode Latency', yLabel: 'ms',
    series: [{ key: 'encode_latency_ms', label: 'Encode (ms)', stroke: PAL.gold }] },
  { id: 'actual_fps', title: 'Actual Frame Rate', yLabel: 'fps',
    series: [{ key: 'actual_fps', label: 'FPS', stroke: PAL.blue }] },
  { id: 'network_throughput', title: 'Network Throughput', yLabel: 'Mbps',
    series: [{ key: 'network_throughput_mbps', label: 'Mbps', stroke: PAL.teal }] },
];

const CONNECTION_CHARTS: ChartSpec[] = [
  { id: 'connection_quality', title: 'Connection Quality Events', yLabel: 'count/s',
    series: [
      { key: 'client_losses',      label: 'Losses',     stroke: PAL.flare },
      { key: 'idr_requests',       label: 'IDR',        stroke: PAL.gold },
      { key: 'ref_invalidations',  label: 'Ref Inv.',   stroke: PAL.blue },
    ] },
];

const HOST_CHARTS: ChartSpec[] = [
  { id: 'host_compute', title: 'Host CPU / GPU', yLabel: '%',
    yScaleMin: 0, yScaleMax: 100,
    series: [
      { key: 'host_cpu_pct',          label: 'CPU',     stroke: PAL.gold },
      { key: 'host_gpu_pct',          label: 'GPU',     stroke: PAL.blue },
      { key: 'host_gpu_encoder_pct',  label: 'Encoder', stroke: PAL.violet },
    ] },
  { id: 'host_memory', title: 'Host RAM / VRAM', yLabel: '%',
    yScaleMin: 0, yScaleMax: 100,
    series: [
      { key: 'host_ram_pct',   label: 'RAM',  stroke: PAL.flare },
      { key: 'host_vram_pct',  label: 'VRAM', stroke: PAL.teal },
    ] },
];

/**
 * True once any of the chart's series has at least one recorded point.
 * Used to swap the uPlot host for a "waiting for telemetry" placeholder
 * so a chart whose producer isn't emitting yet doesn't render as a
 * confusing empty axis box.
 */
function specHasData(spec: ChartSpec): boolean {
  return spec.series.some((s) => (session.value?.series?.[s.key]?.length ?? 0) > 0);
}

function buildAlignedData(spec: ChartSpec): AlignedData {
  // uPlot wants column-major data: x-axis array followed by one
  // y-axis array per series. We union the timestamps across all
  // series in the chart so the axis is continuous even when one
  // metric started later than another.
  const tsSet = new Set<number>();
  for (const s of spec.series) {
    const arr = session.value?.series?.[s.key] ?? [];
    for (const [ts] of arr) tsSet.add(ts);
  }
  const xs = Array.from(tsSet).sort((a, b) => a - b);
  const cols: Series[] = [xs];
  for (const s of spec.series) {
    const map = new Map<number, number>();
    const arr = session.value?.series?.[s.key] ?? [];
    for (const [ts, v] of arr) map.set(ts, v);
    cols.push(xs.map((t) => map.get(t) ?? NaN));
  }
  return cols as unknown as AlignedData;
}

function buildOptions(spec: ChartSpec, width: number, height: number): UPlotOptions {
  // LuminalShine sunshine-glass chart styling:
  //   - Transparent canvas (the surrounding glass card carries the
  //     background) — uPlot's default is opaque white.
  //   - Subtle axis ticks/labels in muted warm text.
  //   - Recessive grid on y only — keep the chart-card surface clean.
  //   - Series strokes use the validated categorical palette per spec.
  return {
    width,
    height,
    legend: { show: true, live: true },
    cursor: { drag: { x: true, y: false, uni: 8 } },
    series: [
      { label: 'time' },
      ...spec.series.map((s) => ({
        label: s.label,
        stroke: s.stroke,
        width: 2,
        points: { show: false },
      })),
    ],
    axes: [
      {
        stroke: 'rgba(240,231,218,0.55)',
        grid: { show: false },
        ticks: { stroke: 'rgba(240,231,218,0.12)', size: 4 },
      },
      {
        stroke: 'rgba(240,231,218,0.55)',
        label: spec.yLabel,
        labelSize: 18,
        grid: { stroke: 'rgba(240,231,218,0.06)', width: 1 },
        ticks: { stroke: 'rgba(240,231,218,0.12)', size: 4 },
      },
    ],
    scales: {
      x: { time: true },
      y: spec.yScaleMin != null && spec.yScaleMax != null
        ? { range: [spec.yScaleMin, spec.yScaleMax] as [number, number] }
        : { auto: true },
    },
  } as UPlotOptions;
}

function redrawChart(spec: ChartSpec) {
  const el = chartRefs[spec.id];
  if (!el || !session.value) return;
  const rect = el.getBoundingClientRect();
  const width = Math.max(240, Math.floor(rect.width));
  const height = 200;
  const data = buildAlignedData(spec);
  let chart = charts.get(spec.id);
  if (!chart) {
    chart = new uPlot(buildOptions(spec, width, height), data, el);
    charts.set(spec.id, chart);
  } else {
    chart.setSize({ width, height });
    chart.setData(data);
  }
}

function redrawAllCharts() {
  for (const spec of [...STREAM_CHARTS, ...CONNECTION_CHARTS, ...HOST_CHARTS]) {
    redrawChart(spec);
  }
}

window.addEventListener('resize', redrawAllCharts);
onBeforeUnmount(() => window.removeEventListener('resize', redrawAllCharts));

// --------------------------------------------------------------- actions
async function disconnectSession() {
  if (!session.value || !isActive.value) return;
  dialog.warning({
    title: 'Disconnect this streaming session?',
    content:
      'The connected Moonlight client will be dropped immediately. The session ' +
      'history and recorded telemetry remain in the dashboard.',
    positiveText: 'Disconnect',
    negativeText: 'Cancel',
    onPositiveClick: async () => {
      const m = session.value!.metadata ?? {};
      const clientUuid = (m.device || m.client_name || '').trim();
      try {
        const r = await http.post(
          './api/clients/disconnect',
          { uuid: clientUuid },
          { validateStatus: () => true },
        );
        if (r.status >= 200 && r.status < 300) {
          message.success('Disconnect request sent.');
        } else {
          message.error(`Disconnect failed (HTTP ${r.status}).`);
        }
      } catch (e) {
        message.error(e instanceof Error ? e.message : 'Disconnect failed.');
      }
    },
  });
}

function exportSession() {
  if (!props.sessionId) return;
  // The proxy endpoint sets Content-Disposition: attachment, so a
  // plain link navigate triggers the browser's download flow.
  window.location.href = `./api/sessions/${encodeURIComponent(props.sessionId)}/export.json`;
}

async function deleteSession() {
  if (!props.sessionId) return;
  dialog.warning({
    title: 'Delete this session from the dashboard?',
    content:
      'The recorded telemetry will be removed from %ProgramData%\\LuminalShine\\' +
      'sessions\\ and the entry will disappear from the Session History list. ' +
      'Only ended sessions can be deleted.',
    positiveText: 'Delete',
    negativeText: 'Cancel',
    onPositiveClick: async () => {
      const id = props.sessionId!;
      try {
        const r = await http.delete(`./api/sessions/${encodeURIComponent(id)}`, {
          validateStatus: () => true,
        });
        if (r.status >= 200 && r.status < 300) {
          message.success('Session deleted.');
          emit('session-deleted', id);
          emit('update:show', false);
        } else if (r.status === 400) {
          message.warning('Cannot delete an active session. Disconnect it first.');
        } else {
          message.error(`Delete failed (HTTP ${r.status}).`);
        }
      } catch (e) {
        message.error(e instanceof Error ? e.message : 'Delete failed.');
      }
    },
  });
}

function close() {
  emit('update:show', false);
}
</script>

<template>
  <NDrawer
    :show="show"
    :width="640"
    placement="right"
    :auto-focus="false"
    :close-on-esc="true"
    :mask-closable="true"
    @update:show="(v: boolean) => $emit('update:show', v)"
  >
    <NDrawerContent :native-scrollbar="false" :closable="false" class="session-details-drawer">
      <!-- Sticky compact header -->
      <header class="sticky top-0 z-10 -mx-6 px-6 py-4 backdrop-blur-md
                     border-b border-light/10 dark:border-dark/40
                     bg-light/70 dark:bg-surface/85">
        <div class="flex items-start gap-3">
          <!-- LIVE pulse / ENDED badge -->
          <div class="flex items-center gap-2 shrink-0 mt-1">
            <span
              class="inline-flex items-center gap-1.5 px-2 py-0.5 rounded-full text-[10px]
                     font-semibold uppercase tracking-wide"
              :class="isActive
                ? 'bg-success/15 text-success border border-success/30'
                : 'bg-light/10 text-light/70 border border-light/15'"
            >
              <span
                v-if="isActive"
                class="w-1.5 h-1.5 rounded-full bg-success animate-pulse"
                aria-hidden
              />
              {{ isActive ? 'Live' : 'Ended' }}
            </span>
          </div>

          <!-- Identity + chips -->
          <div class="flex-1 min-w-0">
            <h2 class="text-base font-semibold truncate text-dark dark:text-light">
              {{ headerLine }}
            </h2>
            <div class="flex flex-wrap gap-1.5 mt-1.5">
              <NTag
                v-for="(c, i) in chips"
                :key="i"
                size="small"
                :type="c.tone === 'default' ? 'default' : c.tone"
                :bordered="false"
              >{{ c.label }}</NTag>
              <span class="text-xs opacity-60 self-center ml-1">{{ durationText }}</span>
            </div>
          </div>

          <!-- Top-right actions -->
          <div class="flex items-center gap-2 shrink-0">
            <NButton
              v-if="isActive"
              type="error"
              size="small"
              strong
              @click="disconnectSession"
            >
              Disconnect
            </NButton>
            <NButton size="small" tertiary circle @click="close" aria-label="Close">
              ×
            </NButton>
          </div>
        </div>

        <!-- Key metrics row -->
        <div class="grid grid-cols-3 gap-3 mt-3">
          <div class="rounded-lg px-3 py-2 bg-light/5 dark:bg-dark/30 border border-light/10">
            <div class="text-[10px] uppercase tracking-wider opacity-60">Avg Bitrate</div>
            <div class="text-lg font-mono font-semibold opacity-90">
              {{ avgBitrate != null ? avgBitrate.toFixed(0) + ' Mbps' : '—' }}
            </div>
          </div>
          <div class="rounded-lg px-3 py-2 bg-light/5 dark:bg-dark/30 border border-light/10">
            <div class="text-[10px] uppercase tracking-wider opacity-60">Avg FPS</div>
            <div class="text-lg font-mono font-semibold opacity-90">
              {{ avgFps != null ? avgFps.toFixed(0) : '—' }}
            </div>
          </div>
          <div class="rounded-lg px-3 py-2 bg-light/5 dark:bg-dark/30 border border-light/10">
            <div class="text-[10px] uppercase tracking-wider opacity-60">Target</div>
            <div class="text-lg font-mono font-semibold opacity-90">
              {{ session?.metadata.bitrate_mbps_target?.toFixed(0) ?? '—' }} Mbps
            </div>
          </div>
        </div>
      </header>

      <!-- Body -->
      <div v-if="loading && !session" class="py-8 text-center opacity-60">Loading…</div>
      <div v-else-if="errorText" class="py-8 text-center text-danger">{{ errorText }}</div>
      <NTabs v-else-if="session" type="line" animated class="mt-2">
        <NTabPane name="stream" tab="Stream">
          <div class="space-y-4">
            <div v-for="spec in STREAM_CHARTS" :key="spec.id" class="chart-card">
              <h3 class="chart-title">{{ spec.title }}</h3>
              <div
                v-if="specHasData(spec)"
                :ref="(el) => setChartRef(spec.id, el as HTMLDivElement)"
                class="chart-host"
              />
              <div v-else class="chart-empty">
                {{ isActive ? 'Waiting for telemetry…' : 'No data recorded for this session.' }}
              </div>
            </div>
          </div>
        </NTabPane>
        <NTabPane name="connection" tab="Connection">
          <div class="space-y-4">
            <div v-for="spec in CONNECTION_CHARTS" :key="spec.id" class="chart-card">
              <h3 class="chart-title">{{ spec.title }}</h3>
              <div
                v-if="specHasData(spec)"
                :ref="(el) => setChartRef(spec.id, el as HTMLDivElement)"
                class="chart-host"
              />
              <div v-else class="chart-empty">
                {{ isActive ? 'Waiting for telemetry…' : 'No data recorded for this session.' }}
              </div>
            </div>
          </div>
        </NTabPane>
        <NTabPane name="host" tab="Host">
          <div class="space-y-4">
            <div v-for="spec in HOST_CHARTS" :key="spec.id" class="chart-card">
              <h3 class="chart-title">{{ spec.title }}</h3>
              <div
                v-if="specHasData(spec)"
                :ref="(el) => setChartRef(spec.id, el as HTMLDivElement)"
                class="chart-host"
              />
              <div v-else class="chart-empty">
                {{ isActive ? 'Waiting for telemetry…' : 'No data recorded for this session.' }}
              </div>
            </div>
            <!-- Host hardware footnote -->
            <div class="rounded-lg p-3 bg-light/5 dark:bg-dark/30 border border-light/10 text-xs space-y-1 opacity-80">
              <div><span class="opacity-60">CPU:</span> {{ session.metadata.cpu_model || '—' }}</div>
              <div><span class="opacity-60">GPU:</span> {{ session.metadata.gpu_model || '—' }}</div>
              <div><span class="opacity-60">LuminalShine:</span> {{ session.metadata.luminalshine_version || '—' }}</div>
            </div>
          </div>
        </NTabPane>
      </NTabs>

      <!-- Footer actions -->
      <footer class="sticky bottom-0 z-10 -mx-6 px-6 py-3 mt-4 backdrop-blur-md
                     border-t border-light/10 dark:border-dark/40
                     bg-light/70 dark:bg-surface/85 flex justify-end gap-2">
        <NButton size="small" tertiary @click="exportSession" :disabled="!session">
          Export JSON
        </NButton>
        <NButton size="small" type="error" ghost @click="deleteSession" :disabled="!session || isActive">
          Delete
        </NButton>
      </footer>
    </NDrawerContent>
  </NDrawer>
</template>

<style scoped>
.session-details-drawer :deep(.n-drawer-body-content-wrapper) {
  /* Sunshine glass body so the drawer sits on the same warm near-black
     gradient the rest of LuminalShine uses. The transparent uPlot
     canvases inherit this background which is why they don't paint
     a white box behind the chart. */
  background:
    radial-gradient(1100px 700px at 100% -10%, rgba(255, 176, 32, 0.06), transparent 60%),
    radial-gradient(900px 500px at -10% 110%, rgba(255, 61, 0, 0.05), transparent 60%),
    rgba(17, 15, 12, 0.92);
}

.chart-card {
  border-radius: 18px;
  padding: 12px 16px 14px;
  background: rgba(46, 34, 22, 0.35);
  border: 1px solid rgba(255, 255, 255, 0.08);
  backdrop-filter: blur(8px);
}

.chart-title {
  font-size: 0.78rem;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: rgba(240, 231, 218, 0.7);
  margin: 0 0 6px;
  font-weight: 600;
}

.chart-host {
  width: 100%;
  min-height: 200px;
}

.chart-empty {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 200px;
  font-size: 0.8rem;
  color: rgba(240, 231, 218, 0.45);
  border: 1px dashed rgba(255, 255, 255, 0.1);
  border-radius: 12px;
}

/* uPlot legend re-skin: live values get rendered into a tiny strip
   below the chart by default — recolour them so they read on the
   glass surface. */
:deep(.u-legend) {
  color: rgba(240, 231, 218, 0.72);
  font-size: 11px;
}
:deep(.u-legend .u-marker) {
  border-radius: 2px;
}
</style>
