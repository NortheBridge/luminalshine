<script setup lang="ts">
/**
 * Stream — live telemetry and session history. Phase 1 hosts the existing
 * session list and details drawer; the chart layout from the Mission
 * Control canvas lands in a later phase. This is also the whole UI for the
 * stats-only role.
 */
import { useT2 } from '@/composables/useT2';
import SessionHistoryCard from '@/components/SessionHistoryCard.vue';

const t2 = useT2();
</script>

<template>
  <div class="space-y-4">
    <div class="flex flex-wrap items-center gap-3">
      <div>
        <div class="mc-page-title">{{ t2('shell.nav_stream', 'Stream') }}</div>
        <div class="mc-page-sub">
          {{ t2('stream.subtitle', 'Live telemetry and every recorded session') }}
        </div>
      </div>
    </div>
    <SessionHistoryCard full-width-panel />
  </div>
</template>
