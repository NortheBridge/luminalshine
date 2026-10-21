#!/usr/bin/env python3
"""Import GitHub Releases into Featurebase Updates (changelogs).

Historical import:
  GITHUB_REPOSITORY=OWNER/REPO FEATUREBASE_API_KEY=sk_... \
    GITHUB_TOKEN=github_token python3 scripts/github_to_featurebase.py --history

Run from a GitHub release event (GITHUB_EVENT_PATH):
  python3 scripts/github_to_featurebase.py [--publish]

Created updates are drafts unless --publish is specified. The script skips
GitHub releases already linked in a Featurebase Update's body.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

FEATUREBASE_API_VERSION = "2026-08-19.orbit"
GITHUB_API_VERSION = "2026-03-10"
FEATUREBASE_URL = "https://do.featurebase.app/v2/changelogs"


def call_json(url, *, token=None, headers=None, method="GET", payload=None):
    request_headers = {"Accept": "application/json", "User-Agent": "github-featurebase-releases-sync"}
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
        raise RuntimeError(f"{method} {url} returned HTTP {exc.code}: {detail[:500]}") from exc


def github_releases(repo, token):
    page = 1
    while True:
        qs = urllib.parse.urlencode({"per_page": 100, "page": page})
        releases = call_json(
            f"https://api.github.com/repos/{repo}/releases?{qs}",
            token=token,
            headers={"X-GitHub-Api-Version": GITHUB_API_VERSION, "Accept": "application/vnd.github+json"},
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


def main():
    parser = argparse.ArgumentParser(description="Import GitHub Releases into Featurebase Updates")
    parser.add_argument("--history", action="store_true", help="Import all previously published GitHub releases")
    parser.add_argument("--publish", action="store_true", help="Publish imported drafts publicly without sending email")
    args = parser.parse_args()

    api_key = os.environ.get("FEATUREBASE_API_KEY")
    repo = os.environ.get("GITHUB_REPOSITORY")
    github_token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not api_key or not repo:
        parser.error("FEATUREBASE_API_KEY and GITHUB_REPOSITORY must be set")

    event_file = os.environ.get("GITHUB_EVENT_PATH")
    if event_file and not args.history:
        event = json.loads(Path(event_file).read_text(encoding="utf-8"))
        if "release" not in event:
            parser.error("GITHUB_EVENT_PATH is not a GitHub release event; pass --history for a historical import")
        releases = [event["release"]]
    else:
        releases = list(github_releases(repo, github_token))
        # Import oldest first, preserving the source publication date.
        releases.sort(key=lambda item: item.get("published_at") or "")

    # The Featurebase API defaults to 'live'. Include drafts to avoid re-imports.
    existing_texts = [
        "\n".join(filter(None, (entry.get("markdownContent"), entry.get("content"))))
        for entry in featurebase_changelogs(api_key)
    ]
    created = skipped = 0

    for release in releases:
        if release.get("draft") or not release.get("published_at"):
            skipped += 1
            continue
        url = release["html_url"]
        if any(url in text for text in existing_texts):
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
            "date": release["published_at"][:10],
            "state": "draft",
        }
        response = call_json(
            FEATUREBASE_URL, token=api_key, headers={"Featurebase-Version": FEATUREBASE_API_VERSION},
            method="POST", payload=payload,
        )
        print(f"Created draft: {title} (Featurebase ID: {response['id']})")
        created += 1
        existing_texts.append(markdown)

        if args.publish:
            call_json(
                f"{FEATUREBASE_URL}/{response['id']}/publish",
                token=api_key, headers={"Featurebase-Version": FEATUREBASE_API_VERSION},
                method="POST", payload={"sendEmail": False},
            )
            print(f"Published (email notifications disabled): {title}")

    print(f"Done. Created: {created}. Skipped: {skipped}.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
