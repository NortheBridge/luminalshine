<script setup lang="ts">
/**
 * Session History card — renders the list of recorded streaming
 * sessions on the Dashboard. Each row is a click-target that opens
 * the SessionDetailsPanel slide-out. Active sessions sit at the top
 * with a pulsing "Live" indicator; ended sessions are listed below
 * in reverse-chronological order.
 *
 * Data source: GET /api/sessions (the main service's proxy to the
 * LuminalShineSessionMonitor sidecar). On a 503 the card collapses
 * to a one-line "Session Monitor offline" hint instead of an
 * error — the rest of the dashboard keeps working without the
 * sidecar.
 */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { NCard, NEmpty, NSpin } from 'naive-ui';
import SessionDetailsPanel from './SessionDetailsPanel.vue';
import { http } from '@/http';

interface Summary {
  id: string;
  started_at: number;
  stream_ended_at: number | null;
  metadata: {
    client_name?: string;
    codec?: string;
    application?: string;
    resolution_w?: number;
    resolution_h?: number;
    fps?: number;
  };
}

const sessions = ref<Summary[]>([]);
const loading = ref(false);
const monitorOffline = ref(false);

const panelShow = ref(false);
const panelSessionId = ref<string | null>(null);

let pollTimer: ReturnType<typeof setTimeout> | null = null;

async function refresh() {
  loading.value = true;
  try {
    const r = await http.get('./api/sessions', { validateStatus: () => true });
    if (r.status === 503) {
      monitorOffline.value = true;
      sessions.value = [];
    } else if (r.status >= 200 && r.status < 300) {
      monitorOffline.value = false;
      const list = Array.isArray(r.data) ? (r.data as Summary[]) : [];
      list.sort((a, b) => {
        // Active first, then most-recent-start first.
        const aActive = a.stream_ended_at == null ? 1 : 0;
        const bActive = b.stream_ended_at == null ? 1 : 0;
        if (aActive !== bActive) return bActive - aActive;
        return (b.started_at ?? 0) - (a.started_at ?? 0);
      });
      sessions.value = list;
    } else {
      monitorOffline.value = false;
      sessions.value = [];
    }
  } catch {
    monitorOffline.value = true;
    sessions.value = [];
  } finally {
    loading.value = false;
  }
}

function startPolling() {
  refresh();
  pollTimer = setInterval(() => {
    refresh();
  }, 5000);
}
function stopPolling() {
  if (pollTimer != null) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

onMounted(startPolling);
onBeforeUnmount(stopPolling);

function openSession(id: string) {
  panelSessionId.value = id;
  panelShow.value = true;
}

function onPanelDeleted(id: string) {
  sessions.value = sessions.value.filter((s) => s.id !== id);
}

function fmtDate(epoch: number): string {
  return new Date(epoch * 1000).toLocaleString();
}

function fmtDuration(s: Summary): string {
  const end = s.stream_ended_at ?? Math.floor(Date.now() / 1000);
  const secs = Math.max(0, end - s.started_at);
  const h = Math.floor(secs / 3600);
  const m = Math.floor((secs % 3600) / 60);
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${secs % 60}s`;
}

function rowLabel(s: Summary): string {
  const m = s.metadata ?? {};
  const app = m.application?.trim() || 'Streaming';
  const client = m.client_name?.trim() || 'unknown';
  return `${app} · ${client}`;
}

function rowSubLabel(s: Summary): string {
  const m = s.metadata ?? {};
  const parts: string[] = [];
  if (m.codec) parts.push(m.codec);
  if (m.resolution_w && m.resolution_h) {
    parts.push(`${m.resolution_w}×${m.resolution_h}` + (m.fps ? `@${m.fps}` : ''));
  }
  parts.push(fmtDuration(s));
  parts.push(fmtDate(s.started_at));
  return parts.join(' · ');
}
</script>

<template>
  <NCard
    title="Session History"
    :segmented="{ content: true }"
    class="session-history-card"
  >
    <template #header-extra>
      <span v-if="loading" class="text-xs opacity-60">refreshing…</span>
    </template>

    <div v-if="monitorOffline" class="py-3 text-sm opacity-70 text-center">
      Session Monitor service is offline. Stream telemetry won't appear here until
      <code class="opacity-90">LuminalShineSessionMonitor</code> is started.
    </div>
    <NEmpty
      v-else-if="!loading && sessions.length === 0"
      description="No sessions recorded yet. Start a Moonlight stream to populate this list."
      class="py-4"
    />
    <NSpin v-else-if="loading && sessions.length === 0" />
    <ul v-else class="divide-y divide-light/5 dark:divide-dark/30">
      <li
        v-for="s in sessions"
        :key="s.id"
        class="py-2.5 flex items-center gap-3 hover:bg-light/5 dark:hover:bg-dark/30
               rounded-md px-2 -mx-2 cursor-pointer transition-colors"
        @click="openSession(s.id)"
      >
        <span
          class="inline-flex items-center gap-1.5 px-2 py-0.5 rounded-full text-[10px]
                 font-semibold uppercase tracking-wide shrink-0"
          :class="s.stream_ended_at == null
            ? 'bg-success/15 text-success border border-success/30'
            : 'bg-light/10 text-light/65 border border-light/15'"
        >
          <span
            v-if="s.stream_ended_at == null"
            class="w-1.5 h-1.5 rounded-full bg-success animate-pulse"
            aria-hidden
          />
          {{ s.stream_ended_at == null ? 'Live' : 'Ended' }}
        </span>
        <div class="min-w-0 flex-1">
          <div class="text-sm font-medium truncate">{{ rowLabel(s) }}</div>
          <div class="text-xs opacity-65 truncate">{{ rowSubLabel(s) }}</div>
        </div>
        <span class="text-xs opacity-40 shrink-0">›</span>
      </li>
    </ul>

    <SessionDetailsPanel
      v-model:show="panelShow"
      :session-id="panelSessionId"
      @session-deleted="onPanelDeleted"
    />
  </NCard>
</template>

<style scoped>
.session-history-card :deep(.n-card-header) {
  padding-bottom: 8px;
}
</style>
