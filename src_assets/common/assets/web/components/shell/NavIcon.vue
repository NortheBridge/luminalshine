<script setup lang="ts">
/**
 * Stroke icon set for the Mission Control shell. Drawn inline (no icon
 * font) so icons scale, recolor with `currentColor`, and share one style:
 * 24-unit grid, 1.7px round-capped strokes.
 */
import { computed } from 'vue';

type Shape = { tag: 'path' | 'rect' | 'circle'; attrs: Record<string, string> };

const ICONS: Record<string, Shape[]> = {
  overview: [
    { tag: 'rect', attrs: { x: '3.5', y: '3.5', width: '17', height: '17', rx: '2.5' } },
    { tag: 'path', attrs: { d: 'M3.5 9.5h17M9.5 9.5v11' } },
  ],
  stream: [
    { tag: 'circle', attrs: { cx: '12', cy: '12', r: '2' } },
    {
      tag: 'path',
      attrs: {
        d: 'M7.5 7.5a6.4 6.4 0 0 0 0 9M16.5 7.5a6.4 6.4 0 0 1 0 9M4.6 4.6a10.5 10.5 0 0 0 0 14.8M19.4 4.6a10.5 10.5 0 0 1 0 14.8',
      },
    },
  ],
  play: [
    {
      tag: 'path',
      attrs: { d: 'M7.5 5.2v13.6a1 1 0 0 0 1.5.9l10.6-6.8a1 1 0 0 0 0-1.7L9 4.3a1 1 0 0 0-1.5.9z' },
    },
  ],
  library: [
    { tag: 'rect', attrs: { x: '3.5', y: '3.5', width: '7', height: '7', rx: '1.5' } },
    { tag: 'rect', attrs: { x: '13.5', y: '3.5', width: '7', height: '7', rx: '1.5' } },
    { tag: 'rect', attrs: { x: '3.5', y: '13.5', width: '7', height: '7', rx: '1.5' } },
    { tag: 'rect', attrs: { x: '13.5', y: '13.5', width: '7', height: '7', rx: '1.5' } },
  ],
  clients: [
    { tag: 'circle', attrs: { cx: '9', cy: '8', r: '3.2' } },
    {
      tag: 'path',
      attrs: {
        d: 'M3.5 19.5a5.5 5.5 0 0 1 11 0M16 4.6a3.2 3.2 0 0 1 0 6.4M17.5 14.2a5.5 5.5 0 0 1 3 5.3',
      },
    },
  ],
  display: [
    { tag: 'rect', attrs: { x: '3', y: '4.5', width: '18', height: '12', rx: '2' } },
    { tag: 'path', attrs: { d: 'M8 20h8M12 16.5V20' } },
  ],
  settings: [
    { tag: 'path', attrs: { d: 'M4 7h8M18 7h2M4 12h2M12 12h8M4 17h6M16 17h4' } },
    { tag: 'circle', attrs: { cx: '15', cy: '7', r: '2.2' } },
    { tag: 'circle', attrs: { cx: '9', cy: '12', r: '2.2' } },
    { tag: 'circle', attrs: { cx: '13', cy: '17', r: '2.2' } },
  ],
  diagnostics: [{ tag: 'path', attrs: { d: 'M3 12h4l3-7 4 14 3-7h4' } }],
  logout: [
    { tag: 'path', attrs: { d: 'M10 4H6a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h4M15 8l4 4-4 4M19 12H9' } },
  ],
  search: [
    { tag: 'circle', attrs: { cx: '11', cy: '11', r: '6.5' } },
    { tag: 'path', attrs: { d: 'M16 16l4 4' } },
  ],
  chevron: [{ tag: 'path', attrs: { d: 'M6 9l6 6 6-6' } }],
  'chevron-right': [{ tag: 'path', attrs: { d: 'M9 6l6 6-6 6' } }],
  check: [{ tag: 'path', attrs: { d: 'M5 12l4 4 10-10' } }],
  plus: [{ tag: 'path', attrs: { d: 'M12 5v14M5 12h14' } }],
  close: [{ tag: 'path', attrs: { d: 'M6 6l12 12M18 6L6 18' } }],
  copy: [
    { tag: 'rect', attrs: { x: '9', y: '9', width: '11', height: '11', rx: '2' } },
    { tag: 'path', attrs: { d: 'M5 15V5a2 2 0 0 1 2-2h10' } },
  ],
  download: [{ tag: 'path', attrs: { d: 'M12 4v12M6 11l6 6 6-6M4 20h16' } }],
  refresh: [{ tag: 'path', attrs: { d: 'M20 12a8 8 0 1 1-2.3-5.7M20 4v5h-5' } }],
  power: [{ tag: 'path', attrs: { d: 'M12 3v9M6.3 6.8a8 8 0 1 0 11.4 0' } }],
  warning: [
    { tag: 'path', attrs: { d: 'M12 3.5l9.5 17h-19z' } },
    { tag: 'path', attrs: { d: 'M12 9.5v4.5M12 17.2v.3' } },
  ],
  sync: [
    {
      tag: 'path',
      attrs: { d: 'M4 12a8 8 0 0 1 13.7-5.7M20 12a8 8 0 0 1-13.7 5.7M17 3v4h-4M7 21v-4h4' },
    },
  ],
  menu: [{ tag: 'path', attrs: { d: 'M4 7h16M4 12h16M4 17h16' } }],
  external: [
    {
      tag: 'path',
      attrs: { d: 'M14 4h6v6M20 4l-9 9M18 13v6a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h6' },
    },
  ],
};

const props = withDefaults(
  defineProps<{
    name: string;
    size?: number;
  }>(),
  { size: 20 },
);

const shapes = computed(() => ICONS[props.name] ?? []);
const px = computed(() => `${props.size}px`);
</script>

<template>
  <svg
    viewBox="0 0 24 24"
    :style="{ width: px, height: px }"
    fill="none"
    stroke="currentColor"
    stroke-width="1.7"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    class="shrink-0"
  >
    <component :is="shape.tag" v-for="(shape, i) in shapes" :key="i" v-bind="shape.attrs" />
  </svg>
</template>
