import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';

vi.mock('@web/http', () => ({
  http: {
    get: vi.fn(),
    post: vi.fn(),
    patch: vi.fn(),
    delete: vi.fn(),
  },
  refreshSession: vi.fn(async () => false),
  initHttpLayer: vi.fn(async () => {}),
}));

import { coerceConfigBoolean, useConfigStore } from '@web/stores/config';

describe('coerceConfigBoolean', () => {
  it('accepts the same vocabulary as the host to_bool()', () => {
    for (const v of [true, 1, 'true', 'TRUE', ' yes ', 'enable', 'enabled', 'on', '1']) {
      expect(coerceConfigBoolean(v, false), String(v)).toBe(true);
    }
    for (const v of [false, 0, 'false', 'no', 'disable', 'disabled', 'off', '0']) {
      expect(coerceConfigBoolean(v, true), String(v)).toBe(false);
    }
  });

  it('uses the fallback for an absent or empty value, like bool_f', () => {
    for (const v of [null, undefined, '', '   ']) {
      expect(coerceConfigBoolean(v, true)).toBe(true);
      expect(coerceConfigBoolean(v, false)).toBe(false);
    }
  });

  it('reads anything else as off, like to_bool — including its "contains 1" quirk', () => {
    for (const v of ['auto', 'maybe', 'sometimes', {}]) {
      expect(coerceConfigBoolean(v, true)).toBe(false);
    }
    expect(coerceConfigBoolean('10', false)).toBe(true);
  });
});

// /api/config returns the conf file verbatim, so every value arrives as a
// string. Issue #178: the YUV 4:4:4 switch compared that string with
// `!== false`, read "false" as on, and showed checked after every refresh
// while the host had the option off.
describe('config store: boolean keys loaded from /api/config', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    setActivePinia(createPinia());
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('coerces conf-file strings on boolean-default keys to real booleans', () => {
    const store = useConfigStore();
    store.setConfig({
      yuv444_streaming: 'false',
      prefer_10bit_sdr: 'true',
      session_monitor: 'disabled',
      steam_auto_sync: 'enabled',
      playnite_auto_sync: '1',
    });
    expect(store.config.yuv444_streaming).toBe(false);
    expect(store.config.prefer_10bit_sdr).toBe(true);
    expect(store.config.session_monitor).toBe(false);
    expect(store.config.steam_auto_sync).toBe(true);
    expect(store.config.playnite_auto_sync).toBe(true);
  });

  it('leaves string-valued keys and keys without a default untouched', () => {
    const store = useConfigStore();
    store.setConfig({ controller: 'enabled', capture: 'wgc', some_future_key: 'false' });
    expect(store.config.controller).toBe('enabled');
    expect(store.config.capture).toBe('wgc');
    expect((store.config as Record<string, unknown>)['some_future_key']).toBe('false');
  });

  it('reads a hand-edited value the way the host will, not the way the UI hopes', () => {
    const store = useConfigStore();
    store.setConfig({ yuv444_streaming: 'sometimes', session_monitor: '' });
    expect(store.config.yuv444_streaming).toBe(false); // to_bool: off
    expect(store.config.session_monitor).toBe(true); // bool_f: empty keeps the default
  });

  it('treats writing the value the file already holds as a no-op', () => {
    const store = useConfigStore();
    store.setConfig({ yuv444_streaming: 'false' });
    // Before coercion this compared "false" with false, recorded a change and
    // queued a PATCH that rewrote the same value — the "toggle does nothing"
    // half of #178.
    store.config.yuv444_streaming = false;
    expect(store.hasPendingPatch()).toBe(false);
    expect(store.savingState).toBe('idle');
  });

  it('queues a PATCH when the stored value actually changes', () => {
    const store = useConfigStore();
    store.setConfig({ yuv444_streaming: 'false' });
    store.config.yuv444_streaming = true;
    expect(store.hasPendingPatch()).toBe(true);
    expect(store.savingState).toBe('dirty');
  });
});
