<script setup lang="ts">
/**
 * Single-series time chart on uPlot, styled for Mission Control: one hue,
 * a 2px line, recessive grid, crosshair + value readout on hover, and a
 * width that follows its container.
 */
import { onBeforeUnmount, onMounted, ref, watch } from 'vue';
import uPlot, { type AlignedData, type Options } from 'uplot';
import 'uplot/dist/uPlot.min.css';

const props = withDefaults(
  defineProps<{
    /** [epoch seconds, value] pairs in ascending time order. */
    points: Array<[number, number]>;
    label: string;
    unit?: string;
    color?: string;
    height?: number;
    /** Fixed y-range bounds; NaN (the default) follows the data, with min never above 0. */
    min?: number;
    max?: number;
    decimals?: number;
  }>(),
  { unit: '', color: '#FFB020', height: 120, min: Number.NaN, max: Number.NaN, decimals: 1 },
);

const host = ref<HTMLDivElement | null>(null);
let chart: uPlot | null = null;
let observer: ResizeObserver | null = null;

const MONO = '10.5px "IBM Plex Mono", Consolas, "Courier New", monospace';

function fmt(v: number | null | undefined): string {
  if (v == null || !Number.isFinite(v)) return '—';
  return Number.isInteger(v) ? String(v) : v.toFixed(props.decimals);
}

function toData(): AlignedData {
  const xs: number[] = [];
  const ys: number[] = [];
  for (const [t, v] of props.points) {
    xs.push(t);
    ys.push(v);
  }
  return [xs, ys];
}

function destroy(): void {
  if (chart) {
    chart.destroy();
    chart = null;
  }
}

function build(): void {
  destroy();
  const el = host.value;
  if (!el) return;
  const width = Math.max(120, el.clientWidth);
  const opts: Options = {
    width,
    height: props.height,
    class: 'mc-uplot',
    cursor: { points: { size: 7, fill: props.color, stroke: '#14171B' }, y: false },
    legend: { show: true, live: true },
    scales: {
      x: { time: true },
      y: {
        range: (_u, dataMin, dataMax) => {
          const lo = Number.isFinite(props.min) ? props.min : Math.min(0, dataMin);
          const hi = Number.isFinite(props.max)
            ? props.max
            : dataMax > lo
              ? dataMax + (dataMax - lo) * 0.08
              : lo + 1;
          return [lo, hi];
        },
      },
    },
    axes: [
      {
        stroke: '#6F757C',
        font: MONO,
        grid: { stroke: 'rgba(255,255,255,0.06)', width: 1 },
        ticks: { show: false },
        space: 80,
      },
      {
        stroke: '#6F757C',
        font: MONO,
        grid: { stroke: 'rgba(255,255,255,0.06)', width: 1 },
        ticks: { show: false },
        size: 46,
        splits: (_u, _axisIdx, scaleMin, scaleMax) => {
          const mid = (scaleMin + scaleMax) / 2;
          return [scaleMin, mid, scaleMax];
        },
        values: (_u, splits) => splits.map((v) => fmt(v)),
      },
    ],
    series: [
      { label: 'Time' },
      {
        label: props.label,
        stroke: props.color,
        width: 2,
        points: { show: false },
        value: (_u, v) => (v == null ? '—' : `${fmt(v)}${props.unit ? ` ${props.unit}` : ''}`),
      },
    ],
  };
  chart = new uPlot(opts, toData(), el);
}

watch(
  () => props.points,
  () => {
    if (chart) chart.setData(toData());
    else build();
  },
);

onMounted(() => {
  build();
  if (typeof ResizeObserver !== 'undefined' && host.value) {
    observer = new ResizeObserver(() => {
      if (chart && host.value) {
        chart.setSize({ width: Math.max(120, host.value.clientWidth), height: props.height });
      }
    });
    observer.observe(host.value);
  }
});

onBeforeUnmount(() => {
  observer?.disconnect();
  observer = null;
  destroy();
});
</script>

<template>
  <div ref="host" class="mc-chart w-full"></div>
</template>

<style scoped>
.mc-chart :deep(.u-legend) {
  font-family: 'IBM Plex Mono', Consolas, 'Courier New', monospace;
  font-size: 11px;
  color: var(--mc-ink-3);
  text-align: left;
  padding: 2px 0 0;
}
.mc-chart :deep(.u-legend .u-series:first-child) {
  display: none;
}
.mc-chart :deep(.u-legend .u-marker) {
  border-radius: 2px;
}
.mc-chart :deep(.u-legend .u-value) {
  color: var(--mc-ink);
}
.mc-chart :deep(.u-select) {
  background: rgba(255, 176, 32, 0.12);
}
</style>
