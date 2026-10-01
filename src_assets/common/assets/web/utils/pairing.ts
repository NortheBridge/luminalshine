export type PairingState = 'pending_pin' | 'awaiting_client' | 'paired' | 'failed' | 'expired';

export interface PairingApiEntry {
  pairing_id: string;
  client_address?: string;
  state: PairingState;
  age_seconds?: number;
  device_uuid?: string;
}

export interface PairingSubmissionResult {
  accepted: boolean;
  pairingId: string | null;
  state: 'awaiting_client' | 'paired' | 'failed';
  reason: string;
}

function apiStatusIsTrue(value: unknown): boolean {
  return value === true || value === 'true' || value === 1;
}

export function parsePairingSubmission(
  httpOk: boolean,
  body: { status?: unknown; result?: unknown; state?: unknown; pairing_id?: unknown } | undefined,
): PairingSubmissionResult {
  const accepted = httpOk && apiStatusIsTrue(body?.status);
  const reason = typeof body?.result === 'string' ? body.result : accepted ? 'pin_delivered' : 'pairing_failed';
  const pairingId = typeof body?.pairing_id === 'string' ? body.pairing_id : null;

  if (accepted && (reason === 'already_paired' || body?.state === 'paired')) {
    return { accepted: true, pairingId, state: 'paired', reason };
  }
  if (accepted) {
    return { accepted: true, pairingId, state: 'awaiting_client', reason };
  }
  return { accepted: false, pairingId, state: 'failed', reason };
}

export function choosePendingPairing(
  pairings: PairingApiEntry[],
  currentPairingId: string | null,
): string | null {
  const pending = pairings.filter((entry) => entry.state === 'pending_pin');
  if (currentPairingId && pending.some((entry) => entry.pairing_id === currentPairingId)) {
    return currentPairingId;
  }
  return pending.length === 1 ? pending[0]!.pairing_id : null;
}

export function findPairingState(
  pairings: PairingApiEntry[],
  pairingId: string | null,
): PairingState | null {
  if (!pairingId) return null;
  return pairings.find((entry) => entry.pairing_id === pairingId)?.state ?? null;
}

export function pairingOptionLabel(pairing: PairingApiEntry): string {
  const suffix = pairing.pairing_id.slice(-8);
  return pairing.client_address ? `${pairing.client_address} · ${suffix}` : suffix;
}
