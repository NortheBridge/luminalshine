import { computed, ref } from 'vue';
import LuminalShineVersion, { GitHubRelease } from '@/sunshine_version';
import { renderReleaseMarkdown } from '@/utils/markdown';
import { useConfigStore } from '@/stores/config';

/**
 * Installed-version and GitHub release check, lifted out of the old
 * Dashboard so the Overview can show it as one health row plus an
 * optional "update available" card.
 */
type ReleaseEntry = GitHubRelease & { prerelease?: boolean; draft?: boolean; tag_name?: string };

// Module-level state: the GitHub check runs once per page load however often
// the Overview is mounted (the unauthenticated GitHub API allows 60 requests/h).
const installedVersion = ref<LuminalShineVersion>(new LuminalShineVersion('0.0.0'));
const githubRelease = ref<GitHubRelease | null>(null);
const preReleaseRelease = ref<GitHubRelease | null>(null);
const notifyPreReleases = ref(false);
const branch = ref('');
const commit = ref('');
const installedIsPrerelease = ref(false);
const loading = ref(false);
const checked = ref(false);
const remoteReachable = ref(true);
let started = false;

export function useUpdateCheck() {
  const configStore = useConfigStore();

  const githubVersion = computed(() =>
    githubRelease.value
      ? LuminalShineVersion.fromRelease(githubRelease.value)
      : new LuminalShineVersion('0.0.0'),
  );
  const preReleaseVersion = computed(() =>
    preReleaseRelease.value
      ? LuminalShineVersion.fromRelease(preReleaseRelease.value)
      : new LuminalShineVersion('0.0.0'),
  );
  // Sanitized HTML (marked → highlight.js → DOMPurify); safe for v-html.
  const stableReleaseHtml = computed(() => renderReleaseMarkdown(githubRelease.value?.body));
  const preReleaseHtml = computed(() => renderReleaseMarkdown(preReleaseRelease.value?.body));

  const installedVersionNotStable = computed(() => {
    if (branch.value && !['master', 'main'].includes(branch.value)) return true;
    return installedIsPrerelease.value;
  });
  // If build is untagged (e.g., 0.0.0), display the current pre-release tag instead (when available)
  const displayVersion = computed(() => {
    const v = installedVersion.value?.version || '0.0.0';
    if (!v || v === '0.0.0') {
      const pre = preReleaseRelease.value?.tag_name || '';
      if (pre) return pre.replace(/^v/i, '');
    }
    return v;
  });
  const stableBuildAvailable = computed(() => {
    if (!githubRelease.value) return false;
    return githubVersion.value.isGreater(installedVersion.value);
  });
  const preReleaseBuildAvailable = computed(() => {
    if (!preReleaseRelease.value || !githubRelease.value) return false;
    return (
      preReleaseVersion.value.isGreater(installedVersion.value) &&
      preReleaseVersion.value.isGreater(githubVersion.value)
    );
  });
  const buildVersionIsDirty = computed(() => {
    if (!installedVersion.value) return false;
    return (
      installedVersion.value.version?.split('.').length === 5 &&
      installedVersion.value.version.indexOf('dirty') !== -1
    );
  });
  const updateAvailable = computed(
    () => stableBuildAvailable.value || (notifyPreReleases.value && preReleaseBuildAvailable.value),
  );

  async function run(): Promise<void> {
    if (started) return;
    started = true;
    loading.value = true;
    try {
      const cfg = await configStore.fetchConfig();
      if (!cfg) {
        started = false;
        return;
      }
      const c = cfg as Record<string, unknown>;
      const str = (v: unknown): string => (typeof v === 'string' ? v : '');
      const notify = c['notify_pre_releases'];
      notifyPreReleases.value =
        notify === true || String(notify) === 'true' || notify === 'enabled';
      const serverVersion = configStore.metadata?.version || str(c['version']);
      installedVersion.value = new LuminalShineVersion(serverVersion || '0.0.0');
      branch.value = str(c['branch']);
      commit.value = str(c['commit']);

      try {
        githubRelease.value = (await fetch(
          'https://api.github.com/repos/NortheBridge/luminalshine/releases/latest',
        ).then((r) => r.json())) as GitHubRelease;
      } catch (e) {
        remoteReachable.value = false;
        console.warn('[Overview] latest release fetch failed', e);
      }
      try {
        const releases = (await fetch(
          'https://api.github.com/repos/NortheBridge/luminalshine/releases',
        ).then((r) => r.json())) as unknown;
        if (Array.isArray(releases)) {
          const list = releases as ReleaseEntry[];
          const prereleases = list.filter((r) => r && r.prerelease && !r.draft);
          const first = prereleases[0];
          if (first) {
            // Pick the latest prerelease by semver, not just the first one listed.
            let best: ReleaseEntry = first;
            let bestV = LuminalShineVersion.fromRelease(best);
            for (const cand of prereleases.slice(1)) {
              const candV = LuminalShineVersion.fromRelease(cand);
              if (candV.isGreater(bestV)) {
                best = cand;
                bestV = candV;
              }
            }
            preReleaseRelease.value = best;
          }
          const installedTag = installedVersion.value?.version || '';
          const installedTagV = installedTag.toLowerCase().startsWith('v')
            ? installedTag
            : 'v' + installedTag;
          const match = list.find(
            (r) =>
              r &&
              !r.draft &&
              typeof r.tag_name === 'string' &&
              (r.tag_name === installedTag || r.tag_name === installedTagV),
          );
          installedIsPrerelease.value = !!(match && match.prerelease === true);
        }
      } catch (e) {
        remoteReachable.value = false;
        console.warn('[Overview] releases list fetch failed', e);
      }
    } finally {
      loading.value = false;
      checked.value = true;
    }
  }

  return {
    installedVersion,
    githubRelease,
    preReleaseRelease,
    githubVersion,
    preReleaseVersion,
    notifyPreReleases,
    branch,
    commit,
    loading,
    checked,
    remoteReachable,
    stableReleaseHtml,
    preReleaseHtml,
    installedVersionNotStable,
    displayVersion,
    stableBuildAvailable,
    preReleaseBuildAvailable,
    buildVersionIsDirty,
    updateAvailable,
    run,
  };
}
