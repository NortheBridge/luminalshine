<template>
  <div v-if="visible" class="min-w-0">
    <n-card :segmented="{ content: true, footer: true }">
      <template #header>
        <h2 class="text-xl sm:text-2xl font-semibold tracking-tight mx-auto text-center break-words">
          {{ $t('high_perf.title') }}
        </h2>
      </template>

      <div class="space-y-4 text-sm">
        <p class="opacity-80 m-0 leading-relaxed">
          {{ $t('high_perf.description') }}
        </p>

        <n-alert type="warning" :show-icon="true" class="rounded-xl">
          <p class="text-sm m-0 font-medium">{{ $t('high_perf.requirements_title') }}</p>
          <ul class="text-xs opacity-90 mt-2 mb-0 ps-5 space-y-1 list-disc">
            <li>{{ $t('high_perf.req_gpu') }}</li>
            <li>{{ $t('high_perf.req_host_net') }}</li>
            <li>{{ $t('high_perf.req_client_net') }}</li>
            <li>{{ $t('high_perf.req_cpu') }}</li>
            <li>{{ $t('high_perf.req_resource_use') }}</li>
          </ul>
        </n-alert>

        <n-alert type="info" :show-icon="true" class="rounded-xl">
          <p class="text-sm m-0 font-medium">{{ $t('high_perf.tested_title') }}</p>
          <p class="text-xs opacity-90 m-0 mt-1 leading-relaxed">
            {{ $t('high_perf.tested_body') }}
          </p>
        </n-alert>

        <div
          class="flex flex-col gap-3 rounded-lg border border-dark/10 dark:border-light/10 bg-surface/60 dark:bg-dark/40 p-3 md:flex-row md:items-center md:justify-between"
        >
          <div class="min-w-0">
            <p class="text-sm m-0 font-medium">{{ $t('high_perf.toggle_label') }}</p>
            <p class="text-xs opacity-75 m-0 mt-1">
              <span v-if="vendor === 'nvidia'">{{ $t('high_perf.vendor_nvidia') }}</span>
              <span v-else-if="vendor === 'amd'">{{ $t('high_perf.vendor_amd') }}</span>
              <span v-else-if="vendor === 'intel'">{{ $t('high_perf.vendor_intel') }}</span>
              <span v-else>{{ $t('high_perf.vendor_unknown') }}</span>
            </p>
          </div>
          <n-switch
            :value="isEnabled"
            :loading="busy"
            :disabled="busy || !configReady"
            size="medium"
            @update:value="onToggle"
          />
        </div>

        <div class="grid gap-2 sm:flex sm:flex-wrap sm:items-center sm:justify-end">
          <n-button
            size="small"
            type="default"
            strong
            class="w-full justify-center sm:w-auto"
            :disabled="busy || !hasSnapshot || !configReady"
            @click="onRestorePrevious"
          >
            <i class="fas fa-rotate-left" />
            <span>{{ $t('high_perf.restore_previous') }}</span>
          </n-button>
          <n-button
            size="small"
            type="error"
            strong
            secondary
            class="w-full justify-center sm:w-auto"
            :disabled="busy || !configReady"
            @click="onRestoreDefaults"
          >
            <i class="fas fa-undo" />
            <span>{{ $t('high_perf.restore_default') }}</span>
          </n-button>
        </div>
      </div>
    </n-card>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, onMounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { storeToRefs } from 'pinia';
import { NCard, NAlert, NSwitch, NButton, useMessage } from 'naive-ui';
import { useConfigStore } from '@/stores/config';

type Vendor = 'nvidia' | 'amd' | 'intel' | 'unknown';

const SNAPSHOT_KEY = 'luminalshine.high_perf.snapshot.v1';

const HP_BASE: Record<string, unknown> = {
  virtual_display_mode: 'per_client',
  virtual_display_layout: 'exclusive',
  dd_configuration_option: 'ensure_only_display',
  dd_wa_virtual_double_refresh: true,
  min_threads: 4,
};

const HP_NVIDIA: Record<string, unknown> = {
  nvenc_preset: 6,
  nvenc_realtime_hags: 'disabled',
};

const HP_AMD: Record<string, unknown> = {
  amd_quality: 'quality',
};

const HP_INTEL: Record<string, unknown> = {
  qsv_preset: 'slower',
};

const DEFAULTS: Record<string, unknown> = {
  virtual_display_mode: 'disabled',
  virtual_display_layout: 'exclusive',
  dd_configuration_option: 'verify_only',
  dd_wa_virtual_double_refresh: true,
  min_threads: 2,
  nvenc_preset: 1,
  nvenc_realtime_hags: 'enabled',
  amd_quality: 'balanced',
  qsv_preset: 'medium',
};

const store = useConfigStore();
const { metadata, config } = storeToRefs(store);
const message = useMessage();
const { t: $t } = useI18n();

const busy = ref(false);
const hasSnapshot = ref(hasSnapshotInStorage());

const platform = computed(() => metadata.value?.platform || '');
const visible = computed(() => platform.value === 'windows');

const configReady = computed(() => {
  return !!config.value && typeof (config.value as any).virtual_display_mode !== 'undefined';
});

const vendor = computed<Vendor>(() => {
  const m = metadata.value || {};
  if (m.has_nvidia_gpu) return 'nvidia';
  if (m.has_amd_gpu) return 'amd';
  if (m.has_intel_gpu) return 'intel';
  return 'unknown';
});

function hpSettingsForVendor(v: Vendor): Record<string, unknown> {
  const base: Record<string, unknown> = { ...HP_BASE };
  if (v === 'nvidia') return { ...base, ...HP_NVIDIA };
  if (v === 'amd') return { ...base, ...HP_AMD };
  if (v === 'intel') return { ...base, ...HP_INTEL };
  return base;
}

function readVal(key: string): unknown {
  return (config.value as Record<string, unknown> | undefined)?.[key];
}

function valuesEqual(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (typeof a === 'boolean' || typeof b === 'boolean') {
    return toBool(a) === toBool(b);
  }
  return String(a) === String(b);
}

function toBool(v: unknown): boolean {
  if (typeof v === 'boolean') return v;
  if (typeof v === 'number') return v !== 0;
  if (typeof v === 'string') {
    const s = v.toLowerCase();
    return s === 'enabled' || s === 'true' || s === 'yes' || s === 'on' || s === '1';
  }
  return false;
}

const isEnabled = computed(() => {
  if (!configReady.value) return false;
  const hp = hpSettingsForVendor(vendor.value);
  for (const [k, v] of Object.entries(hp)) {
    if (!valuesEqual(readVal(k), v)) return false;
  }
  return true;
});

function applySettings(map: Record<string, unknown>) {
  for (const [k, v] of Object.entries(map)) {
    store.updateOption(k as any, v as any);
  }
}

function snapshotCurrent(v: Vendor): Record<string, unknown> {
  const keys = Object.keys(hpSettingsForVendor(v));
  const snap: Record<string, unknown> = {};
  for (const k of keys) {
    const cur = readVal(k);
    snap[k] = typeof cur === 'undefined' ? DEFAULTS[k] : cur;
  }
  return snap;
}

function saveSnapshot(snap: Record<string, unknown>, v: Vendor) {
  try {
    window.localStorage.setItem(
      SNAPSHOT_KEY,
      JSON.stringify({ vendor: v, savedAt: Date.now(), keys: snap }),
    );
    hasSnapshot.value = true;
  } catch {
    // localStorage may be unavailable; surface as a soft warning
    message.warning($t('high_perf.snapshot_save_failed'));
  }
}

function loadSnapshot(): { vendor: Vendor; keys: Record<string, unknown> } | null {
  try {
    const raw = window.localStorage.getItem(SNAPSHOT_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object' || !parsed.keys) return null;
    return { vendor: (parsed.vendor as Vendor) || 'unknown', keys: parsed.keys };
  } catch {
    return null;
  }
}

function clearSnapshot() {
  try {
    window.localStorage.removeItem(SNAPSHOT_KEY);
  } catch {
    // ignore
  }
  hasSnapshot.value = false;
}

function hasSnapshotInStorage(): boolean {
  try {
    return !!window.localStorage.getItem(SNAPSHOT_KEY);
  } catch {
    return false;
  }
}

async function onToggle(next: boolean) {
  if (busy.value || !configReady.value) return;
  busy.value = true;
  try {
    if (next) {
      const v = vendor.value;
      const snap = snapshotCurrent(v);
      saveSnapshot(snap, v);
      applySettings(hpSettingsForVendor(v));
      message.success($t('high_perf.applied'));
    } else {
      const snap = loadSnapshot();
      if (snap) {
        applySettings(snap.keys);
        message.success($t('high_perf.restored_previous'));
      } else {
        applySettings(restoreDefaultsFor(vendor.value));
        message.success($t('high_perf.restored_default'));
      }
    }
  } finally {
    busy.value = false;
  }
}

function restoreDefaultsFor(v: Vendor): Record<string, unknown> {
  const keys = Object.keys(hpSettingsForVendor(v));
  const out: Record<string, unknown> = {};
  for (const k of keys) out[k] = DEFAULTS[k];
  return out;
}

async function onRestorePrevious() {
  if (busy.value || !configReady.value) return;
  const snap = loadSnapshot();
  if (!snap) return;
  busy.value = true;
  try {
    applySettings(snap.keys);
    message.success($t('high_perf.restored_previous'));
  } finally {
    busy.value = false;
  }
}

async function onRestoreDefaults() {
  if (busy.value || !configReady.value) return;
  busy.value = true;
  try {
    applySettings(restoreDefaultsFor(vendor.value));
    clearSnapshot();
    message.success($t('high_perf.restored_default'));
  } finally {
    busy.value = false;
  }
}

onMounted(async () => {
  if (!configReady.value) {
    try {
      await store.fetchConfig();
    } catch {
      // Dashboard fetches separately; ignore
    }
  }
});
</script>
