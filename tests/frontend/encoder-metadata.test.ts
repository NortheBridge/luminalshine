import { describe, expect, it } from 'vitest';
import { encoderCodecLabels } from '@web/utils/encoderMetadata';

describe('encoder top-bar metadata', () => {
  it('preserves the regular codec order', () => {
    expect(
      encoderCodecLabels({
        probed: true,
        h264_available: true,
        hevc_available: true,
        av1_available: true,
      }),
    ).toEqual(['H.264', 'HEVC', 'AV1']);
  });

  it('appends advertised PyroWave capability', () => {
    expect(
      encoderCodecLabels({
        probed: true,
        h264_available: true,
        hevc_available: true,
        av1_available: true,
        pyrowave_advertised: true,
      }),
    ).toEqual(['H.264', 'HEVC', 'AV1', 'PyroWave']);
  });

  it('stays backward compatible when PyroWave metadata is absent or false', () => {
    expect(encoderCodecLabels({ probed: true, h264_available: true })).toEqual(['H.264']);
    expect(
      encoderCodecLabels({
        probed: true,
        h264_available: true,
        pyrowave_advertised: false,
      }),
    ).toEqual(['H.264']);
  });

  it('keeps the independent PyroWave result while regular probing is incomplete', () => {
    expect(
      encoderCodecLabels({
        probed: false,
        h264_available: true,
        pyrowave_advertised: true,
      }),
    ).toEqual(['PyroWave']);
    expect(encoderCodecLabels()).toEqual([]);
  });
});
