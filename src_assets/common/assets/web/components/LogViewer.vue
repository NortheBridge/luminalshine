<script setup lang="ts">
/**
 * Log viewer — the host's log files with a source selector, a chunked
 * case-insensitive search with context snippets, follow-the-tail auto-scroll
 * and a "new lines" affordance. Extracted from the old Troubleshooting page;
 * the logic is unchanged, the chrome is Mission Control.
 */
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { NButton, NInput, NScrollbar, NSelect } from 'naive-ui';
import { storeToRefs } from 'pinia';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import { useT2 } from '@/composables/useT2';
import NavIcon from '@/components/shell/NavIcon.vue';

const props = withDefaults(defineProps<{ height?: number }>(), { height: 520 });

const t2 = useT2();
const authStore = useAuthStore();
const configStore = useConfigStore();
const { metadata } = storeToRefs(configStore);
const platform = computed(() => String(metadata.value?.platform || '').toLowerCase());

const latestLogs = ref('Loading...');
const displayedLogs = ref('Loading...');
const logFilter = ref('');
const searchTerm = ref('');
const logSource = ref('sunshine');
type LogSegment = { text: string; isMatch: boolean };
const matchLines = ref<number[]>([]);
const matchCount = ref(0);
const searchInProgress = ref(false);
const searchResultLimit = 200;
const searchChunkSize = 1000;
let searchTaskId = 0;
let searchTaskTimer: number | null = null;
const segmentCache = new Map<number, { term: string; text: string; segments: LogSegment[] }>();

const tCount = (key: string, fallback: string, count: number) => {
  const value = t2(key, '');
  return value
    ? value.replace('{count}', String(count))
    : fallback.replace('{count}', String(count));
};

const logSourceOptions = computed(() => {
  const options = [
    { label: t2('troubleshooting.logs_source_sunshine', 'LuminalShine'), value: 'sunshine' },
  ];
  if (platform.value === 'windows') {
    options.push(
      {
        label: t2('troubleshooting.logs_source_display_helper', 'Display helper'),
        value: 'display_helper',
      },
      { label: t2('troubleshooting.logs_source_playnite', 'Playnite'), value: 'playnite' },
      {
        label: t2('troubleshooting.logs_source_playnite_launcher', 'Playnite launcher'),
        value: 'playnite_launcher',
      },
      { label: t2('troubleshooting.logs_source_wgc', 'WGC helper'), value: 'wgc' },
    );
  }
  return options;
});

const logScrollbar = ref<InstanceType<typeof NScrollbar> | null>(null);
const autoScrollEnabled = ref(true);
const latestLineCount = ref(0);
const displayedLineCount = ref(0);
const isAtBottom = ref(true);

let logInterval: number | null = null;
let loginDisposer: (() => void) | null = null;
let searchDebounce: number | null = null;

const lineRefs = new Map<number, HTMLElement>();
const resultRefs = new Map<number, HTMLElement>();
const pendingJumpLine = ref<number | null>(null);

const setLineRef = (index: number) => (el: unknown) => {
  if (el instanceof HTMLElement) lineRefs.set(index, el);
  else lineRefs.delete(index);
};
const setResultRef = (index: number) => (el: unknown) => {
  if (el instanceof HTMLElement) resultRefs.set(index, el);
  else resultRefs.delete(index);
};

const rawSearch = computed(() => logFilter.value.trim());
const rawSearchActive = computed(() => rawSearch.value.length > 0);
const searchActive = computed(() => searchTerm.value.length > 0);
const logLines = computed(() => (displayedLogs.value ?? '').split('\n'));
const logLinesLower = computed(() => logLines.value.map((line) => line.toLowerCase()));
const lineNumberWidth = computed(() => {
  const digits = Math.max(3, String(logLines.value.length || 0).length);
  return `${digits}ch`;
});
const contextLines = 5;
const activeMatchIndex = ref(-1);
const activeLineIndex = computed(() =>
  activeMatchIndex.value >= 0 ? (matchLines.value[activeMatchIndex.value] ?? null) : null,
);

const searchPending = computed(
  () => rawSearchActive.value && (rawSearch.value !== searchTerm.value || searchInProgress.value),
);
const matchCountLabel = computed(() => {
  if (!rawSearchActive.value) return '';
  if (searchPending.value) return t2('troubleshooting.search_pending', 'Searching...');
  if (matchCount.value === 0) return t2('troubleshooting.search_no_matches', 'No matches');
  return tCount('troubleshooting.search_matches', '{count} matches', matchCount.value);
});
const searchContextLabel = computed(() =>
  tCount('troubleshooting.search_context', '{count} lines of context', contextLines),
);

const resultsWindow = computed(() => {
  const total = matchLines.value.length;
  if (total <= searchResultLimit) return { start: 0, end: total };
  let start = 0;
  if (activeMatchIndex.value >= 0) {
    const half = Math.floor(searchResultLimit / 2);
    start = Math.max(
      0,
      Math.min(activeMatchIndex.value - half, Math.max(0, total - searchResultLimit)),
    );
  }
  return { start, end: Math.min(total, start + searchResultLimit) };
});
const resultsRangeLabel = computed(() => {
  const total = matchLines.value.length;
  if (total <= searchResultLimit) return '';
  const { start, end } = resultsWindow.value;
  return `${t2('troubleshooting.search_showing', 'Showing')} ${start + 1}-${end} ${t2('overview.of', 'of')} ${total}`;
});

const searchResults = computed(() => {
  if (!searchActive.value || searchInProgress.value) return [];
  const lines = logLines.value;
  const { start: windowStart, end: windowEnd } = resultsWindow.value;
  return matchLines.value.slice(windowStart, windowEnd).map((lineIndex, offset) => {
    const id = windowStart + offset;
    const snippetStart = Math.max(0, lineIndex - contextLines);
    const snippetEnd = Math.min(lines.length - 1, lineIndex + contextLines);
    const snippet: Array<{ lineIndex: number; text: string }> = [];
    for (let i = snippetStart; i <= snippetEnd; i += 1)
      snippet.push({ lineIndex: i, text: lines[i] ?? '' });
    return { id, lineIndex, snippet };
  });
});

function getLineSegments(line: string, lineIndex: number): LogSegment[] {
  const term = searchTerm.value.trim();
  if (!term) return [{ text: line.length === 0 ? ' ' : line, isMatch: false }];
  const cached = segmentCache.get(lineIndex);
  if (cached && cached.term === term && cached.text === line) return cached.segments;
  const needle = term.toLowerCase();
  const lower = line.toLowerCase();
  const segments: LogSegment[] = [];
  let cursor = 0;
  let matchIndex = lower.indexOf(needle, cursor);
  while (matchIndex !== -1) {
    if (matchIndex > cursor)
      segments.push({ text: line.slice(cursor, matchIndex), isMatch: false });
    segments.push({ text: line.slice(matchIndex, matchIndex + needle.length), isMatch: true });
    cursor = matchIndex + needle.length;
    matchIndex = lower.indexOf(needle, cursor);
  }
  if (cursor < line.length) segments.push({ text: line.slice(cursor), isMatch: false });
  if (segments.length === 0)
    segments.push({ text: line.length === 0 ? ' ' : line, isMatch: false });
  segmentCache.set(lineIndex, { term, text: line, segments });
  return segments;
}

/** Tint by log level, parsed from the "[ts]: Level:" prefix. */
function levelClass(line: string): string {
  const m = /^\[[^\]]+\]:\s*(\w+):/.exec(line);
  const level = m?.[1]?.toLowerCase();
  if (level === 'fatal' || level === 'error') return 'log-level-error';
  if (level === 'warning' || level === 'warn') return 'log-level-warn';
  if (level === 'debug' || level === 'verbose') return 'log-level-dim';
  return '';
}

const unseenLines = computed(() => Math.max(0, latestLineCount.value - displayedLineCount.value));
const newLogsAvailable = computed(() => unseenLines.value > 0);
const showJumpToLatest = computed(
  () => !newLogsAvailable.value && !isAtBottom.value && !autoScrollEnabled.value,
);

function resetLogState() {
  latestLogs.value = 'Loading...';
  displayedLogs.value = 'Loading...';
  latestLineCount.value = 0;
  displayedLineCount.value = 0;
  autoScrollEnabled.value = true;
  isAtBottom.value = true;
}

function buildLogUrl(): string {
  if (logSource.value === 'sunshine') return './api/logs';
  const params = new URLSearchParams();
  params.set('source', logSource.value);
  return `./api/logs?${params.toString()}`;
}

function getLogContainer(): HTMLElement | null {
  // Naive UI's <n-scrollbar> exposes `scrollbarInstRef` (a Vue ref) which is
  // sometimes auto-unwrapped by the component public instance proxy.
  interface ScrollbarInternals {
    containerRef?: HTMLElement;
  }
  interface ScrollbarHandle {
    scrollbarInstRef?: ScrollbarInternals | { value?: ScrollbarInternals };
    $el?: HTMLElement;
  }
  const inst = logScrollbar.value as unknown as ScrollbarHandle | null;
  const maybe = inst?.scrollbarInstRef;
  let internal: ScrollbarInternals = {};
  if (maybe) internal = 'value' in maybe ? (maybe.value ?? {}) : (maybe as ScrollbarInternals);
  const fromInst = internal.containerRef ?? null;
  if (fromInst) return fromInst;
  const rootEl = inst?.$el;
  if (!rootEl) return null;
  return (
    rootEl.querySelector<HTMLElement>('.n-scrollbar-container') ??
    rootEl.querySelector<HTMLElement>('[class*="-scrollbar-container"]') ??
    null
  );
}

function hasActiveLogSelection(): boolean {
  if (typeof window === 'undefined') return false;
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed) return false;
  const container = getLogContainer();
  if (!container) return false;
  const anchor = selection.anchorNode;
  const focus = selection.focusNode;
  return !!anchor && !!focus && container.contains(anchor) && container.contains(focus);
}

function onLogScroll() {
  const container = getLogContainer();
  if (!container) return;
  const atBottom = isNearBottom(container);
  isAtBottom.value = atBottom;
  if (atBottom) {
    if (!rawSearchActive.value) {
      autoScrollEnabled.value = true;
      displayedLogs.value = latestLogs.value;
      displayedLineCount.value = latestLineCount.value;
      scrollToBottom();
    } else {
      autoScrollEnabled.value = false;
    }
  } else {
    autoScrollEnabled.value = false;
  }
}

function isNearBottom(el: HTMLElement): boolean {
  return el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
}

function scrollToBottom() {
  const doScroll = () => {
    logScrollbar.value?.scrollTo({ top: Number.MAX_SAFE_INTEGER, behavior: 'auto' });
    const container = getLogContainer();
    if (!container) return;
    container.scrollTop = container.scrollHeight;
    container.scrollTo?.({ top: container.scrollHeight, behavior: 'auto' });
    isAtBottom.value = isNearBottom(container);
  };
  doScroll();
  if (typeof window !== 'undefined') window.requestAnimationFrame(() => doScroll());
}

function scrollToResult(index: number) {
  resultRefs.get(index)?.scrollIntoView?.({ block: 'center' });
}

function scrollToLogLine(lineIndex: number) {
  const lineEl = lineRefs.get(lineIndex);
  if (!lineEl) return;
  lineEl.scrollIntoView({ block: 'center' });
  flashLogLine(lineEl);
  const container = getLogContainer();
  if (container) isAtBottom.value = isNearBottom(container);
}

function flashLogLine(lineEl: HTMLElement) {
  lineEl.classList.remove('log-flash');
  void lineEl.offsetWidth; // restart the animation on rapid repeats
  lineEl.classList.add('log-flash');
  if (typeof window !== 'undefined')
    window.setTimeout(() => lineEl.classList.remove('log-flash'), 3000);
}

function pauseAutoScroll() {
  if (!autoScrollEnabled.value) return;
  autoScrollEnabled.value = false;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
}

function setActiveMatch(index: number) {
  if (matchLines.value.length === 0) return;
  const total = matchLines.value.length;
  const nextIndex = ((index % total) + total) % total;
  activeMatchIndex.value = nextIndex;
  autoScrollEnabled.value = false;
  void nextTick(() => scrollToResult(nextIndex));
}

function openSearchResult(index: number) {
  const lineIndex = matchLines.value[index];
  if (lineIndex === undefined) return;
  pendingJumpLine.value = lineIndex;
  activeMatchIndex.value = index;
  autoScrollEnabled.value = false;
  clearDebounce();
  cancelSearchTask();
  logFilter.value = '';
  searchTerm.value = '';
}

function jumpToPreviousMatch() {
  if (matchLines.value.length > 0) setActiveMatch(activeMatchIndex.value - 1);
}
function jumpToNextMatch() {
  if (matchLines.value.length > 0) setActiveMatch(activeMatchIndex.value + 1);
}
function clearDebounce() {
  if (searchDebounce !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchDebounce);
    searchDebounce = null;
  }
}
function clearSearch() {
  clearDebounce();
  cancelSearchTask();
  pendingJumpLine.value = null;
  logFilter.value = '';
  searchTerm.value = '';
}
function cancelSearchTask() {
  searchTaskId += 1;
  searchInProgress.value = false;
  if (searchTaskTimer !== null && typeof window !== 'undefined') {
    window.clearTimeout(searchTaskTimer);
    searchTaskTimer = null;
  }
}

function startSearch(term: string) {
  cancelSearchTask();
  segmentCache.clear();
  matchLines.value = [];
  matchCount.value = 0;
  const needle = term.trim().toLowerCase();
  if (!needle) return;
  const linesLower = logLinesLower.value;
  const totalLines = linesLower.length;
  let index = 0;
  let count = 0;
  const matches: number[] = [];
  const jobId = searchTaskId;
  const scanLine = (i: number) => {
    const lower = linesLower[i] ?? '';
    let fromIndex = 0;
    let lineHasMatch = false;
    while (fromIndex <= lower.length) {
      const matchIndex = lower.indexOf(needle, fromIndex);
      if (matchIndex === -1) break;
      count += 1;
      lineHasMatch = true;
      fromIndex = matchIndex + needle.length;
    }
    if (lineHasMatch) matches.push(i);
  };
  if (typeof window === 'undefined') {
    for (index = 0; index < totalLines; index += 1) scanLine(index);
    matchLines.value = matches;
    matchCount.value = count;
    searchInProgress.value = false;
    return;
  }
  const processChunk = () => {
    if (jobId !== searchTaskId) return;
    const end = Math.min(totalLines, index + searchChunkSize);
    for (; index < end; index += 1) scanLine(index);
    if (index < totalLines) {
      searchTaskTimer = window.setTimeout(processChunk, 0);
      return;
    }
    searchTaskTimer = null;
    if (jobId !== searchTaskId) return;
    matchLines.value = matches;
    matchCount.value = count;
    searchInProgress.value = false;
  };
  searchInProgress.value = true;
  searchTaskTimer = window.setTimeout(processChunk, 0);
}

async function refreshLogs() {
  if (!authStore.isAuthenticated || authStore.loggingIn) return;
  try {
    const r = await http.get(buildLogUrl(), {
      responseType: 'text',
      transformResponse: [(v: string) => v],
      validateStatus: () => true,
    });
    if (r.status !== 200 || typeof r.data !== 'string') return;
    const nextText = r.data;
    latestLogs.value = nextText;
    latestLineCount.value = nextText ? nextText.split('\n').length : 0;

    const selectionActive = hasActiveLogSelection();
    const searchActiveNow = searchActive.value;
    const container = getLogContainer();
    const atBottom = container ? isNearBottom(container) : true;
    isAtBottom.value = atBottom;
    if (rawSearchActive.value) autoScrollEnabled.value = false;
    else if (!atBottom && autoScrollEnabled.value) autoScrollEnabled.value = false;
    const shouldAutoScroll =
      autoScrollEnabled.value && atBottom && !selectionActive && !searchActiveNow;
    if (shouldAutoScroll) {
      displayedLogs.value = nextText;
      displayedLineCount.value = latestLineCount.value;
      await nextTick();
      scrollToBottom();
    } else if (latestLineCount.value < displayedLineCount.value) {
      displayedLogs.value = nextText;
      displayedLineCount.value = latestLineCount.value;
    }
  } catch {
    /* the next poll retries */
  }
}

function exportLogs() {
  try {
    if (typeof window === 'undefined') return;
    if (platform.value === 'windows') {
      window.location.href = './api/logs/export';
      return;
    }
    const content = latestLogs.value || displayedLogs.value || '';
    const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
    const url = window.URL.createObjectURL(blob);
    const link = window.document.createElement('a');
    const timestamp = new Date()
      .toISOString()
      .replace(/[:.]/g, '-')
      .replace('T', '_')
      .replace('Z', '');
    link.href = url;
    link.download = `sunshine-logs-${timestamp}.log`;
    link.click();
    window.URL.revokeObjectURL(url);
  } catch {
    /* nothing to export */
  }
}

function jumpToLatest() {
  autoScrollEnabled.value = true;
  displayedLogs.value = latestLogs.value;
  displayedLineCount.value = latestLineCount.value;
  void nextTick(() => scrollToBottom());
}

onMounted(async () => {
  loginDisposer = authStore.onLogin(() => void refreshLogs());
  await authStore.waitForAuthentication();
  void nextTick(() => {
    if (getLogContainer()) scrollToBottom();
  });
  logInterval = window.setInterval(() => void refreshLogs(), 5000);
  void refreshLogs();
});

onBeforeUnmount(() => {
  if (logInterval) window.clearInterval(logInterval);
  if (loginDisposer) loginDisposer();
  clearDebounce();
  cancelSearchTask();
});

watch(rawSearch, (value) => {
  clearDebounce();
  if (!value) {
    searchTerm.value = '';
    activeMatchIndex.value = -1;
    return;
  }
  autoScrollEnabled.value = false;
  if (typeof window === 'undefined') {
    searchTerm.value = value;
    return;
  }
  searchDebounce = window.setTimeout(() => {
    searchTerm.value = value;
  }, 150);
});

watch([searchTerm, logLinesLower], ([term]) => startSearch(term));

watch(searchActive, (active) => {
  if (active) {
    autoScrollEnabled.value = false;
    return;
  }
  activeMatchIndex.value = -1;
  if (pendingJumpLine.value !== null) {
    const targetLine = pendingJumpLine.value;
    pendingJumpLine.value = null;
    void nextTick(() => scrollToLogLine(targetLine));
    return;
  }
  const container = getLogContainer();
  if (container && isNearBottom(container)) {
    autoScrollEnabled.value = true;
    displayedLogs.value = latestLogs.value;
    displayedLineCount.value = latestLineCount.value;
    void nextTick(() => scrollToBottom());
  }
});

watch(matchLines, (list) => {
  if (!searchActive.value || list.length === 0) {
    activeMatchIndex.value = -1;
    return;
  }
  if (activeMatchIndex.value >= list.length) activeMatchIndex.value = list.length - 1;
});

watch(searchTerm, (value, oldValue) => {
  if (value !== oldValue) activeMatchIndex.value = -1;
});

watch(logSource, () => {
  resetLogState();
  void refreshLogs();
  void nextTick(() => scrollToBottom());
});

defineExpose({ exportLogs, refreshLogs });
</script>

<template>
  <section id="logs" class="mc-panel">
    <div class="mc-panel-h flex-wrap">
      <div class="flex min-w-0 flex-wrap items-center gap-2.5">
        <span class="mc-panel-title">{{ t2('troubleshooting.logs', 'Logs') }}</span>
        <NSelect
          v-if="logSourceOptions.length > 1"
          v-model:value="logSource"
          size="small"
          class="!w-[190px]"
          :options="logSourceOptions"
        />
        <NInput
          v-model:value="logFilter"
          size="small"
          clearable
          class="!w-[220px]"
          :placeholder="t2('troubleshooting.logs_find', 'Find in log')"
        />
        <template v-if="rawSearchActive">
          <span class="text-xs text-ink-3">{{ matchCountLabel }}</span>
          <NButton
            size="tiny"
            :disabled="matchCount === 0 || searchPending"
            @click="jumpToPreviousMatch"
            >{{ t2('troubleshooting.search_prev', 'Prev') }}</NButton
          >
          <NButton
            size="tiny"
            :disabled="matchCount === 0 || searchPending"
            @click="jumpToNextMatch"
            >{{ t2('troubleshooting.search_next', 'Next') }}</NButton
          >
          <NButton size="tiny" @click="clearSearch">{{
            t2('troubleshooting.search_clear', 'Clear')
          }}</NButton>
        </template>
      </div>
      <div class="flex items-center gap-2">
        <span v-if="newLogsAvailable && !rawSearchActive" class="mc-tag mc-tag-warn"
          >+{{ unseenLines }} {{ t2('troubleshooting.new_lines', 'new lines') }}</span
        >
        <NButton
          v-if="(newLogsAvailable || showJumpToLatest) && !rawSearchActive"
          size="tiny"
          @click="jumpToLatest"
          >{{ t2('troubleshooting.jump_to_latest', 'Jump to latest') }}</NButton
        >
        <NButton size="tiny" @click="exportLogs"
          ><NavIcon name="download" :size="12" />{{
            t2('troubleshooting.export_logs', 'Export logs')
          }}</NButton
        >
      </div>
    </div>

    <NScrollbar
      ref="logScrollbar"
      :style="{ height: `${props.height}px` }"
      @scroll="onLogScroll"
      @wheel="pauseAutoScroll"
      @mousedown="pauseAutoScroll"
      @touchstart="pauseAutoScroll"
    >
      <div class="log-body" @mousedown="pauseAutoScroll">
        <div
          v-if="!searchActive"
          class="log-lines"
          :style="{ '--log-line-number-width': lineNumberWidth }"
        >
          <div
            v-for="(line, index) in logLines"
            :key="index"
            :ref="setLineRef(index)"
            class="log-line"
            :class="levelClass(line)"
          >
            <span class="log-line-number">{{ index + 1 }}</span>
            <span class="log-line-text">{{ line.length === 0 ? ' ' : line }}</span>
          </div>
        </div>
        <div v-else class="space-y-2 p-2">
          <div class="flex items-center justify-between text-[11px] text-ink-3">
            <span>{{ t2('troubleshooting.search_results', 'Results') }}</span>
            <span
              >{{ searchContextLabel
              }}<template v-if="resultsRangeLabel"> · {{ resultsRangeLabel }}</template></span
            >
          </div>
          <div v-if="searchInProgress" class="rounded-lg border border-line p-3 text-xs text-ink-3">
            {{ t2('troubleshooting.search_pending', 'Searching...') }}
          </div>
          <div
            v-else-if="matchCount === 0"
            class="rounded-lg border border-line p-3 text-xs text-ink-3"
          >
            {{ t2('troubleshooting.search_no_matches', 'No matches') }}
          </div>
          <button
            v-for="result in searchResults"
            :key="result.id"
            :ref="setResultRef(result.id)"
            type="button"
            class="w-full rounded-lg border p-2 text-left transition-colors hover:bg-white/[0.03]"
            :class="
              result.id === activeMatchIndex ? 'border-primary/60 bg-primary/5' : 'border-line'
            "
            @click="openSearchResult(result.id)"
          >
            <div class="text-[11px] text-ink-4">
              {{ t2('troubleshooting.search_line', 'Line') }} {{ result.lineIndex + 1 }}
            </div>
            <div class="log-lines mt-1" :style="{ '--log-line-number-width': lineNumberWidth }">
              <div
                v-for="snippetLine in result.snippet"
                :key="snippetLine.lineIndex"
                class="log-line"
                :class="levelClass(snippetLine.text)"
              >
                <span class="log-line-number">{{ snippetLine.lineIndex + 1 }}</span>
                <span class="log-line-text">
                  <template
                    v-for="(segment, sIndex) in getLineSegments(
                      snippetLine.text,
                      snippetLine.lineIndex,
                    )"
                    :key="sIndex"
                  >
                    <span
                      :class="
                        segment.isMatch
                          ? snippetLine.lineIndex === activeLineIndex
                            ? 'log-match-active'
                            : 'log-match'
                          : ''
                      "
                      >{{ segment.text }}</span
                    >
                  </template>
                </span>
              </div>
            </div>
          </button>
        </div>
      </div>
    </NScrollbar>
  </section>
</template>

<style scoped>
.log-body {
  font-family: 'IBM Plex Mono', Consolas, 'Courier New', monospace;
  font-size: 12px;
  line-height: 1.5;
  color: var(--mc-ink-2);
  white-space: pre-wrap;
  word-break: break-word;
  padding: 6px 0;
}
.log-lines {
  display: flex;
  flex-direction: column;
}
.log-line {
  display: grid;
  grid-template-columns: var(--log-line-number-width, 4ch) minmax(0, 1fr);
  gap: 14px;
  padding: 1px 16px;
}
.log-line-number {
  color: var(--mc-ink-4);
  text-align: right;
  user-select: none;
}
.log-level-warn .log-line-text {
  color: var(--mc-gold-soft);
}
.log-level-error .log-line-text {
  color: #ff8a80;
}
.log-level-dim .log-line-text {
  color: var(--mc-ink-4);
}
.log-match {
  background: rgba(255, 176, 32, 0.22);
  border-radius: 2px;
}
.log-match-active {
  background: rgba(255, 176, 32, 0.5);
  color: #1a1208;
  border-radius: 2px;
}
.log-flash {
  animation: log-flash-kf 3s ease-out 1;
}
@keyframes log-flash-kf {
  0% {
    background: rgba(255, 176, 32, 0.35);
  }
  100% {
    background: transparent;
  }
}
</style>
