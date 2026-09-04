import { defineStore } from 'pinia';
import { computed, ref } from 'vue';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import type { CrashDumpStatus } from '@/utils/crashDump';
import { isCrashDumpEligible, sanitizeCrashDumpStatus } from '@/utils/crashDump';

/**
 * Host status store — the single status model behind the Mission Control
 * shell. The host strip, the Overview and any page that needs "what is the
 * host doing right now" read from here instead of polling on their own.
 *
 * Three cadences:
 *  - fast   (5 s):  /api/sessions          — live session + monitor reachability
 *  - medium (20 s): /api/clients/list      — paired / connected clients
 *  - slow   (60 s): health probes          — TDR, crash dump, ViGEm, golden
 *                                            snapshot, Playnite extension
 * The startup log tail is fetched once (it is the whole log file) and again
 * only on demand.
 *
 * A stats-only session may call exactly /api/sessions, /api/metadata and
 * /api/auth/*, so everything else is skipped for that role rather than
 * producing a wall of 403s.
 */

export interface SessionSummary {
  id: string;
  started_at: number; // epoch seconds
  stream_ended_at: number | null; // null ⇒ live
  metadata: {
    client_name?: string;
    codec?: string;
    application?: string;
    resolution_w?: number;
    resolution_h?: number;
    fps?: number;
  };
}

interface ClientApiEntry {
  uuid?: unknown;
  name?: unknown;
  connected?: unknown;
  last_seen?: unknown;
}

export interface ClientSummary {
  uuid: string;
  name: string;
  connected: boolean;
  lastSeen: number | null; // epoch seconds
}

export interface TdrStatus {
  count?: number;
  recovery_recent?: boolean;
  stack_down?: boolean;
  incident?: {
    started_at?: number;
    last_at?: number;
    duration_seconds?: number;
    events?: number;
  } | null;
}

export interface PlayniteStatus {
  installed: boolean | null;
  active: boolean;
  extensions_dir?: string;
  installed_version?: string;
  packaged_version?: string;
  update_available?: boolean;
}

export interface GoldenStatus {
  exists?: boolean;
  snapshot_version?: number | null;
  latest_snapshot_version?: number;
  has_layout?: boolean;
  needs_layout_upgrade?: boolean;
}

export interface LogLine {
  timestamp: string; // "[YYYY-MM-DD HH:MM:SS.mmm]"
  time: string; // "HH:MM:SS"
  level: string; // Info | Warning | Error | Fatal | Debug | Verbose
  message: string;
}

export type HealthState = 'ok' | 'warn' | 'danger' | 'unknown';

export interface HealthCheck {
  id: string;
  label: string;
  state: HealthState;
  detail: string;
}

const FAST_MS = 5000;
const MEDIUM_MS = 20000;
const SLOW_MS = 60000;

function toEpochSeconds(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) {
    // Accept milliseconds too.
    return value > 1e12 ? Math.floor(value / 1000) : Math.floor(value);
  }
  if (typeof value === 'string' && value.trim()) {
    const n = Number(value);
    if (Number.isFinite(n)) return toEpochSeconds(n);
    const d = Date.parse(value);
    if (Number.isFinite(d)) return Math.floor(d / 1000);
  }
  return null;
}

export function parseLogText(text: string): LogLine[] {
  if (!text) return [];
  const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g;
  const raw = text.split(regex).splice(1);
  const out: LogLine[] = [];
  for (let i = 0; i + 1 < raw.length; i += 2) {
    const stamp = raw[i] ?? '';
    const body = (raw[i + 1] ?? '').replace(/\s+$/, '');
    const colon = body.indexOf(':');
    const level = colon > 0 ? body.slice(0, colon).trim() : '';
    const message = colon > 0 ? body.slice(colon + 1).trim() : body;
    out.push({
      timestamp: stamp,
      time: stamp.slice(12, 20),
      level,
      message,
    });
  }
  return out;
}

export const useHostStore = defineStore('host', () => {
  const auth = useAuthStore();
  const configStore = useConfigStore();

  // ---- state ----------------------------------------------------------
  const sessions = ref<SessionSummary[]>([]);
  const monitorOffline = ref(false);
  const sessionsLoaded = ref(false);

  const clients = ref<ClientSummary[]>([]);
  const clientsLoaded = ref(false);

  const tdr = ref<TdrStatus | null>(null);
  const crashDump = ref<CrashDumpStatus | null>(null);
  const vigemInstalled = ref<boolean | null>(null);
  const vigemVersion = ref('');
  const golden = ref<GoldenStatus | null>(null);
  const playnite = ref<PlayniteStatus | null>(null);
  const healthLoaded = ref(false);

  const logText = ref('');
  const logsLoaded = ref(false);

  const running = ref(false);
  let fastTimer: ReturnType<typeof setInterval> | null = null;
  let mediumTimer: ReturnType<typeof setInterval> | null = null;
  let slowTimer: ReturnType<typeof setInterval> | null = null;

  // ---- derived --------------------------------------------------------
  const platform = computed(() => String(configStore.metadata?.platform || '').toLowerCase());
  const isWindows = computed(() => platform.value === 'windows');
  const statsOnly = computed(() => auth.isStatsOnly());

  const activeSession = computed<SessionSummary | null>(
    () => sessions.value.find((s) => s.stream_ended_at == null) ?? null,
  );
  const recentSessions = computed(() => sessions.value.filter((s) => s.stream_ended_at != null));
  const lastSession = computed<SessionSummary | null>(() => recentSessions.value[0] ?? null);
  const connectedClients = computed(() => clients.value.filter((c) => c.connected));

  const logLines = computed(() => parseLogText(logText.value));
  const fatalLines = computed(() => logLines.value.filter((l) => l.level === 'Fatal'));

  const controllerEnabled = computed(
    () => (configStore.config as Record<string, unknown>)?.['controller'] === 'enabled',
  );

  const crashDumpVisible = computed(
    () =>
      isWindows.value &&
      isCrashDumpEligible(crashDump.value) &&
      crashDump.value?.dismissed !== true,
  );

  const healthChecks = computed<HealthCheck[]>(() => {
    if (statsOnly.value) return [];
    const checks: HealthCheck[] = [];

    if (isWindows.value) {
      const t = tdr.value;
      const count = typeof t?.count === 'number' ? t.count : null;
      checks.push({
        id: 'tdr',
        label: 'GPU / display stack',
        state: t?.stack_down ? 'danger' : count == null ? 'unknown' : count > 0 ? 'warn' : 'ok',
        detail: t?.stack_down
          ? 'display stack down · reboot required'
          : count == null
            ? 'not probed'
            : `${count} TDR since start`,
      });

      checks.push({
        id: 'crashdump',
        label: 'Crash dumps',
        state: crashDumpVisible.value ? 'danger' : crashDump.value == null ? 'unknown' : 'ok',
        detail: crashDumpVisible.value
          ? 'recent crash detected'
          : crashDump.value == null
            ? 'not probed'
            : 'none',
      });

      if (controllerEnabled.value) {
        checks.push({
          id: 'vigem',
          label: 'Gamepad bus',
          state:
            vigemInstalled.value === false
              ? 'warn'
              : vigemInstalled.value == null
                ? 'unknown'
                : 'ok',
          detail:
            vigemInstalled.value === false
              ? 'ViGEmBus not installed'
              : vigemInstalled.value == null
                ? 'not probed'
                : `ViGEmBus ${vigemVersion.value || 'installed'}`,
        });
      }

      const g = golden.value;
      checks.push({
        id: 'golden',
        label: 'Golden display snapshot',
        state:
          g?.exists === true && g.needs_layout_upgrade === true
            ? 'warn'
            : g == null
              ? 'unknown'
              : 'ok',
        detail:
          g?.exists === true && g.needs_layout_upgrade === true
            ? 'needs an update'
            : g == null
              ? 'not probed'
              : g.exists
                ? 'current'
                : 'not captured',
      });
    }

    checks.push({
      id: 'startup',
      label: 'Startup log',
      state: !logsLoaded.value ? 'unknown' : fatalLines.value.length > 0 ? 'danger' : 'ok',
      detail: !logsLoaded.value
        ? 'not loaded'
        : fatalLines.value.length > 0
          ? `${fatalLines.value.length} fatal`
          : '0 fatal',
    });

    if (isWindows.value && playnite.value?.installed) {
      const p = playnite.value;
      checks.push({
        id: 'playnite',
        label: 'Playnite extension',
        state: p.update_available ? 'warn' : 'ok',
        detail: p.update_available
          ? `update ${p.installed_version || '?'} → ${p.packaged_version || '?'}`
          : p.installed_version || 'installed',
      });
    }

    return checks;
  });

  const incidentSummary = computed(() => {
    if (statsOnly.value || !isWindows.value) return '';
    const parts: string[] = [];
    if (typeof tdr.value?.count === 'number') parts.push(`${tdr.value.count} TDR`);
    if (crashDump.value != null)
      parts.push(crashDumpVisible.value ? '1 crash dump' : '0 crash dumps');
    return parts.join(' · ');
  });

  const attentionCount = computed(
    () => healthChecks.value.filter((c) => c.state === 'warn' || c.state === 'danger').length,
  );

  // ---- fetchers -------------------------------------------------------
  async function refreshSessions(): Promise<void> {
    try {
      const r = await http.get('./api/sessions', { validateStatus: () => true });
      if (r.status === 503) {
        monitorOffline.value = true;
        sessions.value = [];
      } else if (r.status >= 200 && r.status < 300) {
        monitorOffline.value = false;
        const list = Array.isArray(r.data) ? (r.data as SessionSummary[]) : [];
        list.sort((a, b) => {
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
      sessionsLoaded.value = true;
    }
  }

  async function refreshClients(): Promise<void> {
    if (statsOnly.value) return;
    try {
      const r = await http.get('./api/clients/list', { validateStatus: () => true });
      if (r.status !== 200) return; // keep the previous list on a transient error
      const body = r.data as { named_certs?: unknown } | undefined;
      const list: ClientApiEntry[] =
        r.status === 200 && Array.isArray(body?.named_certs)
          ? (body.named_certs as ClientApiEntry[])
          : [];
      clients.value = list
        .filter((c) => typeof c.uuid === 'string' && c.uuid)
        .map((c) => ({
          uuid: String(c.uuid),
          name: typeof c.name === 'string' && c.name.trim() ? c.name.trim() : '',
          connected: c.connected === true,
          lastSeen: toEpochSeconds(c.last_seen),
        }));
      clients.value.sort((a, b) => {
        if (a.connected !== b.connected) return a.connected ? -1 : 1;
        return (b.lastSeen ?? 0) - (a.lastSeen ?? 0);
      });
    } catch {
      /* keep the previous list */
    } finally {
      clientsLoaded.value = true;
    }
  }

  async function refreshTdr(): Promise<void> {
    try {
      const r = await http.get('./api/health/tdr', { validateStatus: () => true });
      tdr.value =
        r.status === 200 && r.data && typeof r.data === 'object' ? (r.data as TdrStatus) : null;
    } catch {
      tdr.value = null;
    }
  }

  async function refreshCrashDump(): Promise<void> {
    try {
      const r = await http.get('/api/health/crashdump', { validateStatus: () => true });
      if (r.status === 200 && r.data) {
        crashDump.value = sanitizeCrashDumpStatus(r.data as CrashDumpStatus) ?? {
          available: false,
        };
      } else {
        crashDump.value = { available: false };
      }
    } catch {
      crashDump.value = null;
    }
  }

  async function refreshVigem(): Promise<void> {
    if (!controllerEnabled.value) {
      vigemInstalled.value = null;
      return;
    }
    try {
      const r = await http.get('/api/health/vigem', { validateStatus: () => true });
      const body = r.data as { installed?: boolean; version?: string } | undefined;
      if (r.status === 200 && body) {
        vigemInstalled.value = !!body.installed;
        vigemVersion.value = body.version || '';
      } else {
        vigemInstalled.value = null;
      }
    } catch {
      vigemInstalled.value = null;
    }
  }

  async function refreshGolden(): Promise<void> {
    try {
      const r = await http.get('/api/display/golden_status', { validateStatus: () => true });
      golden.value = r.status === 200 && r.data ? (r.data as GoldenStatus) : null;
    } catch {
      golden.value = null;
    }
  }

  async function refreshPlaynite(): Promise<void> {
    try {
      const r = await http.get('/api/playnite/status', { validateStatus: () => true });
      playnite.value = r.status === 200 && r.data ? (r.data as PlayniteStatus) : null;
    } catch {
      playnite.value = null;
    }
  }

  async function refreshHealth(): Promise<void> {
    if (statsOnly.value) return;
    if (isWindows.value) {
      await Promise.all([
        refreshTdr(),
        refreshCrashDump(),
        refreshVigem(),
        refreshGolden(),
        refreshPlaynite(),
      ]);
    }
    healthLoaded.value = true;
  }

  async function refreshLogs(): Promise<void> {
    if (statsOnly.value) return;
    try {
      const r = await http.get('./api/logs', {
        responseType: 'text',
        transformResponse: [(v: string) => v],
        validateStatus: () => true,
      });
      if (r.status === 200 && typeof r.data === 'string') {
        logText.value = r.data;
      }
    } catch {
      /* keep the previous tail */
    } finally {
      logsLoaded.value = true;
    }
  }

  async function refreshAll(): Promise<void> {
    await Promise.all([refreshSessions(), refreshClients(), refreshHealth(), refreshLogs()]);
  }

  // ---- lifecycle ------------------------------------------------------
  async function start(): Promise<void> {
    if (running.value) return;
    running.value = true;
    // The probes key off metadata.platform and config.controller, so make sure
    // the config (and with it the metadata) is loaded before the first sweep.
    try {
      await configStore.fetchConfig();
    } catch {
      /* probes report 'not probed' and retry on the slow timer */
    }
    await refreshAll();
    fastTimer = setInterval(() => void refreshSessions(), FAST_MS);
    mediumTimer = setInterval(() => void refreshClients(), MEDIUM_MS);
    slowTimer = setInterval(() => void refreshHealth(), SLOW_MS);
  }

  function stop(): void {
    running.value = false;
    if (fastTimer) clearInterval(fastTimer);
    if (mediumTimer) clearInterval(mediumTimer);
    if (slowTimer) clearInterval(slowTimer);
    fastTimer = mediumTimer = slowTimer = null;
  }

  function removeSession(id: string): void {
    sessions.value = sessions.value.filter((s) => s.id !== id);
  }

  function setCrashDump(value: CrashDumpStatus | null): void {
    crashDump.value = value;
  }

  return {
    // state
    sessions,
    monitorOffline,
    sessionsLoaded,
    clients,
    clientsLoaded,
    tdr,
    crashDump,
    vigemInstalled,
    vigemVersion,
    golden,
    playnite,
    healthLoaded,
    logText,
    logsLoaded,
    running,
    // derived
    isWindows,
    statsOnly,
    activeSession,
    recentSessions,
    lastSession,
    connectedClients,
    logLines,
    fatalLines,
    crashDumpVisible,
    healthChecks,
    incidentSummary,
    attentionCount,
    // actions
    start,
    stop,
    refreshAll,
    refreshSessions,
    refreshClients,
    refreshHealth,
    refreshLogs,
    refreshCrashDump,
    refreshPlaynite,
    removeSession,
    setCrashDump,
  };
});
