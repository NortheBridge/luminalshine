import { describe, expect, it } from 'vitest';
import { getConfigSelectOptions } from '@web/configs/configSelectOptions';

describe('Windows virtual gamepad profiles', () => {
  const t = (key: string) => key;
  it('offers replacement-driver profiles', () => {
    const options = getConfigSelectOptions('gamepad', { t, platform: 'windows' });
    expect(options?.map((option) => option.value)).toEqual([
      'auto',
      'xbox_series',
      'xbox_one',
      'ds4',
      'dualsense',
      'switch_pro',
    ]);
  });
  it('preserves a saved Xbox 360 alias without offering it to new configurations', () => {
    const options = getConfigSelectOptions('gamepad', {
      t: (key) =>
        key === 'config.gamepad_x360_legacy' ? 'Xbox Series (legacy Xbox 360 setting)' : key,
      platform: 'windows',
      currentValue: 'x360',
    });
    expect(options?.find((option) => option.value === 'x360')?.label).toBe(
      'Xbox Series (legacy Xbox 360 setting)',
    );
  });
});
