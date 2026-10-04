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

  it('leaves build-only PyroWave placement to HostStrip', () => {
    expect(
      encoderCodecLabels({
        probed: true,
        h264_available: true,
        hevc_available: true,
        av1_available: true,
        pyrowave_compiled: true,
        pyrowave_advertised: true,
      }),
    ).toEqual(['H.264', 'HEVC', 'AV1']);
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

  it('does not confuse advertisement state with regular probe results', () => {
    expect(
      encoderCodecLabels({
        probed: false,
        h264_available: true,
        pyrowave_compiled: true,
        pyrowave_advertised: true,
      }),
    ).toEqual([]);
    expect(encoderCodecLabels()).toEqual([]);
  });
});
