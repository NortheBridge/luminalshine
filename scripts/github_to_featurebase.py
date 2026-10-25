#!/usr/bin/env python3
"""Import GitHub Releases and synchronize their dates with Featurebase Updates.

Examples:
  python3 scripts/github_to_featurebase.py --history
  python3 scripts/github_to_featurebase.py --sync-dates                # dry run
  python3 scripts/github_to_featurebase.py --sync-dates --apply-dates  # update dates
  python3 scripts/github_to_featurebase.py --publish                   # release event
  python3 scripts/github_to_featurebase.py --export-feed changelog.json # public WebUI feed

Environment: FEATUREBASE_API_KEY and GITHUB_REPOSITORY=OWNER/REPO.
For private GitHub repositories, also set GITHUB_TOKEN (Actions supplies it).

Existing Featurebase updates are matched by their own dedicated
"Original GitHub release" citation, not every release link mentioned in
the body. Unmatched Featurebase updates are never modified.
"""

import argparse
from collections import defaultdict
from html import unescape
from html.parser import HTMLParser
import re
from datetime import datetime, timezone
import json
import xml.etree.ElementTree as ET
import os
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request

FEATUREBASE_API_VERSION = "2026-08-19.orbit"
GITHUB_API_VERSION = "2026-03-10"
FEATUREBASE_URL = "https://do.featurebase.app/v2/changelogs"


def call_json(url, *, token=None, headers=None, method="GET", payload=None):
    request_headers = {
        "Accept": "application/json",
        "User-Agent": "github-featurebase-releases-sync",
    }
    if token:
        request_headers["Authorization"] = f"Bearer {token}"
    if headers:
        request_headers.update(headers)
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(
            f"{method} {url} returned HTTP {exc.code}: {detail[:500]}"
        ) from exc


def github_releases(repo, token):
    page = 1
    while True:
        qs = urllib.parse.urlencode({"per_page": 100, "page": page})
        releases = call_json(
            f"https://api.github.com/repos/{repo}/releases?{qs}",
            token=token,
            headers={
                "X-GitHub-Api-Version": GITHUB_API_VERSION,
                "Accept": "application/vnd.github+json",
            },
        )
        if not isinstance(releases, list):
            raise RuntimeError("GitHub returned an unexpected releases response")
        yield from releases
        if len(releases) < 100:
            break
        page += 1


def featurebase_changelogs(api_key):
    cursor = None
    while True:
        qs = {"state": "all", "limit": 100}
        if cursor:
            qs["cursor"] = cursor
        data = call_json(
            f"{FEATUREBASE_URL}?{urllib.parse.urlencode(qs)}",
            token=api_key,
            headers={"Featurebase-Version": FEATUREBASE_API_VERSION},
        )
        yield from data["data"]
        cursor = data.get("nextCursor")
        if not cursor:
            break


def utc_date(value):
    """Normalize GitHub / Featurebase ISO 8601 dates to UTC timestamps."""
    if not value:
        return None
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def github_timestamp(release):
    """Keep the original GitHub publication *time*, not just YYYY-MM-DD."""
    stamp = utc_date(release.get("published_at"))
    return stamp.isoformat(timespec="seconds").replace("+00:00", "Z") if stamp else None


# The importer always appends this *dedicated* source link to each Update.
# A simple `release_url in entry_content` check is unsafe: release notes often
# link to earlier releases, causing false matches and skipped date repairs.
SOURCE_LABEL = "original github release"
MARKDOWN_SOURCE_LINK = re.compile(
    r"\[\s*original\s+github\s+release\s*\]\(\s*(<[^>]+>|https?://[^\s)]+)",
    flags=re.IGNORECASE,
)


class SourceAnchorParser(HTMLParser):
    """Extract only HTML anchors labelled 'Original GitHub release'."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.active_href = None
        self.active_label = []
        self.urls = []

    def handle_starttag(self, tag, attrs):
        if tag.lower() == "a":
            self.active_href = dict(attrs).get("href")
            self.active_label = []

    def handle_data(self, data):
        if self.active_href:
            self.active_label.append(data)

    def handle_endtag(self, tag):
        if tag.lower() == "a":
            label = " ".join("".join(self.active_label).lower().split())
            if self.active_href and label == SOURCE_LABEL:
                self.urls.append(self.active_href)
            self.active_href = None
            self.active_label = []


def release_key(url):
    """Return a canonical (owner, repo, release-tag) key for a GitHub URL."""
    parts = urllib.parse.urlsplit(unescape(url.strip().strip("<>")))
    if parts.scheme not in ("https", "http") or (parts.hostname or "").lower() not in (
        "github.com", "www.github.com",
    ):
        return None
    path = urllib.parse.unquote(parts.path).strip("/").split("/")
    if len(path) != 5 or path[2:4] != ["releases", "tag"]:
        return None
    return (path[0].lower(), path[1].lower(), path[4])


def source_release_keys(entry):
    """Identify this Update's OWN GitHub release (never links in its notes)."""
    keys = set()
    md = entry.get("markdownContent") or ""
    for match in MARKDOWN_SOURCE_LINK.finditer(md):
        key = release_key(match.group(1))
        if key:
            keys.add(key)

    html = entry.get("content") or ""
    if html:
        parser = SourceAnchorParser()
        parser.feed(html)
        parser.close()
        for url in parser.urls:
            key = release_key(url)
            if key:
                keys.add(key)
    return keys


def index_featurebase_entries(entries):
    """Index changelogs by their exact dedicated source-release citation."""
    by_release = defaultdict(list)
    for entry in entries:
        for key in source_release_keys(entry):
            by_release[key].append(entry)
    return by_release


def change_date(api_key, featurebase_id, timestamp):
    return call_json(
        f"{FEATUREBASE_URL}/{featurebase_id}",
        token=api_key,
        headers={"Featurebase-Version": FEATUREBASE_API_VERSION},
        method="PATCH",
        payload={"date": timestamp},
    )


def same_time(first, second):
    a, b = utc_date(first), utc_date(second)
    return a is not None and b is not None and abs((a - b).total_seconds()) < 1


def check_returned_timestamp(data, expected, title):
    actual = data.get("date")
    if actual and not same_time(actual, expected):
        print(
            f"WARNING: Featurebase returned date {actual!r} for {title!r}, "
            f"rather than {expected!r}. Exact timestamp preservation may "
            "not be supported by this workspace/API version.",
            file=sys.stderr,
        )


def sync_dates(releases, entries, api_key, *, apply):
    """Match by original release URL, then audit or correct published dates."""
    matched = corrections = already_correct = missing = ambiguous = 0
    by_release = index_featurebase_entries(entries)
    for release in releases:
        timestamp = github_timestamp(release)
        if release.get("draft") or not timestamp:
            continue
        url = release["html_url"]
        candidates = by_release.get(release_key(url), [])
        if not candidates:
            print(f"UNMATCHED (no Original GitHub release citation): {url}")
            missing += 1
            continue
        if len(candidates) != 1:
            details = ", ".join(
                f"{e.get('title', 'Untitled')!r} [{e.get('id')}]" for e in candidates
            )
            print(f"SKIPPING duplicate source citation ({len(candidates)} posts): {url}: {details}")
            ambiguous += 1
            continue
        entry = candidates[0]
        matched += 1
        current = entry.get("date")
        if same_time(current, timestamp):
            already_correct += 1
            continue
        title = entry.get("title", release.get("tag_name", url))
        if apply:
            result = change_date(api_key, entry["id"], timestamp)
            check_returned_timestamp(result, timestamp, title)
            print(f"UPDATED: {title}: {current} -> {timestamp}")
        else:
            print(f"WOULD UPDATE: {title}: {current} -> {timestamp}")
        corrections += 1
    print(
        f"Done. Matched: {matched}; already correct: {already_correct}; "
        f"{'updated' if apply else 'would update'}: {corrections}; "
        f"unmatched GitHub releases: {missing}; ambiguous matches: {ambiguous}."
    )
    if not apply:
        print("Dry run only. Rerun with --sync-dates --apply-dates to apply corrections.")


def import_releases(releases, entries, api_key, *, publish):
    existing_keys = set(index_featurebase_entries(entries))
    created = skipped = 0
    for release in releases:
        timestamp = github_timestamp(release)
        if release.get("draft") or not timestamp:
            skipped += 1
            continue
        url = release["html_url"]
        if release_key(url) in existing_keys:
            print(f"Skipping already imported: {url}")
            skipped += 1
            continue

        tag = release["tag_name"]
        name = (release.get("name") or "").strip()
        title = name or tag
        if tag not in title:
            title = f"{title} ({tag})"
        markdown = (release.get("body") or "Release notes not provided.").strip()
        markdown += f"\n\n---\n[Original GitHub release]({url})"
        payload = {
            "title": title,
            "markdownContent": markdown,
            "date": timestamp,
            "state": "draft",
        }
        result = call_json(
            FEATUREBASE_URL,
            token=api_key,
            headers={"Featurebase-Version": FEATUREBASE_API_VERSION},
            method="POST",
            payload=payload,
        )
        check_returned_timestamp(result, timestamp, title)
        print(f"Created draft: {title} (Featurebase ID: {result['id']})")
        created += 1
        existing_keys.add(release_key(url))

        if publish:
            published = call_json(
                f"{FEATUREBASE_URL}/{result['id']}/publish",
                token=api_key,
                headers={"Featurebase-Version": FEATUREBASE_API_VERSION},
                method="POST",
                payload={"sendEmail": False},
            )
            # Correct it again if Featurebase changed the timestamp upon publish.
            if not same_time(published.get("date"), timestamp):
                published = change_date(api_key, result["id"], timestamp)
            check_returned_timestamp(published, timestamp, title)
            print(f"Published without subscriber email: {title}")

    print(f"Done. Created: {created}; skipped: {skipped}.")


class PublicFeaturebaseRedirect(urllib.request.HTTPRedirectHandler):
    """An anonymous request must stay on Featurebase's public hosted domains."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not featurebase_origin(newurl):
            raise RuntimeError("Featurebase RSS redirected outside its public hosted domains")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def featurebase_origin(url):
    """Discover only public hosted origins; never turn API content into arbitrary requests."""
    if not isinstance(url, str):
        return None
    parsed = urllib.parse.urlsplit(url)
    host = parsed.hostname or ""
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or parsed.netloc != host or not host.endswith(".featurebase.app")
            or not re.fullmatch(r"[a-z0-9-]+\.featurebase\.app", host)):
        return None
    return "https://" + host


def public_featurebase_entries(origin):
    """Read only anonymously visible RSS content, never privileged API bodies.

    The RSS URL is advertised by Featurebase's public Updates page. Descriptions
    are Markdown. Its history can be shorter than GitHub's; older entries use
    GitHub notes. No token or cookie is attached, including on redirects.
    """
    request = urllib.request.Request(
        origin + "/api/v1/changelog/feed.rss",
        headers={"Accept": "application/rss+xml", "User-Agent": "luminalshine-changelog-feed"},
    )
    opener = urllib.request.build_opener(PublicFeaturebaseRedirect())
    with opener.open(request, timeout=30) as response:
        raw = response.read(8 * 1024 * 1024 + 1)
    if len(raw) > 8 * 1024 * 1024:
        raise RuntimeError("Featurebase RSS exceeds the feed size limit")
    root = ET.fromstring(raw)
    if root.tag != "rss":
        raise ValueError("Featurebase did not return an RSS feed")
    return [
        {"title": item.findtext("title", ""),
         "markdown": item.findtext("description", ""),
         "url": item.findtext("link", "")}
        for item in root.findall("channel/item")
        if featurebase_origin(item.findtext("link", ""))
    ]


def matches_release(markdown, url):
    # Reuse the upstream dedicated-source matcher: incidental links to older
    # releases must not attach another release's Featurebase notes.
    key = release_key(url)
    return key is not None and key in source_release_keys({
        "markdownContent": markdown, "content": markdown,
    })


def build_public_feed(releases, public_entries):
    """Project public sources onto an explicit schema; no private API fields."""
    output = []
    for release in releases:
        timestamp = github_timestamp(release)
        if release.get("draft") or not timestamp:
            continue
        url = release["html_url"]
        matches = [entry for entry in public_entries if matches_release(entry["markdown"], url)]
        # Ambiguous Featurebase mappings fall back instead of choosing arbitrary content.
        entry = matches[0] if len(matches) == 1 else None
        output.append({
            "version": release["tag_name"],
            "title": (entry["title"] if entry else release.get("name")) or release["tag_name"],
            "markdown": (entry["markdown"] if entry else release.get("body")) or "Release notes not provided.",
            "featurebase_url": entry["url"] if entry else "",
            "github_url": url,
            "published_at": timestamp,
            "prerelease": release.get("prerelease") is True,
        })
    output.sort(key=lambda entry: entry["published_at"], reverse=True)
    return {"schema": 1, "releases": output}


def export_public_feed(repo, api_key, destination):
    # Intentionally anonymous: a private repository or draft cannot accidentally
    # become a public data branch through the Actions token.
    releases = list(github_releases(repo, None))
    public_entries = []
    origins = set()
    if api_key:
        # Privileged data is used ONLY to discover a hosted public origin. Never
        # export API titles/content, even for live posts: workspace, category and
        # segment visibility can still make them private.
        try:
            for entry in featurebase_changelogs(api_key):
                origin = featurebase_origin(entry.get("url"))
                organization = entry.get("organization", "")
                if not origin and isinstance(organization, str):
                    origin = featurebase_origin(f"https://{organization}.featurebase.app")
                if origin:
                    origins.add(origin)
        except (RuntimeError, OSError, ValueError, KeyError):
            print("WARNING: Featurebase discovery unavailable; using GitHub notes.", file=sys.stderr)
    for origin in sorted(origins):
        try:
            public_entries.extend(public_featurebase_entries(origin))
        except (RuntimeError, OSError, ValueError, ET.ParseError):
            print("WARNING: Public Featurebase RSS unavailable; using GitHub notes.", file=sys.stderr)
    # An organization can advertise both a hosted and alias origin.
    public_entries = list({entry["url"]: entry for entry in public_entries}.values())
    feed = build_public_feed(releases, public_entries)
    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(feed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {len(feed['releases'])} public releases to {target}")


def main():
    parser = argparse.ArgumentParser(
        description="Import GitHub Releases and synchronize their Featurebase publish dates"
    )
    parser.add_argument("--history", action="store_true", help="Import all published GitHub Releases")
    parser.add_argument("--publish", action="store_true", help="Publish new imports without sending subscriber emails")
    parser.add_argument(
        "--sync-dates", action="store_true",
        help="Compare existing Featurebase publish timestamps against GitHub (dry run)",
    )
    parser.add_argument(
        "--apply-dates", action="store_true",
        help="Apply --sync-dates corrections; touches dates only, not content",
    )
    parser.add_argument("--export-feed", metavar="PATH", help="Export public version-indexed WebUI notes without importing or publishing")
    args = parser.parse_args()
    if args.export_feed and (args.history or args.publish or args.sync_dates or args.apply_dates):
        parser.error("--export-feed cannot be combined with import or date operations")
    if args.apply_dates and not args.sync_dates:
        parser.error("--apply-dates requires --sync-dates")
    if args.sync_dates and (args.history or args.publish):
        parser.error("--sync-dates cannot be combined with --history or --publish")

    api_key = os.environ.get("FEATUREBASE_API_KEY")
    repo = os.environ.get("GITHUB_REPOSITORY")
    github_token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if args.export_feed:
        if not repo:
            parser.error("GITHUB_REPOSITORY must be set")
        export_public_feed(repo, api_key, args.export_feed)
        return
    if not api_key or not repo:
        parser.error("FEATUREBASE_API_KEY and GITHUB_REPOSITORY must be set")

    event_file = os.environ.get("GITHUB_EVENT_PATH")
    if event_file and not args.history and not args.sync_dates:
        event = json.loads(Path(event_file).read_text(encoding="utf-8"))
        if "release" not in event:
            parser.error("GITHUB_EVENT_PATH is not a GitHub release event; pass --history")
        releases = [event["release"]]
    else:
        releases = list(github_releases(repo, github_token))
        releases.sort(key=lambda item: item.get("published_at") or "")

    entries = list(featurebase_changelogs(api_key))
    if args.sync_dates:
        sync_dates(releases, entries, api_key, apply=args.apply_dates)
    else:
        import_releases(releases, entries, api_key, publish=args.publish)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
