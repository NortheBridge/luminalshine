import { computed, onBeforeUnmount, ref, watch, type Ref } from 'vue';
import { http } from '@/http';

/**
 * One session's telemetry from the session monitor: fetch, range window,
 * 2 s polling while the session is live, and the derived numbers the
 * Stream page shows (health, averages, last values). Pure data — the
 * charts render whatever `points()` returns.
 */

export interface SessionMetadata {
  client_name?: string;
  device?: string;
  client_uuid?: string;
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
}

export interface SessionPayload {
  id: string;
  started_at: number;
  stream_ended_at: number | null;
  metadata: SessionMetadata;
  series: Record<string, Array<[number, number]>>;
}

export type RangeKey = 'full' | '3m' | '5m' | '15m';

export const RANGES: ReadonlyArray<{ key: RangeKey; seconds: number | null }> = [
  { key: 'full', seconds: null },
  { key: '3m', seconds: 180 },
  { key: '5m', seconds: 300 },
  { key: '15m', seconds: 900 },
];

export type StreamHealth = 'healthy' | 'poor' | null;

const POLL_MS = 2000;

export function useSessionDetail(sessionId: Ref<string | null>, range: Ref<RangeKey>) {
  const session = ref<SessionPayload | null>(null);
  const loading = ref(false);
  const errorText = ref('');
  const monitorOffline = ref(false);
  let pollTimer: ReturnType<typeof setTimeout> | null = null;
  let generation = 0;

  const isActive = computed(() => !!session.value && session.value.stream_ended_at == null);

  // 'full' serves the whole ring, decimated server-side to <= ~900 mean
  // buckets; the minute windows fetch the trailing window at full 1 Hz
  // fidelity (anchored to the end timestamp for ended sessions).
  function rangeQuery(): string {
    const spec = RANGES.find((r) => r.key === range.value);
    if (!spec) return '';
    const end = session.value?.stream_ended_at ?? Math.floor(Date.now() / 1000);
    if (spec.seconds == null) {
      const s = session.value;
      if (!s) return ''; // first load: full fidelity; the poll refines with a step
      const dur = Math.max(1, end - s.started_at);
      const step = Math.max(1, Math.ceil(dur / 900));
      return step > 1 ? `?step=${step}` : '';
    }
    const from = Math.max(0, end - spec.seconds);
    return `?from=${from}&to=${end}`;
  }

  async function load(): Promise<void> {
    const id = sessionId.value;
    if (!id) return;
    const gen = generation;
    loading.value = true;
    try {
      const r = await http.get(`./api/sessions/${encodeURIComponent(id)}${rangeQuery()}`, {
        validateStatus: () => true,
      });
      if (gen !== generation) return; // a newer selection superseded this request
      if (r.status === 503) {
        monitorOffline.value = true;
        errorText.value = '';
        session.value = null;
      } else if (r.status >= 200 && r.status < 300) {
        monitorOffline.value = false;
        errorText.value = '';
        session.value = r.data as SessionPayload;
      } else {
        monitorOffline.value = false;
        errorText.value = `HTTP ${r.status}`;
        session.value = null;
      }
    } catch (e) {
      if (gen !== generation) return;
      errorText.value = e instanceof Error ? e.message : 'Request failed';
      session.value = null;
    } finally {
      if (gen === generation) loading.value = false;
    }
  }

  function stopPolling(): void {
    if (pollTimer != null) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function startPolling(): void {
    stopPolling();
    if (!sessionId.value) return;
    const gen = generation;
    const tick = async () => {
      await load();
      if (gen === generation && sessionId.value && isActive.value) {
        pollTimer = setTimeout(() => void tick(), POLL_MS);
      }
    };
    pollTimer = setTimeout(() => void tick(), 0);
  }

  watch(
    () => [sessionId.value, range.value] as const,
    ([id], old) => {
      generation += 1;
      if (!old || old[0] !== id) {
        session.value = null;
        errorText.value = '';
      }
      startPolling();
    },
    { immediate: true },
  );

  onBeforeUnmount(stopPolling);

  // ---- derived -------------------------------------------------------------
  function points(key: string): Array<[number, number]> {
    return session.value?.series?.[key] ?? [];
  }

  function last(key: string): number | null {
    const pts = points(key);
    const v = pts[pts.length - 1]?.[1];
    return typeof v === 'number' && Number.isFinite(v) ? v : null;
  }

  function avg(key: string): number | null {
    const pts = points(key);
    if (pts.length === 0) return null;
    let sum = 0;
    for (const [, v] of pts) sum += v;
    return sum / pts.length;
  }

  function sum(key: string): number | null {
    const pts = points(key);
    if (pts.length === 0) return null;
    let total = 0;
    for (const [, v] of pts) total += v;
    return total;
  }

  function peak(key: string): number | null {
    const pts = points(key);
    if (pts.length === 0) return null;
    let max = -Infinity;
    for (const [, v] of pts) if (v > max) max = v;
    return Number.isFinite(max) ? max : null;
  }

  // Health over the trailing minute of connection events: sustained client
  // loss (> 1/s average) or heavy IDR recovery (> 0.2/s) reads as a
  // struggling stream. Null until any connection series has data.
  const health = computed<StreamHealth>(() => {
    const s = session.value;
    if (!s) return null;
    const rate = (key: string): number | null => {
      const pts = s.series?.[key];
      if (!pts || pts.length === 0) return null;
      const tail = pts.slice(-60);
      let total = 0;
      for (const [, v] of tail) total += v;
      return total / tail.length;
    };
    const losses = rate('client_losses');
    const idr = rate('idr_requests');
    if (losses == null && idr == null) return null;
    return (losses ?? 0) > 1 || (idr ?? 0) > 0.2 ? 'poor' : 'healthy';
  });

  return {
    session,
    loading,
    errorText,
    monitorOffline,
    isActive,
    health,
    points,
    last,
    avg,
    sum,
    peak,
    reload: load,
  };
}
