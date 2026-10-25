import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import {
  loadNotes,
  newestEligible,
  parseNotes,
  versionKey,
  type ReleaseNotes,
} from '@web/services/changelog';
import { useChangelogStore } from '@web/stores/changelog';
import { http } from '@web/http';

vi.mock('@web/http', () => ({ http: { get: vi.fn(), post: vi.fn() } }));
const release = (version: string, prerelease = false): ReleaseNotes => ({
  version,
  title: version,
  markdown: 'Release notes',
  github_url: `https://github.com/NortheBridge/luminalshine/releases/tag/${version}`,
  featurebase_url: '',
  published_at: '2026-10-25T00:00:00Z',
  prerelease,
});
const installed = release('26.10.0');
const update = release('26.11.0');
const state = () => ({
  installed_version: installed.version,
  notify_pre_releases: false,
  seen_installed_versions: [] as string[],
  dismissed_update_version: '',
});
const response = (data: unknown) => ({ ok: true, json: async () => data });
let hostState = state();

beforeEach(() => {
  setActivePinia(createPinia());
  vi.clearAllMocks();
  localStorage.clear();
  hostState = state();
  vi.mocked(http.get).mockImplementation(async () => ({ data: structuredClone(hostState) }));
  vi.mocked(http.post).mockImplementation(async (_url, body) => {
    const ack = body as { kind: string; version: string };
    if (ack.kind === 'installed') hostState.seen_installed_versions.push(ack.version);
    else hostState.dismissed_update_version = ack.version;
    return { data: structuredClone(hostState) };
  });
  vi.stubGlobal(
    'fetch',
    vi.fn(async () => response({ schema: 1, releases: [update, installed] })),
  );
});

describe('release selection and content', () => {
  it('honors release channels and semantic rather than lexical version order', () => {
    const releases = [release('26.9.0'), release('26.12.0-beta.1', true), release('26.11.0')];
    expect(newestEligible(releases, '26.10.0', false)?.version).toBe('26.11.0');
    expect(newestEligible(releases, '26.10.0', true)?.version).toBe('26.12.0-beta.1');
    expect(newestEligible(releases, '0.0.0', true)).toBeUndefined();
    expect(newestEligible(releases, '27.0.0', true)).toBeUndefined();
    expect(versionKey('v26.10.0')).toBe('26.10.0');
  });
  it('rejects drafts, invalid versions and unrelated download links', () => {
    expect(
      parseNotes([
        { ...installed, draft: true },
        { ...installed, version: '' },
        { ...installed, github_url: 'javascript:alert(1)' },
      ]),
    ).toEqual([]);
  });
  it('falls back to GitHub when feed is unavailable', async () => {
    vi.mocked(fetch)
      .mockRejectedValueOnce(new Error('offline'))
      .mockResolvedValueOnce(
        response([
          { tag_name: installed.version, body: 'GitHub notes', html_url: installed.github_url },
        ]) as Response,
      );
    const result = await loadNotes(installed.version);
    expect(result.releases[0]?.markdown).toBe('GitHub notes');
  });
  it('keeps notes available offline without storing acknowledgements in the browser', async () => {
    await loadNotes(installed.version);
    vi.mocked(fetch).mockRejectedValue(new Error('offline'));
    const result = await loadNotes(installed.version);
    expect(result.cached).toBe(true);
    expect(result.releases).toContainEqual(installed);
    expect(localStorage.length).toBe(1);
  });
});

describe('host-backed changelog lifecycle', () => {
  it('opens installed notes before available updates and only acknowledges after display', async () => {
    const store = useChangelogStore();
    await store.refresh();
    expect(store.mode).toBe('installed');
    expect(store.selected?.version).toBe(installed.version);
    expect(http.post).not.toHaveBeenCalled();
    await store.acknowledgeDisplayed();
    expect(hostState.seen_installed_versions).toEqual([installed.version]);
    store.close();
    await store.refresh(true, true);
    expect(store.visible).toBe(false);
    expect(store.available?.version).toBe(update.version);
  });
  it('uses host acknowledgement on another browser and still detects an update', async () => {
    hostState.seen_installed_versions = [installed.version];
    const store = useChangelogStore();
    await store.refresh();
    expect(store.mode).toBe('update');
    await store.acknowledgeDisplayed();
    expect(http.post).not.toHaveBeenCalled();
    store.close();
    await Promise.resolve();
    expect(hostState.dismissed_update_version).toBe(update.version);
  });
  it('never acknowledges future notes as installed; shows them after installation', async () => {
    hostState.seen_installed_versions = [installed.version];
    const store = useChangelogStore();
    await store.refresh();
    await store.acknowledgeDisplayed();
    expect(hostState.seen_installed_versions).not.toContain(update.version);
    store.reset();
    hostState.installed_version = update.version;
    await store.refresh();
    expect(store.mode).toBe('installed');
    expect(store.selected?.version).toBe(update.version);
  });
  it('reopens manually after seen and dismissed state without a new automatic popup', async () => {
    hostState.seen_installed_versions = [installed.version];
    hostState.dismissed_update_version = update.version;
    const store = useChangelogStore();
    await store.refresh();
    expect(store.visible).toBe(false);
    await store.open();
    expect(store.visible).toBe(true);
    expect(store.mode).toBe('manual');
    store.close();
    expect(store.visible).toBe(false);
  });
  it('does not mark missing content as seen', async () => {
    vi.mocked(fetch).mockResolvedValue(
      response({ schema: 1, releases: [{ ...installed, markdown: '' }] }) as Response,
    );
    const store = useChangelogStore();
    await store.refresh();
    expect(store.visible).toBe(false);
    await store.open();
    await store.acknowledgeDisplayed();
    expect(http.post).not.toHaveBeenCalled();
  });
  it('keeps acknowledgement failures retryable', async () => {
    const store = useChangelogStore();
    await store.refresh();
    vi.mocked(http.post).mockRejectedValueOnce(new Error('disk full'));
    await store.acknowledgeDisplayed();
    expect(store.acknowledgementError).toBe(true);
    expect(hostState.seen_installed_versions).toEqual([]);
    await store.acknowledgeDisplayed();
    expect(store.acknowledgementError).toBe(false);
    expect(hostState.seen_installed_versions).toEqual([installed.version]);
  });
  it('does not open a popup when host state cannot be read', async () => {
    vi.mocked(http.get).mockRejectedValue(new Error('503'));
    const store = useChangelogStore();
    await store.refresh();
    expect(store.visible).toBe(false);
    expect(store.error).toBe(true);
  });
  it('loads notes on a manual retry after the host recovers', async () => {
    vi.mocked(http.get).mockRejectedValueOnce(new Error('503'));
    const store = useChangelogStore();
    await store.open();
    expect(store.error).toBe(true);
    await store.refresh(false, true);
    expect(store.selected?.version).toBe(installed.version);
    expect(store.visible).toBe(true);
  });
  it('lets development builds browse releases without marking them installed', async () => {
    hostState.installed_version = '0.0.0';
    const store = useChangelogStore();
    await store.open();
    expect(store.selected?.version).toBe(update.version);
    await store.acknowledgeDisplayed();
    expect(http.post).not.toHaveBeenCalled();
  });
  it('ignores an in-flight request after logout', async () => {
    let resolve!: (value: { data: ReturnType<typeof state> }) => void;
    vi.mocked(http.get).mockReturnValue(
      new Promise((r) => {
        resolve = r;
      }),
    );
    const store = useChangelogStore();
    const pending = store.refresh();
    store.reset();
    resolve({ data: hostState });
    await pending;
    expect(store.visible).toBe(false);
    expect(store.state).toBeUndefined();
  });
});
