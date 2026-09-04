<script setup lang="ts">
/**
 * Contextual inspector — the 340px right-hand column of the Mission Control
 * shell. Pages mount one of these with the settings or details that belong
 * to whatever is selected; the shell shows the column while any panel is
 * mounted and gives the width back when the page has none.
 */
import { onBeforeUnmount, onMounted } from 'vue';
import { INSPECTOR_TARGET_ID, useInspector } from '@/composables/useInspector';

withDefaults(
  defineProps<{
    title: string;
    subtitle?: string;
    tag?: string;
    /** Visual class for the header tag: '', 'ok', 'live', 'warn', 'danger'. */
    tagKind?: '' | 'ok' | 'live' | 'warn' | 'danger';
  }>(),
  { subtitle: '', tag: '', tagKind: '' },
);

const { attach, detach } = useInspector();
onMounted(attach);
onBeforeUnmount(detach);
</script>

<template>
  <Teleport :to="`#${INSPECTOR_TARGET_ID}`" defer>
    <div class="flex h-full min-h-0 flex-col">
      <div
        class="flex items-center justify-between gap-2.5 border-b border-line px-[18px] pb-2.5 pt-3.5"
      >
        <div class="min-w-0">
          <div class="text-[13px] font-semibold text-ink">{{ title }}</div>
          <div v-if="subtitle" class="mt-0.5 text-[11px] text-ink-4">{{ subtitle }}</div>
        </div>
        <span v-if="tag" class="mc-tag" :class="tagKind ? `mc-tag-${tagKind}` : ''">{{ tag }}</span>
      </div>
      <div class="app-scrollbar flex min-h-0 flex-1 flex-col overflow-y-auto">
        <slot />
      </div>
      <div
        v-if="$slots['footer']"
        class="flex items-center justify-between gap-2.5 border-t border-line px-[18px] py-3"
      >
        <slot name="footer" />
      </div>
    </div>
  </Teleport>
</template>
