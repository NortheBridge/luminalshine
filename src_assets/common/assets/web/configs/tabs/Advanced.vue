<script setup lang="ts">
import { useI18n } from 'vue-i18n';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const { t } = useI18n();
const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div class="config-page">
    <ConfigFieldRenderer
      v-model="config.fec_percentage"
      setting-key="fec_percentage"
      class="mb-6"
    />

    <ConfigFieldRenderer v-model="config.qp" setting-key="qp" class="mb-6" />

    <ConfigFieldRenderer v-model="config.min_threads" setting-key="min_threads" class="mb-6" />

    <ConfigFieldRenderer v-model="config.hevc_mode" setting-key="hevc_mode" class="mb-6" />

    <ConfigFieldRenderer v-model="config.av1_mode" setting-key="av1_mode" class="mb-6" />
    <ConfigFieldRenderer
      v-model="config.pyrowave_enabled"
      setting-key="pyrowave_enabled"
      class="mb-6"
    />
    <div v-if="config.pyrowave_enabled" class="mb-6">
      <ConfigFieldRenderer
        v-model="config.pyrowave_bitrate_mbps"
        setting-key="pyrowave_bitrate_mbps"
      />
      <input
        v-model.number="config.pyrowave_bitrate_mbps"
        type="range"
        min="0"
        max="10000"
        step="1"
        :aria-label="t('config.pyrowave_bitrate_mbps')"
        class="mt-3 w-full accent-primary"
      />
    </div>
  </div>
</template>

<style scoped></style>
