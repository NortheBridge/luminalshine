<script setup lang="ts">
import { computed } from 'vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;

// Family-share sub-toggle only carries meaning when the master sync
// is on — disable it visually so the user understands the dependency.
const familyShareDisabled = computed(() => config.steam_auto_sync !== true);
</script>

<template>
  <div id="steamlibrary" class="config-page">
    <ConfigFieldRenderer
      setting-key="steam_auto_sync"
      v-model="config.steam_auto_sync"
      class="mb-6"
    />

    <ConfigFieldRenderer
      setting-key="steam_include_family_shared"
      v-model="config.steam_include_family_shared"
      :disabled="familyShareDisabled"
      class="mb-6"
    />
  </div>
</template>

<style scoped></style>
