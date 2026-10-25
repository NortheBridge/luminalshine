<script setup lang="ts">
import { computed, nextTick, watch } from 'vue';
import { NModal, NButton, NSelect, NSpin } from 'naive-ui';
import { useChangelogStore } from '@/stores/changelog';
import { renderReleaseMarkdown } from '@/utils/markdown';
import { useT2 } from '@/composables/useT2';

const changelog = useChangelogStore();
const t2 = useT2();
const html = computed(() => renderReleaseMarkdown(changelog.selected?.markdown));
const title = computed(() =>
  changelog.mode === 'update'
    ? t2('changelog.update_available', 'New Update Available!')
    : t2('changelog.title', 'Changelog'),
);
const options = computed(() =>
  changelog.releases.map((r) => ({ label: r.version, value: r.version })),
);
watch(
  () => [changelog.visible, changelog.loading, html.value],
  async () => {
    await nextTick();
    if (html.value) await changelog.acknowledgeDisplayed();
  },
  { flush: 'post' },
);
function setVisible(value: boolean): void {
  if (!value) changelog.close();
}
</script>

<template>
  <n-modal
    :show="changelog.visible"
    preset="card"
    :title="title"
    class="w-[min(760px,94vw)]"
    :bordered="false"
    :closable="true"
    :close-on-esc="true"
    :mask-closable="true"
    @update:show="setVisible"
  >
    <div class="space-y-4">
      <p v-if="changelog.state" class="text-sm text-ink-3">
        {{ t2('changelog.installed', 'Installed version') }}:
        {{ changelog.state.installed_version }}
      </p>
      <n-select
        v-if="changelog.mode === 'manual' && options.length"
        v-model:value="changelog.selectedVersion"
        :options="options"
        :aria-label="t2('changelog.version', 'Release version')"
      />
      <n-spin v-if="changelog.loading" size="small" />
      <p v-else-if="!html" role="status">
        {{
          t2(
            'changelog.unavailable',
            'Release notes are unavailable right now. You can try again or open the GitHub releases page.',
          )
        }}
      </p>
      <template v-else>
        <h3 class="text-lg font-semibold">{{ changelog.selected?.title }}</h3>
        <p v-if="changelog.cached" class="text-sm text-ink-3">
          {{ t2('changelog.cached', 'Showing saved release notes while offline.') }}
        </p>
        <!-- eslint-disable vue/no-v-html -- Markdown is sanitized by renderReleaseMarkdown before rendering. -->
        <article
          class="prose prose-invert max-h-[55vh] max-w-none overflow-y-auto pr-2"
          v-html="html"
        ></article>
        <!-- eslint-enable vue/no-v-html -->
      </template>
      <div v-if="changelog.acknowledgementError" role="status" class="text-sm">
        {{
          t2(
            'changelog.ack_failed',
            'Could not save your acknowledgement on this host. The changelog may appear again.',
          )
        }}
        <n-button size="small" @click="changelog.acknowledgeDisplayed()">{{
          t2('changelog.retry', 'Retry')
        }}</n-button>
      </div>
      <div class="flex flex-wrap items-center gap-3">
        <a
          v-if="changelog.mode === 'update' && changelog.selected"
          class="text-primary underline"
          :href="changelog.selected.github_url"
          target="_blank"
          rel="noopener noreferrer"
          >{{ t2('changelog.download', 'Download update') }}</a
        >
        <a
          v-if="changelog.selected?.featurebase_url"
          class="text-primary underline"
          :href="changelog.selected.featurebase_url"
          target="_blank"
          rel="noopener noreferrer"
          >{{ t2('changelog.featurebase', 'View on Featurebase') }}</a
        >
        <a
          v-else
          class="text-primary underline"
          :href="
            changelog.selected?.github_url ||
            'https://github.com/NortheBridge/luminalshine/releases'
          "
          target="_blank"
          rel="noopener noreferrer"
          >{{ t2('changelog.github', 'View on GitHub') }}</a
        >
        <n-button
          v-if="changelog.error || !html"
          :loading="changelog.loading"
          @click="changelog.refresh(false, true)"
          >{{ t2('changelog.retry', 'Retry') }}</n-button
        >
        <n-button class="ml-auto" @click="changelog.close()">{{
          t2('changelog.close', 'Close')
        }}</n-button>
      </div>
    </div>
  </n-modal>
</template>

<style scoped>
article :deep(h1),
article :deep(h2),
article :deep(h3) {
  margin: 1rem 0 0.5rem;
  font-size: 1.1rem;
  font-weight: 600;
}
article :deep(p),
article :deep(ul),
article :deep(ol) {
  margin: 0.6rem 0;
  line-height: 1.65;
}
article :deep(ul) {
  list-style: disc;
  padding-left: 1.5rem;
}
article :deep(ol) {
  list-style: decimal;
  padding-left: 1.5rem;
}
article :deep(a) {
  color: var(--mc-gold, #ffb020);
  text-decoration: underline;
}
article :deep(pre) {
  overflow-x: auto;
  padding: 0.8rem;
  border-radius: 0.4rem;
  background: #0e1013;
}
article :deep(img) {
  max-width: 100%;
  height: auto;
}
article :deep(table) {
  display: block;
  overflow-x: auto;
  border-collapse: collapse;
}
article :deep(th),
article :deep(td) {
  padding: 0.4rem 0.6rem;
  border: 1px solid #34383e;
}
</style>
