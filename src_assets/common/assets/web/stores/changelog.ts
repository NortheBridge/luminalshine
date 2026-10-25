import { computed, ref } from 'vue';
import { defineStore } from 'pinia';
import { http } from '@/http';
import { isReleaseVersion, loadNotes, newestEligible, versionKey } from '@/services/changelog';
import type { ReleaseNotes } from '@/services/changelog';

export interface ChangelogState {
  installed_version: string;
  notify_pre_releases?: boolean;
  seen_installed_versions: string[];
  dismissed_update_version: string;
}
export const useChangelogStore = defineStore('changelog', () => {
  const state = ref<ChangelogState>();
  const releases = ref<ReleaseNotes[]>([]);
  const visible = ref(false);
  const loading = ref(false);
  const cached = ref(false);
  const error = ref(false);
  const acknowledgementError = ref(false);
  const selectedVersion = ref('');
  const mode = ref<'installed' | 'update' | 'manual'>('manual');
  let generation = 0;
  let lastCheck = 0;
  let automaticShown = false;
  let saving = false;
  const request: { pending?: Promise<void> } = {};
  const selected = computed(() =>
    releases.value.find((r) => versionKey(r.version) === versionKey(selectedVersion.value)),
  );
  const available = computed(() =>
    state.value
      ? newestEligible(
          releases.value,
          state.value.installed_version,
          state.value.notify_pre_releases === true,
        )
      : undefined,
  );
  const seenInstalled = computed(
    () =>
      !!state.value?.seen_installed_versions.some(
        (v) => versionKey(v) === versionKey(state.value?.installed_version || ''),
      ),
  );

  function reset(): void {
    generation++;
    state.value = undefined;
    releases.value = [];
    visible.value = false;
    loading.value = false;
    error.value = false;
    acknowledgementError.value = false;
    automaticShown = false;
    lastCheck = 0;
    delete request.pending;
    saving = false;
  }
  function refresh(automatic = true, force = false): Promise<void> {
    if (request.pending) return request.pending;
    if (!force && Date.now() - lastCheck < 15 * 60 * 1000) return Promise.resolve();
    const ticket = generation;
    loading.value = true;
    error.value = false;
    const task = async (): Promise<void> => {
      try {
        const response = await http.get<ChangelogState>('/api/changelog/state');
        if (ticket !== generation) return;
        const next = response.data;
        if (
          !next ||
          !Array.isArray(next.seen_installed_versions) ||
          typeof next.installed_version !== 'string'
        )
          throw new Error('Invalid state');
        state.value = next;
        const notes = await loadNotes(next.installed_version);
        if (ticket !== generation) return;
        releases.value = notes.releases;
        if (visible.value && !selected.value) {
          selectedVersion.value =
            notes.releases.find((r) => versionKey(r.version) === versionKey(next.installed_version))
              ?.version ||
            notes.releases[0]?.version ||
            '';
        }
        cached.value = notes.cached;
        lastCheck = Date.now();
        error.value = !notes.releases.length;
        if (!automatic || automaticShown || visible.value) return;
        const installed = notes.releases.find(
          (r) => versionKey(r.version) === versionKey(next.installed_version),
        );
        if (
          isReleaseVersion(next.installed_version) &&
          !seenInstalled.value &&
          installed?.markdown.trim()
        ) {
          mode.value = 'installed';
          selectedVersion.value = installed.version;
        } else if (
          available.value?.markdown.trim() &&
          versionKey(next.dismissed_update_version) !== versionKey(available.value.version)
        ) {
          mode.value = 'update';
          selectedVersion.value = available.value.version;
        } else return;
        automaticShown = true;
        visible.value = true;
      } catch {
        if (ticket === generation) error.value = true;
      } finally {
        if (ticket === generation) {
          loading.value = false;
          delete request.pending;
        }
      }
    };
    request.pending = task();
    return request.pending;
  }
  async function open(update = false): Promise<void> {
    const ticket = generation;
    mode.value = update ? 'update' : 'manual';
    selectedVersion.value = update
      ? available.value?.version || ''
      : state.value?.installed_version || '';
    visible.value = true;
    await refresh(false, error.value);
    if (ticket !== generation) return;
    selectedVersion.value =
      update && available.value
        ? available.value.version
        : releases.value.find(
            (r) => versionKey(r.version) === versionKey(state.value?.installed_version || ''),
          )?.version ||
          releases.value[0]?.version ||
          '';
  }
  async function acknowledge(kind: 'installed' | 'update', version: string): Promise<void> {
    const ticket = generation;
    if (saving || !state.value) return;
    saving = true;
    acknowledgementError.value = false;
    try {
      const response = await http.post<ChangelogState>('/api/changelog/ack', { kind, version });
      if (ticket === generation) state.value = response.data;
    } catch {
      if (ticket === generation) acknowledgementError.value = true;
    } finally {
      if (ticket === generation) saving = false;
    }
  }
  async function acknowledgeDisplayed(): Promise<void> {
    if (
      mode.value === 'update' ||
      !visible.value ||
      loading.value ||
      !selected.value?.markdown.trim() ||
      seenInstalled.value ||
      !state.value
    )
      return;
    // Viewing the future release before installation must not acknowledge the installed-version screen.
    if (versionKey(selected.value.version) === versionKey(state.value.installed_version)) {
      await acknowledge('installed', state.value.installed_version);
    }
  }
  function close(): void {
    if (mode.value === 'update' && selected.value)
      void acknowledge('update', selected.value.version);
    visible.value = false;
  }
  return {
    state,
    releases,
    visible,
    loading,
    cached,
    error,
    acknowledgementError,
    selectedVersion,
    mode,
    selected,
    available,
    reset,
    refresh,
    open,
    close,
    acknowledgeDisplayed,
  };
});
