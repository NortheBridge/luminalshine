<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const { metadata } = storeToRefs(store);
const config = store.config;
const platform = computed(() => (metadata.value?.platform || '').toString().toLowerCase());
</script>

<template>
  <div id="files" class="config-page">
    <ConfigFieldRenderer
      setting-key="file_apps"
      v-model="config.file_apps"
      class="mb-6"
      placeholder="apps.json"
    />

    <ConfigFieldRenderer
      setting-key="credentials_file"
      v-model="config.credentials_file"
      class="mb-6"
      placeholder="sunshine_state.json"
    />

    <ConfigFieldRenderer
      v-if="platform === 'windows'"
      setting-key="tpm_binding"
      v-model="config.tpm_binding"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="log_path"
      v-model="config.log_path"
      class="mb-6"
      placeholder="sunshine.log"
    />

    <ConfigFieldRenderer
      setting-key="pkey"
      v-model="config.pkey"
      class="mb-6"
      placeholder="/dir/pkey.pem"
    />

    <ConfigFieldRenderer
      setting-key="cert"
      v-model="config.cert"
      class="mb-6"
      placeholder="/dir/cert.pem"
    />

    <ConfigFieldRenderer
      setting-key="file_state"
      v-model="config.file_state"
      class="mb-6"
      placeholder="sunshine_state.json"
    />
  </div>
</template>

<style scoped></style>
