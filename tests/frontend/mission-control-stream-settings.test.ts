import {
  codecModeDescription,
  formatAutomaticBitrate,
  parseAutomaticBitrate,
} from '@web/utils/missionControlStreamSettings';

describe('Mission Control stream settings', () => {
  const t2 = (key: string, fallback: string) =>
    key === 'config.hevc_mode_2' ? 'HEVC Main profile' : fallback;

  test('shows the complete selected codec-mode description', () => {
    expect(codecModeDescription(t2, 'hevc', 2)).toBe('HEVC Main profile');
    expect(codecModeDescription(t2, 'av1', 3)).toBe('');
    expect(codecModeDescription(t2, 'hevc', 4)).toBe('');
  });

  test('labels zero bitrate as automatic without changing numeric values', () => {
    expect(formatAutomaticBitrate(0, 'Automatic')).toBe('0 (Automatic)');
    expect(formatAutomaticBitrate(80000, 'Automatic')).toBe('80000');
    expect(formatAutomaticBitrate(null, 'Automatic')).toBe('');
  });

  test('parses the automatic label and ordinary numeric input', () => {
    expect(parseAutomaticBitrate('0 (Automatic)', 'Automatic')).toBe(0);
    expect(parseAutomaticBitrate('0', 'Automatic')).toBe(0);
    expect(parseAutomaticBitrate('80000', 'Automatic')).toBe(80000);
    expect(parseAutomaticBitrate('', 'Automatic')).toBeNull();
    expect(parseAutomaticBitrate('not a bitrate', 'Automatic')).toBeNull();
  });
});
