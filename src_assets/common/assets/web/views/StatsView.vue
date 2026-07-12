<template>
  <div class="space-y-6">
    <div class="flex items-start justify-between gap-4 flex-wrap">
      <div>
        <h1 class="text-2xl font-semibold tracking-tight text-dark dark:text-light">
          {{ t('stats.title') }}
        </h1>
        <p class="text-xs opacity-70 leading-snug mt-1">
          {{ t('stats.subtitle') }}
        </p>
      </div>
    </div>
    <SessionHistoryCard />
  </div>
</template>

<script setup lang="ts">
import { useI18n } from 'vue-i18n';
import SessionHistoryCard from '@/components/SessionHistoryCard.vue';

const { t } = useI18n();
</script>
