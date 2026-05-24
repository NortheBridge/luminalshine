#!/usr/bin/env python3
"""Generate the LuminalShine changelog page from GitHub Releases.

Fetches every release for NortheBridge/luminalshine, converts
GitHub-flavoured alerts (`> [!IMPORTANT]`) into MyST admonitions, and
writes the result to docs/_generated/changelog_releases.md, which
docs/changelog.md includes.

Invoked automatically from docs/conf.py on every Sphinx build (both
local and Read the Docs). Falls back to a placeholder page if the
GitHub API is unreachable, so the doc build never breaks on a
network hiccup.

Reads an optional bearer token from $GH_TOKEN or $GITHUB_TOKEN to
sidestep the anonymous 60-req/hr rate limit; not required for normal
builds.
"""

from __future__ import annotations

import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO = "NortheBridge/luminalshine"
OUT_PATH = (
    Path(__file__).resolve().parent.parent / "_generated" / "changelog_releases.md"
)

# GitHub alert kind → MyST admonition name. CAUTION maps to "danger"
# because Furo styles "danger" red, matching GitHub's caution colour.
ALERT_TO_ADMONITION = {
    "NOTE": "note",
    "TIP": "tip",
    "IMPORTANT": "important",
    "WARNING": "warning",
    "CAUTION": "danger",
}

_ALERT_RE = re.compile(r"^>\s*\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*$")
_BLOCKQUOTE_LINE_RE = re.compile(r"^>\s?")
_HEADING_RE = re.compile(r"^(#{1,6})(\s+\S)")
_FENCE_RE = re.compile(r"^(\s*)(```+|~~~+)")


def _fetch_releases() -> list[dict]:
    """Hit the GitHub Releases API once and return the parsed list."""
    url = f"https://api.github.com/repos/{REPO}/releases?per_page=100"
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": f"{REPO}-docs-build",
    }
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def _convert_github_alerts(body: str) -> str:
    """Rewrite GitHub alert blockquotes into MyST colon-fenced admonitions.

    Colon fences are used (not backtick fences) because release bodies
    often contain triple-backtick code blocks, and nested same-character
    fences confuse MyST.
    """
    lines = body.split("\n")
    out: list[str] = []
    i = 0
    while i < len(lines):
        match = _ALERT_RE.match(lines[i])
        if not match:
            out.append(lines[i])
            i += 1
            continue
        kind = ALERT_TO_ADMONITION[match.group(1)]
        j = i + 1
        content: list[str] = []
        while j < len(lines) and lines[j].startswith(">"):
            content.append(_BLOCKQUOTE_LINE_RE.sub("", lines[j]))
            j += 1
        # Strip surrounding blank lines inside the alert
        while content and not content[0].strip():
            content.pop(0)
        while content and not content[-1].strip():
            content.pop()
        out.append(f":::{{{kind}}}")
        out.extend(content)
        out.append(":::")
        i = j
    return "\n".join(out)


def _demote_headings(body: str, levels: int = 1) -> str:
    """Demote ATX headings by `levels`, skipping fenced code blocks.

    Release bodies routinely start with their own H2 title (e.g.
    "## LuminalShine 26.05.0 Update Release Candidate 3 Hotfix 4")
    that collides with the H2 this script emits per release. Demoting
    pushes the body into H3+ so the page hierarchy stays one release =
    one H2, and the per-page TOC stays readable.
    """
    out: list[str] = []
    in_fence = False
    fence_marker: str | None = None
    for line in body.split("\n"):
        fence = _FENCE_RE.match(line)
        if fence:
            marker = fence.group(2)[:3]  # ``` or ~~~
            if not in_fence:
                in_fence = True
                fence_marker = marker
            elif fence_marker and line.lstrip().startswith(fence_marker):
                in_fence = False
                fence_marker = None
            out.append(line)
            continue
        if not in_fence:
            match = _HEADING_RE.match(line)
            if match:
                old_hashes = match.group(1)
                new_level = min(len(old_hashes) + levels, 6)
                # Replace only the leading hashes; preserve the rest of
                # the line verbatim (the prior version reconstructed
                # `line` from the match groups, which dropped every
                # character of the title past the first).
                line = "#" * new_level + line[len(old_hashes):]
        out.append(line)
    return "\n".join(out)


def _slugify(tag: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", tag.lower()).strip("-") or "release"


def _format_release(rel: dict) -> str:
    tag = rel["tag_name"]
    name = rel.get("name") or tag
    date = (rel.get("published_at") or "").split("T")[0]
    is_pre = rel.get("prerelease", False)
    is_draft = rel.get("draft", False)
    html_url = rel.get("html_url") or f"https://github.com/{REPO}/releases/tag/{tag}"
    raw_body = (rel.get("body") or "").strip() or "_No release notes provided._"
    body = _demote_headings(_convert_github_alerts(raw_body), levels=1)

    badges: list[str] = []
    if is_draft:
        badges.append("**Draft**")
    if is_pre:
        badges.append("**Pre-release**")
    badge_line = " · ".join(badges)

    parts: list[str] = []
    parts.append(f"(release-{_slugify(tag)})=")
    parts.append(f"## {name}")
    parts.append("")
    meta = f"**Tag:** [`{tag}`]({html_url})"
    if date:
        meta += f" · **Released:** {date}"
    if badge_line:
        meta += f" · {badge_line}"
    parts.append(meta)
    parts.append("")
    parts.append(body)
    parts.append("")
    return "\n".join(parts)


def _write_placeholder(reason: str) -> None:
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUT_PATH.write_text(
        "<!-- Auto-generated placeholder. -->\n\n"
        ":::{warning}\n"
        "The GitHub Releases API could not be reached when this page was built.\n"
        f"See [github.com/{REPO}/releases](https://github.com/{REPO}/releases)\n"
        "for the canonical release list.\n"
        f"\n_Reason:_ `{reason}`\n"
        ":::\n",
        encoding="utf-8",
    )


def main() -> int:
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        releases = _fetch_releases()
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as exc:
        print(f"build_changelog: GitHub fetch failed: {exc}", file=sys.stderr)
        _write_placeholder(str(exc))
        return 0

    if not releases:
        OUT_PATH.write_text(
            "<!-- Auto-generated. -->\n\n"
            "No releases have been published yet. Watch "
            f"[github.com/{REPO}/releases](https://github.com/{REPO}/releases) "
            "for the first one.\n",
            encoding="utf-8",
        )
        print("build_changelog: no releases found", file=sys.stderr)
        return 0

    header = (
        "<!-- This file is regenerated on every documentation build by "
        "docs/_scripts/build_changelog.py from the GitHub Releases API. "
        "DO NOT EDIT MANUALLY — changes will be overwritten. -->\n"
    )
    body = "\n".join(_format_release(r) for r in releases)
    OUT_PATH.write_text(header + "\n" + body, encoding="utf-8")
    print(
        f"build_changelog: wrote {len(releases)} releases to {OUT_PATH}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
