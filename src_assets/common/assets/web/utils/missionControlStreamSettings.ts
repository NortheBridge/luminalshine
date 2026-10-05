export type CodecMode = 'hevc' | 'av1';

export function codecModeDescription(
  translateWithFallback: (key: string, fallback: string) => string,
  codec: CodecMode,
  value: unknown,
): string {
  const mode = typeof value === 'number' ? value : Number(value);
  if (!Number.isInteger(mode) || mode < 0 || mode > 3) return '';
  return translateWithFallback(`config.${codec}_mode_${mode}`, '');
}

export function formatAutomaticBitrate(value: number | null, automaticLabel: string): string {
  if (value === null) return '';
  return value === 0 ? `0 (${automaticLabel})` : String(value);
}

export function parseAutomaticBitrate(input: string, automaticLabel: string): number | null {
  const value = input.trim();
  if (!value) return null;
  if (value === `0 (${automaticLabel})`) return 0;

  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}
