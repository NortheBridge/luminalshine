<template>
  <button
    v-if="visible"
    type="button"
    class="mc-tag select-none"
    :class="[kindClass, canSave ? 'cursor-pointer' : 'cursor-default']"
    :title="tooltip"
    :disabled="!canSave"
    @click="onClick"
  >
    <i :class="iconClass" />
    <span>{{ label }}</span>
  </button>
</template>

<script setup lang="ts">
/**
 * Global save-state badge for the host strip. Shows "Saved" everywhere,
 * counts down pending auto-saves, and turns into the restart affordance when
 * a saved change still needs LuminalShine to restart.
 */
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { storeToRefs } from 'pinia';
import { useMessage } from 'naive-ui';
import { http } from '@/http';

const store = useConfigStore();
const auth = useAuthStore();
const { savingState, manualDirty, validationError } = storeToRefs(store);
const message = useMessage();
const hasPending = computed(() => store.hasPendingPatch());
const restartRequired = computed(
  () => !!(store.lastSaveResult && store.lastSaveResult.restartRequired),
);
const intervalMs = computed(() => store.autosaveIntervalMs || 3000);
const nowMs = ref(Date.now());
const nextAt = computed(() => store.nextAutosaveAt());
const countdown = computed(() => {
  if (!hasPending.value) return 0;
  const ms = Math.max(0, nextAt.value - nowMs.value);
  return Math.ceil(ms / 1000);
});

let timer: any = null;
onMounted(() => {
  timer = setInterval(() => (nowMs.value = Date.now()), 250);
});
onUnmounted(() => {
  if (timer) clearInterval(timer);
});

// Visible once an authenticated session has loaded its config; before that
// there is nothing to report.
const visible = computed(
  () => auth.isAuthenticated && !store.loading && !!store.metadata?.platform,
);
const canSave = computed(
  () =>
    savingState.value === 'error' ||
    manualDirty.value === true ||
    hasPending.value === true ||
    (savingState.value === 'saved' && restartRequired.value === true),
);

const label = computed(() => {
  if (hasPending.value) return `Auto-save in ${countdown.value}s`;
  switch (savingState.value) {
    case 'saving':
      return 'Saving…';
    case 'dirty':
      return 'Unsaved changes';
    case 'saved':
      return restartRequired.value ? 'Needs restart' : 'Saved';
    case 'error':
      return 'Save failed';
    default:
      return 'Saved';
  }
});

const kindClass = computed(() => {
  if (hasPending.value || savingState.value === 'dirty') return 'mc-tag-warn';
  if (savingState.value === 'error') return 'mc-tag-danger';
  if (savingState.value === 'saved' && restartRequired.value) return 'mc-tag-warn';
  return 'mc-tag-ok';
});

const iconClass = computed(() => {
  const base = 'fas text-[10px]';
  if (hasPending.value) return base + ' fa-clock';
  switch (savingState.value) {
    case 'saving':
      return base + ' fa-spinner animate-spin';
    case 'dirty':
      return base + ' fa-circle-exclamation';
    case 'saved':
      return restartRequired.value ? base + ' fa-power-off' : base + ' fa-check';
    case 'error':
      return base + ' fa-triangle-exclamation';
    default:
      return base + ' fa-check';
  }
});

const tooltip = computed(() => {
  if (savingState.value === 'error' && validationError.value) return validationError.value;
  if (hasPending.value)
    return `Auto-save flushes every ${Math.round(intervalMs.value / 1000)}s. Tap to save now.`;
  if (restartRequired.value)
    return 'Saved. A restart is required to apply runtime changes. Tap to restart now.';
  if (savingState.value === 'dirty') return 'Tap to save now.';
  return 'Settings auto-save as you edit.';
});

async function onClick() {
  if (!canSave.value) return;
  try {
    if (restartRequired.value && savingState.value === 'saved') {
      await http.post(
        '/api/restart',
        {},
        { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
      );
      return;
    }
    if (hasPending.value) {
      const ok = await store.flushPatchQueue();
      if (!ok) {
        try {
          message.error(validationError.value || 'Save failed. Check fields for errors.', {
            duration: 5000,
          });
        } catch {
          /* the toast is best effort */
        }
      }
      return;
    }
    const ok = await store.save();
    if (!ok) {
      try {
        message.error(validationError.value || 'Save failed. Check fields for errors.', {
          duration: 5000,
        });
      } catch {
        /* the toast is best effort */
      }
    }
  } catch {
    /* errors are surfaced through savingState */
  }
}
</script>
