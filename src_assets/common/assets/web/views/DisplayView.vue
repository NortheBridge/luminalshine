<script setup lang="ts">
/**
 * Display — the LuminalVGD control panel and the driver's About page under
 * one destination. `?sec=about` opens the driver page.
 */
import { computed } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { useT2 } from '@/composables/useT2';
import VgdControlPanelView from '@/views/VgdControlPanelView.vue';
import VgdAboutView from '@/views/VgdAboutView.vue';

const route = useRoute();
const router = useRouter();
const t2 = useT2();

type Section = 'panel' | 'about';
const section = computed<Section>(() => (route.query['sec'] === 'about' ? 'about' : 'panel'));

function go(sec: Section): void {
  const query = { ...route.query };
  if (sec === 'about') query['sec'] = 'about';
  else delete query['sec'];
  void router.replace({ path: route.path, query });
}
</script>

<template>
  <div class="space-y-4">
    <div class="flex flex-wrap items-center gap-3">
      <div>
        <div class="mc-page-title">{{ t2('shell.nav_display', 'Display') }}</div>
        <div class="mc-page-sub">
          {{ t2('display.subtitle', 'LuminalVGD virtual display and display configuration') }}
        </div>
      </div>
      <div class="flex-1"></div>
      <div class="mc-chips" role="tablist">
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'panel' }"
          role="tab"
          :aria-selected="section === 'panel'"
          @click="go('panel')"
        >
          {{ t2('vgd.nav_control_panel', 'Control panel') }}
        </button>
        <button
          type="button"
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'about' }"
          role="tab"
          :aria-selected="section === 'about'"
          @click="go('about')"
        >
          {{ t2('vgd.nav_about', 'About the driver') }}
        </button>
      </div>
    </div>

    <VgdControlPanelView v-if="section === 'panel'" />
    <VgdAboutView v-else />
  </div>
</template>
