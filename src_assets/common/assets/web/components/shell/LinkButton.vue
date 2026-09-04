<script setup lang="ts">
/**
 * A Naive button that navigates like a router link: renders one <a> (no
 * <button> nested inside an <a>), so it is a single tab stop, supports
 * middle-click / open-in-new-tab, and still uses client-side navigation.
 */
import { NButton } from 'naive-ui';
import type { RouteLocationRaw } from 'vue-router';

withDefaults(
  defineProps<{
    to: RouteLocationRaw;
    size?: 'tiny' | 'small' | 'medium' | 'large';
    type?: 'default' | 'primary' | 'info' | 'success' | 'warning' | 'error';
  }>(),
  { size: 'small', type: 'default' },
);
</script>

<template>
  <RouterLink v-slot="{ href, navigate }" :to="to" custom>
    <NButton tag="a" :href="href" :size="size" :type="type" @click="navigate">
      <slot />
    </NButton>
  </RouterLink>
</template>
