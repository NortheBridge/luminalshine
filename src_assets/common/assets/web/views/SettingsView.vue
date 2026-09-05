<template>
  <div class="flex gap-3.5">
    <!-- Section navigation -->
    <aside class="mc-panel w-[200px] shrink-0 self-start">
      <div class="mc-panel-h">
        <span class="mc-panel-title">{{ t2('shell.nav_settings', 'Settings') }}</span>
        <a
          class="mc-panel-link"
          href="https://appdocs.northebridge.com/projects/luminalshine/en/latest/"
          target="_blank"
          rel="noopener noreferrer"
          >{{ t2('settings.docs', 'Docs') }}</a
        >
      </div>
      <nav class="flex flex-col gap-0.5 p-2" :aria-label="t2('shell.nav_settings', 'Settings')">
        <button
          v-for="tab in tabsFiltered"
          :key="tab.id"
          type="button"
          class="subnav-item"
          :class="{ 'subnav-item-on': activeId === tab.id }"
          :aria-current="activeId === tab.id ? 'page' : undefined"
          @click="goSection(tab.id)"
        >
          <span>{{ tab.name }}</span>
          <span v-if="badgeFor(tab.id)" class="text-[10.5px] text-ink-4">{{
            badgeFor(tab.id)
          }}</span>
        </button>
      </nav>
    </aside>

    <!-- Active section -->
    <div class="mc-panel min-w-0 flex-1">
      <div class="mc-panel-h flex-wrap">
        <div class="min-w-0">
          <span class="mc-panel-title">{{ activeTab?.name }}</span>
          <div class="mt-0.5 text-[11px] text-ink-4">{{ activeSubtitle }}</div>
        </div>
        <div class="relative">
          <n-input
            v-model:value="searchQuery"
            size="small"
            clearable
            class="w-[260px]"
            :aria-label="t2('settings.search_all', 'Search all settings')"
            :placeholder="t2('settings.search_all', 'Search all settings')"
            @focus="onSearchFocus"
            @blur="onSearchBlur"
            @keydown.enter.prevent="jumpFirstResult"
          />
          <div
            v-if="searchOpen && searchResults.length"
            class="mc-panel absolute right-0 top-9 z-[60] max-h-80 w-[360px] overflow-y-auto shadow-xl"
          >
            <button
              v-for="(r, i) in searchResults"
              :key="`${r.sectionId}-${r.key || r.label}-${i}`"
              type="button"
              class="mc-row mc-row-clickable w-full text-left"
              @mousedown.prevent="goTo(r)"
            >
              <div class="min-w-0">
                <div class="mc-row-primary truncate">{{ r.label }}</div>
                <div class="mc-row-secondary truncate">
                  {{ r.path }}<span v-if="r.desc"> · {{ r.desc }}</span>
                </div>
              </div>
            </button>
          </div>
          <div
            v-else-if="searchOpen && searchQuery.trim() && !searchResults.length"
            class="mc-panel absolute right-0 top-9 z-[60] w-[360px] px-4 py-3 text-xs text-ink-4 shadow-xl"
          >
            {{ t2('settings.no_results', 'No settings match.') }}
          </div>
        </div>
      </div>

      <div v-if="isLoading" class="px-4 py-6 text-xs text-ink-4">
        {{ t2('_common.loading', 'Loading...') }}
      </div>
      <div v-else-if="isError" class="flex items-center gap-3 px-4 py-6 text-xs">
        <span class="text-danger">{{ store.error }}</span>
        <n-button size="small" @click="store.reloadConfig()">{{
          t2('settings.retry', 'Retry')
        }}</n-button>
      </div>
      <div v-else ref="mainEl" class="settings-sections">
        <!-- Every section stays mounted (hidden) so the search index covers all of them. -->
        <section
          v-for="tab in tabsFiltered"
          :id="tab.id"
          :key="tab.id"
          :ref="(el) => setSectionRef(tab.id, el)"
          v-show="activeId === tab.id"
          class="px-5 py-4"
        >
          <h3 class="sr-only">{{ tab.name }}</h3>
          <component :is="tab.component" />
        </section>
      </div>

      <div class="border-t border-line px-4 py-2.5 text-[11px] text-ink-4">
        {{
          t2(
            'settings.autosave_note',
            'Settings auto-save. Anything that needs a restart is tracked in the Changes panel.',
          )
        }}
      </div>
    </div>

    <!-- Inspector: the one save surface -->
    <InspectorPanel
      :title="t2('settings.changes', 'Changes')"
      :subtitle="t2('settings.changes_sub', 'Auto-save every 3 s · restarts tracked here')"
      :tag="changesTag.text"
      :tag-kind="changesTag.kind"
    >
      <div class="mc-kicker px-[18px] pb-1 pt-3">{{ t2('settings.status', 'Status') }}</div>
      <div class="grid grid-cols-[118px_minmax(0,1fr)] gap-x-3 gap-y-1 px-[18px] pb-2 text-xs">
        <span class="text-ink-3">{{ t2('settings.last_saved', 'Last saved') }}</span>
        <span class="text-ink">{{ lastSavedText }}</span>
        <span class="text-ink-3">{{ t2('settings.pending', 'Pending') }}</span>
        <span class="text-ink">{{ pendingText }}</span>
        <span class="text-ink-3">{{ t2('settings.validation', 'Validation') }}</span>
        <span :class="store.validationError ? 'text-danger' : 'text-success'">{{
          store.validationError || t2('settings.ok', 'ok')
        }}</span>
      </div>

      <div class="mc-kicker px-[18px] pb-1 pt-3">
        {{ t2('settings.needs_restart', 'Needs restart') }}
      </div>
      <div
        v-if="restartKeys.length === 0 && !store.lastSaveResult?.restartRequired"
        class="px-[18px] pb-2 text-xs text-ink-4"
      >
        {{ t2('settings.no_restart', 'Nothing pending.') }}
      </div>
      <template v-else>
        <button
          v-for="k in restartKeys"
          :key="k"
          type="button"
          class="mc-row mc-row-clickable w-full text-left"
          @click="goToKey(k)"
        >
          <div class="min-w-0">
            <div class="mc-row-primary truncate">
              {{ sectionNameForKey(k) }} · {{ labelForKey(k) }}
            </div>
            <div class="mc-row-secondary truncate">{{ changeSummary(k) }}</div>
          </div>
          <span class="mc-tag mc-tag-warn">{{ t2('settings.restart_tag', 'restart') }}</span>
        </button>
        <div v-if="restartKeys.length === 0" class="px-[18px] pb-2 text-xs text-ink-3">
          {{
            t2('settings.restart_generic', 'The last save changed a setting that needs a restart.')
          }}
        </div>
      </template>
      <div v-if="store.lastSaveResult?.deferred" class="px-[18px] pb-2 text-[11px] text-ink-4">
        {{ t2('settings.deferred', 'A stream is live; the last save applies when it ends.') }}
      </div>

      <div class="mc-kicker px-[18px] pb-1 pt-3">
        {{ t2('settings.recent', 'Recently changed') }}
      </div>
      <div v-if="recent.length === 0" class="px-[18px] pb-3 text-xs text-ink-4">
        {{ t2('settings.no_recent', 'No changes this session.') }}
      </div>
      <button
        v-for="c in recent"
        :key="`${c.key}-${c.at}`"
        type="button"
        class="mc-row mc-row-clickable w-full text-left"
        @click="goToKey(c.key)"
      >
        <div class="min-w-0">
          <div class="mc-row-primary truncate">
            {{ sectionNameForKey(c.key) }} · {{ labelForKey(c.key) }}
          </div>
          <div class="mc-row-secondary truncate">
            {{ fmtValue(c.from) }} → {{ fmtValue(c.to) }} · {{ fmtTime(c.at) }}
          </div>
        </div>
        <span class="mc-tag" :class="c.restart ? 'mc-tag-warn' : 'mc-tag-ok'">{{
          c.restart ? t2('settings.restart_tag', 'restart') : t2('settings.applied_tag', 'applied')
        }}</span>
      </button>

      <template #footer>
        <n-button size="small" :disabled="!canDiscard" @click="discardPending">{{
          t2('settings.discard', 'Discard pending')
        }}</n-button>
        <n-button
          size="small"
          type="primary"
          :loading="applying"
          :disabled="!canApply"
          @click="apply"
        >
          <NavIcon name="power" :size="14" />{{
            needsRestart
              ? t2('settings.apply_restart', 'Apply & restart')
              : t2('_common.save', 'Save')
          }}
        </n-button>
      </template>
    </InspectorPanel>
  </div>
</template>

<script setup lang="ts">
// @ts-nocheck
import { ref, computed, onMounted, onUnmounted, watch, markRaw, nextTick } from 'vue';
import { NInput, NButton, useMessage } from 'naive-ui';
import { useRoute, useRouter } from 'vue-router';
import { useI18n } from 'vue-i18n';
import General from '@/configs/tabs/General.vue';
import Inputs from '@/configs/tabs/Inputs.vue';
import Network from '@/configs/tabs/Network.vue';
import Files from '@/configs/tabs/Files.vue';
import Advanced from '@/configs/tabs/Advanced.vue';
import Playnite from '@/configs/tabs/Playnite.vue';
import SteamLibrary from '@/configs/tabs/SteamLibrary.vue';
import AudioVideo from '@/configs/tabs/AudioVideo.vue';
import Capture from '@/configs/tabs/Capture.vue';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { useHostStore } from '@/stores/host';
import { useT2 } from '@/composables/useT2';
import { http } from '@/http';
import { storeToRefs } from 'pinia';
import InspectorPanel from '@/components/shell/InspectorPanel.vue';
import NavIcon from '@/components/shell/NavIcon.vue';

const store = useConfigStore();
const { config } = storeToRefs(store);
const message = useMessage();
const auth = useAuthStore();
const host = useHostStore();
const { t } = useI18n();
const t2 = useT2();

// derive loading/error/ready from the store instead of local flags
const isLoading = computed(() => store.loading === true);
const isError = computed(() => store.error != null);
const isReady = computed(() => !!config.value && !isLoading.value && !isError.value);

const mainEl = ref(null);
const searchQuery = ref('');
const searchOpen = ref(false);
const searchResults = ref([]);
const searchIndex = ref([]); // { sectionId, label, path, key, el, desc, options, optionsText }
const sectionRefs = new Map();

function setSectionRef(id, el) {
  if (el) sectionRefs.set(id, el);
  else sectionRefs.delete(id);
}

const tabs = [
  { id: 'general', name: 'General', component: markRaw(General) },
  { id: 'input', name: 'Input', component: markRaw(Inputs) },
  { id: 'av', name: 'Audio / Video', component: markRaw(AudioVideo) },
  { id: 'capture', name: 'Capture', component: markRaw(Capture) },
  { id: 'network', name: 'Network', component: markRaw(Network) },
  { id: 'files', name: 'Files', component: markRaw(Files) },
  { id: 'advanced', name: 'Advanced', component: markRaw(Advanced) },
  // "Steam Library Integration" must always sit immediately above
  // "Playnite" — user-requested priority ordering so the Steam
  // toggles surface before the meta-launcher integration.
  { id: 'steamlibrary', name: 'Steam Library', component: markRaw(SteamLibrary) },
  { id: 'playnite', name: 'Playnite', component: markRaw(Playnite) },
];
const SUBTITLES = {
  general: ['settings.sub_general', 'Host identity, logging and update behaviour'],
  input: ['settings.sub_input', 'Gamepads, keyboard and mouse handling'],
  av: ['settings.sub_av', 'Encoders, codecs, audio and display modes'],
  capture: ['settings.sub_capture', 'Capture engine and frame pacing'],
  network: ['settings.sub_network', 'Ports, address family and UPnP'],
  files: ['settings.sub_files', 'Paths for state, certificates and logs'],
  advanced: ['settings.sub_advanced', 'Everything with sharp edges'],
  steamlibrary: ['settings.sub_steam', 'Steam library import and shortcuts'],
  playnite: ['settings.sub_playnite', 'Playnite extension and library sync'],
};

const tabsFiltered = computed(() => tabs);

const route = useRoute();
const router = useRouter();

// ---- active section (URL-addressable) ---------------------------------------
const activeId = ref(
  typeof route.query.sec === 'string' && tabs.some((x) => x.id === route.query.sec)
    ? route.query.sec
    : 'general',
);
const activeTab = computed(() => tabs.find((x) => x.id === activeId.value) ?? tabs[0]);
const activeSubtitle = computed(() => {
  const s = SUBTITLES[activeId.value];
  return s ? t2(s[0], s[1]) : '';
});
let suppressRouteScroll = false;

const goSection = (id) => {
  const dest = { path: '/settings', query: { sec: id } };
  route.path === '/settings' ? router.replace(dest) : router.push(dest);
};

async function ensureSectionOpen(id) {
  if (!id) return;
  if (activeId.value !== id && tabs.some((x) => x.id === id)) activeId.value = id;
  await nextTick();
  await new Promise((resolve) => requestAnimationFrame(resolve));
}

async function scrollToOpen(id) {
  if (!id) return;
  await ensureSectionOpen(id);
  // Section switch: back to the top of the scrolling pane (the section sits
  // under the panel header, so scrollIntoView would hide the search box).
  let node = sectionRefs.get(id)?.parentElement ?? null;
  while (node && node !== document.body) {
    const oy = getComputedStyle(node).overflowY;
    if ((oy === 'auto' || oy === 'scroll') && node.scrollHeight > node.clientHeight) {
      node.scrollTo({ top: 0, behavior: 'smooth' });
      return;
    }
    node = node.parentElement;
  }
  window.scrollTo({ top: 0, behavior: 'smooth' });
}

watch(
  () => route.query.sec,
  (id) => {
    if (typeof id !== 'string') return;
    if (suppressRouteScroll) return;
    if (isReady.value) {
      scrollToOpen(id);
    } else {
      const stop = watch(
        () => isReady.value,
        (ready) => {
          if (ready) {
            stop();
            scrollToOpen(id);
          }
        },
        { immediate: false },
      );
    }
  },
);

async function runRouteJump(rawJump) {
  if (typeof rawJump !== 'string') return;
  const query = rawJump.trim();
  if (!query) return;

  queueBuildIndex();
  await nextTick();
  await new Promise((resolve) => requestAnimationFrame(resolve));

  searchQuery.value = query;
  await nextTick();

  if (searchResults.value.length) {
    await goTo(searchResults.value[0]);
  }
}

watch(
  () => route.query.jump,
  async (jump) => {
    if (!isReady.value) return;
    await runRouteJump(jump);
  },
);

onMounted(async () => {
  try {
    if (auth && typeof auth.init === 'function') await auth.init();
  } catch (err) {
    console.warn('auth.init failed', err);
  }
  await auth.waitForAuthentication();
  await store.fetchConfig();
  if (!host.running) void host.start();
  if (config.value) queueBuildIndex();

  if (typeof route.query.sec === 'string') {
    const id = route.query.sec;
    if (isReady.value) {
      await nextTick();
      setTimeout(() => scrollToOpen(id), 0);
    } else {
      const stop = watch(
        () => isReady.value,
        async (ready) => {
          if (ready) {
            stop();
            await nextTick();
            setTimeout(() => scrollToOpen(id), 0);
          }
        },
        { immediate: false },
      );
    }
  }

  if (typeof route.query.jump === 'string') {
    if (isReady.value) {
      await runRouteJump(route.query.jump);
    } else {
      const stop = watch(
        () => isReady.value,
        async (ready) => {
          if (ready) {
            stop();
            await runRouteJump(route.query.jump);
          }
        },
        { immediate: false },
      );
    }
  }
});

// When auth becomes ready or authenticated, rebuild index (debounced a bit)
let authTimer = null;
watch(
  () => ({ ready: auth.ready, authed: auth.isAuthenticated }),
  () => {
    clearTimeout(authTimer);
    authTimer = setTimeout(() => queueBuildIndex(), 120);
  },
  { deep: true },
);
onUnmounted(() => {
  if (authTimer) clearTimeout(authTimer);
});

// ---- Changes panel -----------------------------------------------------------
const KEY_SECTION = {
  port: 'network',
  address_family: 'network',
  upnp: 'network',
  pkey: 'files',
  cert: 'files',
};
const recent = computed(() => (store.recentChanges || []).slice(0, 8));
const restartKeys = computed(() => store.restartPendingKeys || []);
const needsRestart = computed(
  () => restartKeys.value.length > 0 || store.lastSaveResult?.restartRequired === true,
);
const pendingCount = computed(() => {
  try {
    return store.hasPendingPatch() ? 1 : 0;
  } catch {
    return 0;
  }
});
const pendingText = computed(() => {
  if (store.manualDirty) return t2('settings.pending_manual', 'manual save required');
  if (store.savingState === 'saving') return t2('settings.pending_saving', 'saving…');
  if (pendingCount.value > 0) return t2('settings.pending_autosave', 'auto-saving shortly');
  return t2('settings.pending_none', 'none');
});
const lastSavedText = computed(() => {
  const at = store.lastSavedAt;
  if (!at) return t2('settings.not_saved_yet', 'no saves this session');
  return `${new Date(at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })} · ${t2('settings.auto', 'auto')}`;
});
const changesTag = computed(() => {
  if (store.savingState === 'error')
    return { text: t2('settings.tag_error', 'save failed'), kind: 'danger' };
  if (needsRestart.value) {
    const n = restartKeys.value.length;
    return {
      text:
        n > 0
          ? `${n} ${n === 1 ? t2('settings.tag_restart_one', 'needs restart') : t2('settings.tag_restart_many', 'need restart')}`
          : t2('settings.tag_restart_one', 'needs restart'),
      kind: 'warn',
    };
  }
  if (store.manualDirty || pendingCount.value > 0 || store.savingState === 'saving')
    return { text: t2('settings.tag_pending', 'pending'), kind: 'warn' };
  return { text: t2('settings.tag_saved', 'saved'), kind: 'ok' };
});
const canApply = computed(() => needsRestart.value || store.manualDirty || pendingCount.value > 0);
const canDiscard = computed(() => store.manualDirty || pendingCount.value > 0);

function badgeFor(id) {
  const n = restartKeys.value.filter((k) => KEY_SECTION[k] === id).length;
  if (n > 0) return `${n} ${t2('settings.restart_tag', 'restart')}`;
  if (id === 'playnite' && host.playnite?.update_available)
    return t2('settings.update_badge', 'update');
  return '';
}
function labelForKey(key) {
  const v = t(`config.${key}`);
  return v && v !== `config.${key}` ? v : key;
}
function sectionNameForKey(key) {
  const sec = KEY_SECTION[key] || searchIndex.value.find((it) => it.key === key)?.sectionId;
  return tabs.find((x) => x.id === sec)?.name || t2('shell.nav_settings', 'Settings');
}
function changeSummary(key) {
  const c = (store.recentChanges || []).find((x) => x.key === key);
  return c ? `${fmtValue(c.from)} → ${fmtValue(c.to)} · ${fmtTime(c.at)}` : '';
}
function fmtValue(v) {
  if (v === undefined || v === null || v === '') return t2('settings.default_value', 'default');
  if (typeof v === 'boolean') return v ? t2('stream.on', 'on') : t2('stream.off', 'off');
  if (typeof v === 'object') {
    const s = JSON.stringify(v);
    return s.length > 28 ? `${s.slice(0, 26)}…` : s;
  }
  const s = String(v);
  return s.length > 28 ? `${s.slice(0, 26)}…` : s;
}
function fmtTime(at) {
  return new Date(at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}
async function goToKey(key) {
  queueBuildIndex();
  await nextTick();
  const hit = searchIndex.value.find((it) => it.key === key);
  if (hit) {
    await goTo(hit);
    return;
  }
  const sec = KEY_SECTION[key];
  if (sec) goSection(sec);
}

async function save() {
  if (!auth.isAuthenticated) return false;
  if (!config.value) return false;
  const ok = await (store.save ? store.save() : Promise.resolve(false));
  if (!ok) {
    try {
      message.error(store.validationError || 'Save failed. Check fields for errors.', {
        duration: 5000,
      });
    } catch {
      /* the toast is best effort */
    }
  }
  return ok;
}

const applying = ref(false);
async function apply() {
  if (applying.value) return;
  applying.value = true;
  try {
    const ok = await save();
    if (!ok) return;
    if (!needsRestart.value) {
      message.success(t2('settings.saved_toast', 'Saved.'));
      return;
    }
    const res = await http.post(
      '/api/restart',
      {},
      { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
    );
    if (!res || res.status >= 400) {
      message.error(
        `${t2('settings.restart_failed', 'Restart request failed')} (HTTP ${res?.status ?? '?'})`,
      );
      return;
    }
    store.clearRestartPending();
    message.success(
      t2('settings.restart_sent', 'Restart requested. LuminalShine comes back in a few seconds.'),
    );
  } catch (err) {
    message.error(err instanceof Error ? err.message : 'Request failed');
  } finally {
    applying.value = false;
  }
}

async function discardPending() {
  try {
    await store.discardPending();
    message.info(t2('settings.discarded', 'Pending changes discarded.'));
  } catch {
    /* reloadConfig reports through the store */
  }
}

// ---- search index (DOM-scraped; unchanged from the accordion version) ----------
function buildSearchIndex() {
  const root = mainEl.value;
  if (!root) return;
  const items = [];
  for (const sec of Array.from(root.querySelectorAll('section[id]'))) {
    const sectionId = sec.getAttribute('id');
    const sectionTitle = sec.querySelector('h3')?.textContent?.trim() || sectionId;
    for (const lbl of Array.from(sec.querySelectorAll('label'))) {
      const text = (lbl.textContent || '').trim();
      if (!text) continue;
      const forId = lbl.getAttribute('for');
      let target = null;
      if (forId) {
        try {
          target = sec.querySelector('#' + CSS.escape(forId));
        } catch (err) {
          console.warn('buildSearchIndex: CSS.escape lookup failed', err);
        }
      }
      if (!target)
        target = lbl.closest('div')?.querySelector('input,select,textarea,.form-control');
      let descText = '';
      try {
        const isDescClass = (cls) =>
          !!cls &&
          (cls.includes('text-[11px]') || cls.includes('form-text') || cls.includes('text-xs'));
        const container = lbl.parentElement;
        if (container) {
          const candidate = Array.from(container.querySelectorAll('div,p,small')).find(
            (d) => d !== lbl && isDescClass(d.className) && d.textContent.trim().length > 0,
          );
          if (candidate) descText = candidate.textContent.trim();
        }
        if (!descText) {
          let sib = lbl.nextElementSibling;
          let steps = 0;
          while (sib && steps < 6) {
            if (isDescClass(sib.className) && sib.textContent.trim()) {
              descText = sib.textContent.trim();
              break;
            }
            sib = sib.nextElementSibling;
            steps++;
          }
        }
      } catch (err) {
        console.warn('buildSearchIndex: description extraction failed', err);
      }
      let optionsList = [];
      let optionsText = '';
      try {
        if (target && target.tagName && target.tagName.toLowerCase() === 'select') {
          optionsList = Array.from(target.querySelectorAll('option')).map((o) => ({
            text: (o.textContent || '').trim(),
            value: (o.value || '').trim(),
          }));
        }
        if ((!optionsList || optionsList.length === 0) && target) {
          const ds = target.getAttribute?.('data-search-options') || '';
          if (ds && typeof ds === 'string') {
            optionsList = ds
              .split('|')
              .map((chunk) => chunk.trim())
              .filter(Boolean)
              .map((pair) => {
                const [textRaw, valRaw] = pair.split('::');
                return { text: (textRaw || '').trim(), value: (valRaw || '').trim() };
              })
              .filter((o) => o.text || o.value);
          }
        }
        if (optionsList && optionsList.length) {
          optionsText = optionsList
            .map((o) => `${o.text || ''} ${o.value || ''}`.trim())
            .filter(Boolean)
            .join(' | ');
        }
      } catch (err) {
        optionsList = [];
        optionsText = '';
        console.warn('buildSearchIndex: options extraction failed', err);
      }

      if (target)
        items.push({
          sectionId,
          label: text,
          path: `${sectionTitle} › ${text}`,
          key: (forId || target.getAttribute?.('id') || '').trim(),
          el: target,
          desc: descText,
          options: optionsList,
          optionsText,
        });
    }
  }
  searchIndex.value = items;
}

let buildPending = false;
function queueBuildIndex() {
  if (buildPending) return;
  buildPending = true;
  requestAnimationFrame(() => {
    buildPending = false;
    buildSearchIndex();
  });
}

watch(searchQuery, (q) => {
  const v = (q || '').trim().toLowerCase();
  const terms = v.split(/\s+/).filter(Boolean);
  searchOpen.value = terms.length > 0;
  if (!terms.length) {
    searchResults.value = [];
    return;
  }
  // Score matches: require all terms to match one of the fields. Label highest, key, options, path, then desc.
  const scoreFor = (it) => {
    const lv = it.label.toLowerCase();
    const pv = it.path.toLowerCase();
    const dv = (it.desc || '').toLowerCase();
    const ov = (it.optionsText || '').toLowerCase();
    const kv = (it.key || '').toLowerCase();
    let total = 0;
    for (const term of terms) {
      let s = 0;
      if (lv.includes(term)) {
        s += 100 - lv.indexOf(term);
        if (lv.startsWith(term)) s += 50;
      } else if (kv.includes(term)) {
        s += 90 - kv.indexOf(term);
      } else if (ov.includes(term)) {
        s += 60 - ov.indexOf(term) / 10;
      } else if (pv.includes(term)) {
        s += 40 - pv.indexOf(term) / 100;
      } else if (dv.includes(term)) {
        s += 20 - dv.indexOf(term) / 1000;
      } else {
        return 0;
      }
      total += s;
    }
    total -= (pv.length + dv.length + ov.length) / 1000;
    return total;
  };
  searchResults.value = searchIndex.value
    .map((it) => ({ it, s: scoreFor(it) }))
    .filter((x) => x.s > 0)
    .sort((a, b) => b.s - a.s)
    .slice(0, 15)
    .map((x) => x.it);
});

async function jumpFirstResult() {
  if (searchResults.value.length) await goTo(searchResults.value[0]);
}
async function goTo(item) {
  if (!item) return;
  searchOpen.value = false;
  let suppressing = false;
  try {
    if (item.sectionId) {
      suppressRouteScroll = true;
      suppressing = true;
      goSection(item.sectionId);
      await ensureSectionOpen(item.sectionId);
    }
    await nextTick();
    await new Promise((resolve) => requestAnimationFrame(resolve));
    let target = item.el || null;
    if (target) {
      try {
        const wrapper = target.closest(
          '.n-input, .n-select, .n-input-number, .n-checkbox, .n-switch, .form-control',
        );
        if (wrapper) target = wrapper;
      } catch {
        /* keep the raw target */
      }
      target.scrollIntoView({ behavior: 'smooth', block: 'center' });
      flash(target);
    }
  } catch (err) {
    console.warn('goTo: scroll/flash failed', err);
  } finally {
    if (suppressing) suppressRouteScroll = false;
  }
}
function flash(el) {
  let target = el;
  try {
    const wrapper = target?.closest?.(
      '.n-input, .n-select, .n-input-number, .n-checkbox, .n-switch, .form-control',
    );
    if (wrapper) target = wrapper;
  } catch {
    /* keep the raw target */
  }
  target?.classList.add('flash-highlight');
  setTimeout(() => target?.classList.remove('flash-highlight'), 5200);
}

function onSearchFocus() {
  searchOpen.value = (searchQuery.value || '').length > 0;
}
function onSearchBlur() {
  setTimeout(() => {
    searchOpen.value = false;
  }, 120);
}
</script>

<style scoped>
.subnav-item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  height: 34px;
  padding: 0 10px;
  border-radius: 8px;
  font-size: 12.5px;
  font-weight: 500;
  color: #9aa0a6;
  text-align: left;
  transition:
    background-color 0.15s ease,
    color 0.15s ease;
}
.subnav-item:hover {
  color: var(--mc-ink);
  background: rgba(255, 255, 255, 0.04);
}
.subnav-item-on,
.subnav-item-on:hover {
  background: rgba(255, 176, 32, 0.12);
  color: var(--mc-gold);
}
.settings-sections :deep(.config-page) {
  max-width: 880px;
}
</style>

<style>
/* The search "jump" ring (global: it lands on Naive UI wrappers). */
@keyframes flash-highlight-kf {
  0% {
    box-shadow: 0 0 0 0 rgba(255, 176, 32, 0.55);
  }
  40% {
    box-shadow: 0 0 0 4px rgba(255, 176, 32, 0.35);
  }
  100% {
    box-shadow: 0 0 0 0 rgba(255, 176, 32, 0);
  }
}
.flash-highlight {
  animation: flash-highlight-kf 5s ease-out 1;
  border-radius: 8px;
}
</style>
