import Version from '@/sunshine_version';

export interface ReleaseNotes {
  version: string;
  title: string;
  markdown: string;
  github_url: string;
  featurebase_url: string;
  published_at: string;
  prerelease: boolean;
}

export const FEED_URL =
  'https://raw.githubusercontent.com/NortheBridge/luminalshine/changelog-feed/changelog.json';
const GITHUB = 'https://api.github.com/repos/NortheBridge/luminalshine/releases';
const CACHE_KEY = 'luminalshine.public-changelog.v1';
export const versionKey = (version: string): string => version.trim().replace(/^v/i, '');
export const isReleaseVersion = (version: string): boolean =>
  /^v?\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?(?:\+[a-zA-Z0-9.-]+)?$/i.test(version) &&
  versionKey(version) !== '0.0.0';

function record(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' ? (value as Record<string, unknown>) : {};
}
function text(value: unknown): string {
  return typeof value === 'string' ? value : '';
}
function publicUrl(value: unknown): string {
  try {
    const url = new URL(text(value));
    return url.protocol === 'https:' && !url.username && !url.password ? url.href : '';
  } catch {
    return '';
  }
}
export function parseNotes(value: unknown, github = false): ReleaseNotes[] {
  if (!Array.isArray(value)) return [];
  return value.slice(0, 500).flatMap((item: unknown) => {
    const entry = record(item);
    const version = text(entry[github ? 'tag_name' : 'version']);
    if (!isReleaseVersion(version) || entry['draft'] === true) return [];
    const githubUrl = publicUrl(entry[github ? 'html_url' : 'github_url']);
    if (!githubUrl.startsWith('https://github.com/NortheBridge/luminalshine/releases/tag/'))
      return [];
    return [
      {
        version,
        title: text(entry[github ? 'name' : 'title']) || version,
        markdown: text(entry[github ? 'body' : 'markdown']).slice(0, 500000),
        github_url: githubUrl,
        featurebase_url: github ? '' : publicUrl(entry['featurebase_url']),
        published_at: text(entry['published_at']),
        prerelease: entry['prerelease'] === true,
      },
    ];
  });
}
export function newestEligible(releases: ReleaseNotes[], installed: string, prereleases: boolean) {
  if (!isReleaseVersion(installed)) return;
  return releases
    .filter((r) => (prereleases || !r.prerelease) && new Version(r.version).isGreater(installed))
    .sort((a, b) =>
      new Version(a.version).isGreater(b.version)
        ? -1
        : new Version(b.version).isGreater(a.version)
          ? 1
          : 0,
    )[0];
}
async function json(url: string): Promise<unknown> {
  const response = await fetch(url, { signal: AbortSignal.timeout(10000), credentials: 'omit' });
  if (!response.ok) throw new Error(`Changelog request failed: ${response.status}`);
  return response.json() as Promise<unknown>;
}
export async function loadNotes(
  installed: string,
): Promise<{ releases: ReleaseNotes[]; cached: boolean }> {
  let releases: ReleaseNotes[] = [];
  let cached = false;
  try {
    const feed = record(await json(FEED_URL));
    if (feed['schema'] !== 1) throw new Error('Unsupported feed');
    releases = parseNotes(feed['releases']);
    if (!releases.length) throw new Error('Empty feed');
  } catch {
    try {
      releases = parseNotes(await json(`${GITHUB}?per_page=100`), true);
    } catch {
      /* cached notes below */
    }
  }
  if (
    isReleaseVersion(installed) &&
    !releases.some((r) => versionKey(r.version) === versionKey(installed))
  ) {
    for (const tag of [versionKey(installed), `v${versionKey(installed)}`]) {
      try {
        const found = parseNotes([await json(`${GITHUB}/tags/${encodeURIComponent(tag)}`)], true);
        if (found.length) {
          releases.push(...found);
          break;
        }
      } catch {
        /* Tags may or may not use the v prefix. */
      }
    }
  }
  try {
    const old: unknown = JSON.parse(localStorage.getItem(CACHE_KEY) || '[]');
    const stored = parseNotes(old);
    if (!releases.length) {
      releases = stored;
      cached = releases.length > 0;
    } else {
      // Retain older installed releases for offline viewing, without replacing fresh notes.
      releases.push(
        ...stored.filter(
          (r) =>
            versionKey(r.version) === versionKey(installed) &&
            !releases.some((n) => versionKey(n.version) === versionKey(r.version)),
        ),
      );
      localStorage.setItem(CACHE_KEY, JSON.stringify(releases.slice(0, 500)));
    }
  } catch {
    /* Browser storage is optional; acknowledgements live on the host. */
  }
  return { releases, cached };
}
