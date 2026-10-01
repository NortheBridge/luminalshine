import { describe, expect, it } from 'vitest';
import {
  choosePendingPairing,
  findPairingState,
  pairingOptionLabel,
  parsePairingSubmission,
  type PairingApiEntry,
} from '@web/utils/pairing';

const pending = (id: string, address = '192.0.2.10'): PairingApiEntry => ({
  pairing_id: id,
  client_address: address,
  state: 'pending_pin',
});

describe('pairing UI policy', () => {
  it('treats a delivered PIN as awaiting proof rather than paired', () => {
    expect(
      parsePairingSubmission(true, {
        status: true,
        result: 'pin_delivered',
        state: 'awaiting_client',
        pairing_id: 'request-a',
      }),
    ).toEqual({
      accepted: true,
      pairingId: 'request-a',
      state: 'awaiting_client',
      reason: 'pin_delivered',
    });
  });

  it('reports paired only when the host reports final completion', () => {
    expect(
      parsePairingSubmission(true, {
        status: true,
        result: 'already_paired',
        state: 'paired',
        pairing_id: 'request-a',
      }).state,
    ).toBe('paired');
  });

  it('retains legacy status-only response compatibility', () => {
    const result = parsePairingSubmission(true, { status: 1 });
    expect(result.accepted).toBe(true);
    expect(result.state).toBe('awaiting_client');
  });

  it('does not auto-select when multiple clients are waiting', () => {
    expect(choosePendingPairing([pending('request-a'), pending('request-b')], null)).toBeNull();
  });

  it('auto-selects exactly one request and preserves an explicit choice', () => {
    expect(choosePendingPairing([pending('request-a')], null)).toBe('request-a');
    expect(
      choosePendingPairing([pending('request-a'), pending('request-b')], 'request-b'),
    ).toBe('request-b');
  });

  it('tracks terminal state by opaque request id', () => {
    expect(
      findPairingState([{ pairing_id: 'request-a', state: 'paired' }], 'request-a'),
    ).toBe('paired');
    expect(findPairingState([], 'request-a')).toBeNull();
  });

  it('labels a request without exposing protocol credentials', () => {
    expect(
      pairingOptionLabel(pending('01234567-89ab-cdef-0123-456789abcdef', '192.0.2.10')),
    ).toBe(
      '192.0.2.10 · 89abcdef',
    );
  });
});
