import { beforeEach, describe, expect, it, vi } from 'vitest';
import { mount } from '@vue/test-utils';
import { createPinia, setActivePinia } from 'pinia';

vi.mock('@web/composables/useT2', () => ({
  useT2: () => (_key: string, fallback: string) => fallback,
}));

import HostStrip from '@web/components/shell/HostStrip.vue';
import { useConfigStore } from '@web/stores/config';

describe('HostStrip codec order', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
  });

  it('places compiled PyroWave immediately after AV1 without claiming it is advertised', () => {
    const config = useConfigStore();
    config.metadata = {
      encoder_probe: {
        probed: true,
        h264_available: true,
        hevc_available: true,
        av1_available: true,
        pyrowave_compiled: true,
        pyrowave_advertised: false,
      },
    };

    const wrapper = mount(HostStrip, {
      global: {
        stubs: {
          GlobalSearch: true,
          SavingStatus: true,
        },
      },
    });

    expect(wrapper.text()).toContain('Auto · H.264 · HEVC · AV1 · PyroWave');
  });

  it('does not list PyroWave when the backend is not compiled in', () => {
    const config = useConfigStore();
    config.metadata = {
      encoder_probe: {
        probed: true,
        h264_available: true,
        hevc_available: true,
        av1_available: true,
        pyrowave_compiled: false,
        pyrowave_advertised: true,
      },
    };

    const wrapper = mount(HostStrip, {
      global: {
        stubs: {
          GlobalSearch: true,
          SavingStatus: true,
        },
      },
    });

    expect(wrapper.text()).toContain('Auto · H.264 · HEVC · AV1');
    expect(wrapper.text()).not.toContain('PyroWave');
  });
});
