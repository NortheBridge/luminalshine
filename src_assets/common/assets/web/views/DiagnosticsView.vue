<script setup lang="ts">
/**
 * Diagnostics — Troubleshooting, Logs and About merged into one page with a
 * section switcher. `?sec=about` opens the host identity report; anything
 * else shows health, actions and the log viewer. `#logs` still deep-links
 * to the log viewer.
 */
import { computed, nextTick, onMounted, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { NCard } from 'naive-ui';
import { useT2 } from '@/composables/useT2';
import Troubleshooting from '@/Troubleshooting.vue';
import AboutView from '@/views/AboutView.vue';
import ResourceCard from '@/ResourceCard.vue';

const route = useRoute();
const router = useRouter();
const t2 = useT2();

type Section = 'health' | 'about';
const section = computed<Section>(() => (route.query['sec'] === 'about' ? 'about' : 'health'));

function go(sec: Section): void {
  const query = { ...route.query };
  if (sec === 'about') query['sec'] = 'about';
  else delete query['sec'];
  void router.replace({ path: route.path, query, hash: sec === 'health' ? route.hash : '' });
}

function scrollToHash(): void {
  const hash = route.hash;
  if (!hash || section.value !== 'health') return;
  void nextTick(() => {
    try {
      document.querySelector(hash)?.scrollIntoView({ block: 'start' });
    } catch {
      /* invalid selector; ignore */
    }
  });
}

onMounted(scrollToHash);
watch(() => route.hash, scrollToHash);
</script>

<template>
  <div class="space-y-4">
    <div class="flex flex-wrap items-center gap-3">
      <div>
        <div class="mc-page-title">{{ t2('shell.nav_diagnostics', 'Diagnostics') }}</div>
        <div class="mc-page-sub">
          {{ t2('diagnostics.subtitle', 'Health, actions, logs and this host’s identity') }}
        </div>
      </div>
      <div class="flex-1"></div>
      <div class="mc-chips" role="tablist">
        <span
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'health' }"
          role="tab"
          :aria-selected="section === 'health'"
          @click="go('health')"
        >
          {{ t2('diagnostics.section_health', 'Health & logs') }}
        </span>
        <span
          class="mc-chip"
          :class="{ 'mc-chip-on': section === 'about' }"
          role="tab"
          :aria-selected="section === 'about'"
          @click="go('about')"
        >
          {{ t2('diagnostics.section_about', 'About this host') }}
        </span>
      </div>
    </div>

    <Troubleshooting v-if="section === 'health'" />
    <template v-else>
      <AboutView />
      <NCard :title="t2('resources.title', 'Web links')">
        <div class="space-y-2 text-xs">
          <ResourceCard />
        </div>
      </NCard>
    </template>
  </div>
</template>
