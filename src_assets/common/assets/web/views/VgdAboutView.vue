<template>
  <div class="space-y-6">
    <div>
      <h1 class="text-2xl font-semibold tracking-tight text-dark dark:text-light">
        {{ t('vgd.about_title') }}
      </h1>
      <p class="text-xs opacity-70 leading-snug mt-1">
        {{ t('vgd.about_subtitle') }}
      </p>
    </div>

    <n-alert type="warning" :show-icon="true">
      <span class="font-semibold">{{ t('vgd.proto_notice') }}</span>
      <i18n-t keypath="vgd.proto_visit" tag="span" class="block mt-0.5">
        <template #link>
          <a
            :href="VGD_SITE_URL"
            target="_blank"
            rel="noopener noreferrer"
            class="underline underline-offset-2 hover:opacity-80 break-all"
            >{{ VGD_SITE_URL }}</a
          >
        </template>
      </i18n-t>
    </n-alert>

    <div class="grid grid-cols-1 lg:grid-cols-2 gap-6">
      <!-- Driver identity: static design-document facts until the driver
           ships and /api/metadata starts carrying live handshake data. -->
      <section class="rounded-xl border border-white/10 bg-white/[0.03] p-4 sm:p-5 space-y-3">
        <div class="flex items-center gap-2">
          <i class="fas fa-display text-primary" />
          <h2 class="text-base font-semibold text-dark dark:text-light">
            {{ t('vgd.about_driver_card') }}
          </h2>
        </div>
        <dl class="space-y-2 text-sm">
          <div v-for="row in driverRows" :key="row.label" class="flex justify-between gap-4">
            <dt class="opacity-70">{{ row.label }}</dt>
            <dd class="font-mono text-right break-all">{{ row.value }}</dd>
          </div>
        </dl>
      </section>

      <section class="rounded-xl border border-white/10 bg-white/[0.03] p-4 sm:p-5 space-y-3">
        <div class="flex items-center gap-2">
          <i class="fas fa-link text-primary" />
          <h2 class="text-base font-semibold text-dark dark:text-light">
            {{ t('vgd.about_host_card') }}
          </h2>
        </div>
        <dl class="space-y-2 text-sm">
          <div v-for="row in hostRows" :key="row.label" class="flex justify-between gap-4">
            <dt class="opacity-70">{{ row.label }}</dt>
            <dd class="font-mono text-right break-all">{{ row.value }}</dd>
          </div>
        </dl>
      </section>
    </div>

    <section class="rounded-xl border border-white/10 bg-white/[0.03] p-4 sm:p-5 space-y-3">
      <div class="flex items-center justify-between gap-4 flex-wrap">
        <div class="flex items-center gap-2">
          <i class="fas fa-cloud-arrow-down text-primary" />
          <h2 class="text-base font-semibold text-dark dark:text-light">
            {{ tr('vgd.about_updates_card', 'Updates') }}
          </h2>
        </div>
        <div class="flex items-center gap-2">
          <a
            :href="VGD_SITE_URL"
            target="_blank"
            rel="noopener noreferrer"
            class="text-xs underline underline-offset-2 opacity-80 hover:opacity-100"
          >
            {{ t('vgd.about_official_site') }}
          </a>
          <n-button
            type="primary"
            size="small"
            :loading="updateState === 'checking'"
            :disabled="updateState === 'checking'"
            @click="checkForUpdates"
          >
            {{ t('vgd.about_check_updates') }}
          </n-button>
        </div>
      </div>
      <div class="min-h-[1.5rem] text-sm">
        <span v-if="updateState === 'checking'" class="opacity-70">
          {{ t('vgd.about_checking') }}
        </span>
        <n-alert v-else-if="updateState === 'none'" type="info" :show-icon="true">
          {{ t('vgd.about_no_release') }}
        </n-alert>
        <n-alert v-else-if="updateState === 'found'" type="success" :show-icon="true">
          <a
            :href="latestRelease.url"
            target="_blank"
            rel="noopener noreferrer"
            class="underline underline-offset-2"
          >
            {{ t('vgd.about_update_found', { tag: latestRelease.tag, date: latestRelease.date }) }}
          </a>
        </n-alert>
        <n-alert v-else-if="updateState === 'error'" type="error" :show-icon="true">
          {{ t('vgd.about_update_error') }}
        </n-alert>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { storeToRefs } from 'pinia';
import { NAlert, NButton } from 'naive-ui';
import { useConfigStore } from '@/stores/config';

const { t } = useI18n();
const VGD_SITE_URL = 'https://apps.northebridge.com/en/LuminalVGD';
// Public releases feed of the driver repo; returns 404 until the first
// release ships, which the UI reports as "still in prototype".
const VGD_RELEASES_API = 'https://api.github.com/repos/NortheBridge/LuminalVGD/releases/latest';

const tr = (key: string, fallback: string) => {
  const value = t(key);
  return value === key ? fallback : value;
};

const store = useConfigStore();
const { metadata } = storeToRefs(store);

const driverRows = computed(() => [
  {
    label: tr('vgd.about_status', 'Status'),
    value: tr('vgd.about_status_prototype', 'Prototype (pre-development)'),
  },
  {
    label: tr('vgd.about_driver_version', 'Driver version'),
    value: tr('vgd.about_not_installed', 'Not installed'),
  },
  { label: tr('vgd.about_protocol', 'Design protocol'), value: 'v0.3' },
  {
    label: tr('vgd.about_device', 'Device'),
    value: 'Luminal Video Graphics Display (root\\luminal_vgd)',
  },
  { label: tr('vgd.about_provider', 'Provider'), value: 'NortheBridge Foundation' },
]);

const hostRows = computed(() => {
  const md = (metadata.value || {}) as { version?: string; release_date?: string };
  return [
    {
      label: tr('vgd.about_this_host', 'This LuminalShine'),
      value: md?.version || '—',
    },
    {
      label: tr('vgd.about_host_built', 'LuminalShine built'),
      value: md?.release_date || '—',
    },
    { label: tr('vgd.about_shipped_with', 'Driver shipped with LuminalShine'), value: '—' },
    { label: tr('vgd.about_driver_built', 'Driver built'), value: '—' },
    { label: tr('vgd.about_signature', 'Driver signature'), value: '—' },
  ];
});

type UpdateState = 'idle' | 'checking' | 'none' | 'found' | 'error';
const updateState = ref<UpdateState>('idle');
// Only meaningful while updateState === 'found'; the state machine gates it.
const latestRelease = ref({ tag: '', date: '', url: '' });

async function checkForUpdates(): Promise<void> {
  updateState.value = 'checking';
  try {
    // Native fetch on purpose: the app's axios instance sends credentials
    // and custom headers, which cross-origin GitHub rejects under CORS.
    const res = await fetch(VGD_RELEASES_API, {
      headers: { Accept: 'application/vnd.github+json' },
    });
    if (res.status === 404) {
      updateState.value = 'none';
      return;
    }
    if (!res.ok) {
      updateState.value = 'error';
      return;
    }
    const data: unknown = await res.json();
    const rel = data as { tag_name?: unknown; published_at?: unknown; html_url?: unknown };
    if (typeof rel.tag_name === 'string' && rel.tag_name) {
      latestRelease.value = {
        tag: rel.tag_name,
        date:
          typeof rel.published_at === 'string' && rel.published_at
            ? rel.published_at.slice(0, 10)
            : '?',
        url: typeof rel.html_url === 'string' ? rel.html_url : VGD_SITE_URL,
      };
      updateState.value = 'found';
      return;
    }
    updateState.value = 'none';
  } catch {
    updateState.value = 'error';
  }
}
</script>
