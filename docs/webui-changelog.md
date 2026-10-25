# WebUI changelog feed

The WebUI reads `https://raw.githubusercontent.com/NortheBridge/luminalshine/changelog-feed/changelog.json`.
The dedicated `changelog-feed` branch contains generated public data. It does not replace `main`, release tags, or GitHub Pages.

## Publication

After this workflow is merged, run **Import GitHub Releases into Featurebase → Run workflow → export-feed** once to create the feed branch. Subsequent GitHub release publications and the existing daily reconciliation refresh it. Publishing or editing a Featurebase update between those runs can be reflected immediately by running `export-feed` again.

The repository's `FEATUREBASE_API_KEY` Actions secret is used only in Actions to discover the public Featurebase workspace. The UI and feed never contain the key. The existing import workflow still creates drafts; this change does not automatically publish those drafts or send email.

The feed publishing job needs `contents: write` and permission to update the `changelog-feed` branch. It creates normal commits and never force-pushes. Protect `main` normally; branch policies must permit the workflow's data-branch commits. GitHub Pages is not required.

## Public content boundary

API responses can include private workspaces, drafts, hidden posts, category restrictions, and segment restrictions. Consequently, **no authenticated Featurebase title or body is exported**. API records only discover a hosted `*.featurebase.app` origin (or its organization slug). The exporter then fetches `/api/v1/changelog/feed.rss` anonymously, without cookies or API credentials, and accepts redirects only to hosted Featurebase origins. This RSS endpoint is advertised by Featurebase's public Updates page.

Each public RSS entry must contain the exact original GitHub release URL inserted by the importer. A uniquely matching entry supplies its public Markdown/title/link. Ambiguous or absent matches use GitHub release notes instead. Featurebase's public RSS history may be limited; older releases therefore use GitHub notes. Custom domains resolve through the API's organization slug when available; otherwise the export safely falls back to GitHub notes.

GitHub releases are also fetched anonymously. Drafts and unpublished entries are excluded. The `prerelease` flag and exact version tag always come from GitHub, rather than being inferred from Featurebase titles. Failure of Featurebase discovery or RSS uses GitHub notes; failure to fetch GitHub aborts publication and leaves the previous data branch intact.

Schema:

```json
{
  "schema": 1,
  "releases": [{
    "version": "26.10.0",
    "title": "26.10.0",
    "markdown": "Release notes",
    "featurebase_url": "",
    "github_url": "https://github.com/NortheBridge/luminalshine/releases/tag/26.10.0",
    "published_at": "2026-10-25T12:00:00Z",
    "prerelease": false
  }]
}
```

`featurebase_url` is empty when notes come from GitHub. Removing or hiding an Update removes its Featurebase content from the next exported feed if it is no longer publicly served by RSS. Because this is a public Git data branch, prior feed revisions remain in Git history: do not publish confidential information to the public Updates page.

## Verification

Run the offline exporter tests:

```bash
python3 -B -m unittest discover -s tests/scripts -p 'test_changelog_feed.py'
```

Export locally without an API key to verify the GitHub fallback:

```bash
GITHUB_REPOSITORY=NortheBridge/luminalshine python3 scripts/github_to_featurebase.py --export-feed /tmp/luminalshine-changelog.json
```

No public API calls or publishing occur in the unit tests.

## WebUI behavior and acknowledgements

The shared modal is available from the desktop navigation and mobile menu. An eligible newer release displays **New Update Available!**, with a download link to its GitHub release. Stable/prerelease selection follows the host's existing `notify_pre_releases` setting. On the first authenticated visit for an unseen installed version, the modal instead displays **Changelog** with that installed version's notes. The installed-version screen takes priority; the update banner remains available without opening a second automatic popup in the same session. A first visit on a new installation also displays its installed release notes once.

Acknowledgements use authenticated `GET /api/changelog/state` and `POST /api/changelog/ack`. The host derives the username from the session; admin and stats accounts can acknowledge their own notes. State is stored in `<luminalshine_state_path>.changelog.json` with atomic replacement and backup recovery. It survives normal upgrades, restarts, browser storage clearing, and use of another browser/address for the same host and username. Deleting the host state or changing usernames resets the corresponding history. Up to 256 installed versions are retained per user.

The installed version is acknowledged only after nonempty, sanitized notes render. Reading a newer release before installation does not acknowledge it as installed. Closing the available-update popup records a separate dismissal. Unavailable notes are not acknowledged, and failed saves can be retried. If two browsers first open concurrently before either save arrives, both can display; subsequent visits observe the shared saved acknowledgement.

Public notes are cached in browser storage for offline reading; browser storage never determines whether the installed-version popup has been seen. The WebUI falls back to GitHub when the feed is unavailable. If host acknowledgement state is unavailable, automatic popups are suppressed until it can be read. Returning focus to the WebUI checks for changes at most once every 15 minutes.

Frontend tests: `npm test -- changelog.test.ts` from `src_assets/common/assets/web`.
Native state tests: `test_sunshine --gtest_filter='ChangelogStateTest.*'` in a configured build.
