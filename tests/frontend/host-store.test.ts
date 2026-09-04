import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';

vi.mock('@web/http', () => ({
  http: {
    get: vi.fn(),
    post: vi.fn(),
    delete: vi.fn(),
  },
  refreshSession: vi.fn(async () => false),
  initHttpLayer: vi.fn(async () => {}),
}));

import { http } from '@web/http';
import { parseLogText, useHostStore } from '@web/stores/host';
import { useConfigStore } from '@web/stores/config';

const LOG =
  '[2026-09-04 14:30:02.551]: Info: LuminalShine 26.08.3 started\n' +
  '[2026-09-04 14:30:03.004]: Warning: Playnite extension outdated\n' +
  '[2026-09-04 14:30:04.100]: Fatal: Failed to bind port 47989\n';

type Route = (url: string) => { status: number; data: unknown };

function mockGet(route: Route) {
  (http.get as unknown as ReturnType<typeof vi.fn>).mockImplementation(async (url: string) =>
    route(url),
  );
}

describe('parseLogText', () => {
  it('splits the log into timestamped, levelled lines', () => {
    const lines = parseLogText(LOG);
    expect(lines).toHaveLength(3);
    expect(lines[0]).toMatchObject({ time: '14:30:02', level: 'Info' });
    expect(lines[0]?.message).toBe('LuminalShine 26.08.3 started');
    expect(lines[2]).toMatchObject({ level: 'Fatal', message: 'Failed to bind port 47989' });
  });

  it('returns nothing for an empty log', () => {
    expect(parseLogText('')).toEqual([]);
  });
});

describe('host store', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    vi.clearAllMocks();
  });

  it('derives the live session, sorts history and tracks the monitor', async () => {
    mockGet((url) => {
      if (url.includes('/api/sessions')) {
        return {
          status: 200,
          data: [
            { id: 'old', started_at: 100, stream_ended_at: 200, metadata: {} },
            { id: 'live', started_at: 300, stream_ended_at: null, metadata: {} },
            { id: 'newer', started_at: 250, stream_ended_at: 280, metadata: {} },
          ],
        };
      }
      return { status: 404, data: null };
    });
    const host = useHostStore();
    await host.refreshSessions();
    expect(host.activeSession?.id).toBe('live');
    expect(host.recentSessions.map((s) => s.id)).toEqual(['newer', 'old']);
    expect(host.lastSession?.id).toBe('newer');
    expect(host.monitorOffline).toBe(false);
    expect(host.sessionsLoaded).toBe(true);

    mockGet(() => ({ status: 503, data: null }));
    await host.refreshSessions();
    expect(host.monitorOffline).toBe(true);
    expect(host.activeSession).toBeNull();
  });

  it('maps the clients list and puts connected clients first', async () => {
    mockGet((url) => {
      if (url.includes('/api/clients/list')) {
        return {
          status: 200,
          data: {
            status: true,
            named_certs: [
              { uuid: 'b', name: 'Steam Deck', connected: false, last_seen: 1000 },
              { uuid: 'a', name: 'Xbox', connected: true, last_seen: 900 },
              { name: 'no uuid — dropped', connected: true },
            ],
          },
        };
      }
      return { status: 404, data: null };
    });
    const host = useHostStore();
    await host.refreshClients();
    expect(host.clients.map((c) => c.uuid)).toEqual(['a', 'b']);
    expect(host.connectedClients).toHaveLength(1);
    expect(host.clients[1]?.lastSeen).toBe(1000);
  });

  it('turns the Windows health probes into checklist rows', async () => {
    const config = useConfigStore();
    config.metadata = { platform: 'windows' };
    mockGet((url) => {
      if (url.includes('/api/health/tdr')) return { status: 200, data: { count: 0 } };
      if (url.includes('/api/health/crashdump')) return { status: 200, data: { available: false } };
      if (url.includes('/api/health/vigem'))
        return { status: 200, data: { installed: true, version: '1.22' } };
      if (url.includes('/api/display/golden_status')) {
        return { status: 200, data: { exists: true, needs_layout_upgrade: true } };
      }
      if (url.includes('/api/playnite/status')) {
        return {
          status: 200,
          data: {
            installed: true,
            active: true,
            update_available: true,
            installed_version: '1.4.2',
            packaged_version: '1.4.3',
          },
        };
      }
      if (url.includes('/api/logs')) return { status: 200, data: LOG };
      return { status: 404, data: null };
    });
    const host = useHostStore();
    await host.refreshHealth();
    await host.refreshLogs();

    const byId = Object.fromEntries(host.healthChecks.map((c) => [c.id, c]));
    expect(byId['tdr']).toMatchObject({ state: 'ok', detail: '0 TDR since start' });
    expect(byId['crashdump']).toMatchObject({ state: 'ok', detail: 'none' });
    expect(byId['golden']).toMatchObject({ state: 'warn' });
    expect(byId['playnite']).toMatchObject({ state: 'warn', detail: 'update 1.4.2 → 1.4.3' });
    expect(byId['startup']).toMatchObject({ state: 'danger', detail: '1 fatal' });
    expect(host.fatalLines).toHaveLength(1);
    expect(host.attentionCount).toBe(3);
    expect(host.incidentSummary).toBe('0 TDR · 0 crash dumps');
  });

  it('skips the admin-only probes for a stats-only session', async () => {
    const { useAuthStore } = await import('@web/stores/auth');
    const auth = useAuthStore();
    auth.setRole('stats');
    auth.setAuthenticated(true);
    mockGet(() => ({ status: 200, data: [] }));
    const host = useHostStore();
    await host.refreshHealth();
    await host.refreshClients();
    await host.refreshLogs();
    expect(host.healthChecks).toEqual([]);
    const calls = (http.get as unknown as ReturnType<typeof vi.fn>).mock.calls.map((c) =>
      String(c[0]),
    );
    expect(calls.some((u) => u.includes('/api/health'))).toBe(false);
    expect(calls.some((u) => u.includes('/api/clients'))).toBe(false);
    expect(calls.some((u) => u.includes('/api/logs'))).toBe(false);
  });
});
