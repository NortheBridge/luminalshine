"""Sphinx configuration for LuminalShine.

Tunes the Furo theme to a light, content-first layout modelled on
Microsoft Learn (learn.microsoft.com): white surfaces, Segoe UI
typography, generous whitespace, breadcrumbs and an "In this article"
rail. NortheBridge branding is preserved through "Aurora Blue" — the
royal-blue #4a7dff link/accent and the cyan->blue brand duo — used
wherever the old dark theme leaned on purple/violet.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# -- Regenerate the changelog from GitHub Releases --------------------------
# Runs on every Sphinx build (local + Read the Docs). Soft-fails so a
# transient network issue doesn't break the docs build — see the script
# for the placeholder behavior.
_CHANGELOG_SCRIPT = Path(__file__).parent / "_scripts" / "build_changelog.py"
if _CHANGELOG_SCRIPT.is_file():
    try:
        subprocess.run(
            [sys.executable, str(_CHANGELOG_SCRIPT)],
            check=False,
            timeout=60,
        )
    except Exception as exc:  # pragma: no cover - defensive
        print(f"conf.py: changelog regeneration skipped ({exc})", file=sys.stderr)

# -- Project ----------------------------------------------------------------

project = "LuminalShine"
author = "NortheBridge"
copyright = "2026, NortheBridge"
release = "0.0.0"

# -- General ----------------------------------------------------------------

extensions = [
    "myst_parser",
    "sphinx.ext.autodoc",
    "sphinx.ext.intersphinx",
    "sphinx.ext.viewcode",
    "sphinx_copybutton",
    "sphinx_design",
]

# Note on the C++ API: we publish the standalone Doxygen HTML browser
# under /api/ alongside this site (see .readthedocs.yaml). Breathe was
# tried but its sphinxrenderer asserts on certain LuminalShine
# namespace shapes under Sphinx's parallel build.

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

master_doc = "index"
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "**/node_modules"]

myst_enable_extensions = [
    "colon_fence",
    "deflist",
    "linkify",
    "substitution",
    "tasklist",
]
myst_heading_anchors = 4

# The auto-generated changelog (docs/_generated/changelog_releases.md)
# embeds release-note Markdown authored on GitHub. Two warning classes
# are systematic false positives there and would otherwise drown out
# real warnings: heading hierarchy ("Document headings start at H2";
# the file is included into changelog.md which owns H1), and literal
# example text like "[link](url)" that myst treats as a broken xref.
# Neither blocks the build (fail_on_warning: false) but suppressing
# them keeps build logs scannable.
suppress_warnings = [
    "myst.header",
    "myst.xref_missing",
]

# -- HTML / Furo theme ------------------------------------------------------

html_theme = "furo"
html_title = "LuminalShine"
html_static_path = ["_static"]
html_css_files = ["luminalshine.css"]
html_js_files = ["breadcrumbs.js"]

# Microsoft Learn-style light palette with NortheBridge "Aurora Blue"
# branding. The cyan/blue values are the aurora-1/aurora-2 brand colors;
# aurora-3 (violet/#8a5cff) is intentionally retired — every spot that
# used purple now uses Aurora Blue.
_PALETTE = {
    "page_bg": "#ffffff",
    "surface": "#f7f9fc",          # sidebar / toc rail surface
    "surface_alt": "#eef2f8",      # hover, code-block chrome
    "text": "#1b1f24",
    "text_secondary": "#4a5563",
    "text_muted": "#6b7280",
    "border": "#e3e8ef",
    "aurora_cyan": "#1ec8ff",      # aurora-1 — brand duo start
    "aurora_blue": "#4a7dff",      # aurora-2 — "Aurora Blue", links/accents
    "aurora_blue_hover": "#2f5fe0",
    "accent_soft": "#eef3ff",      # selected nav item / note tint
}

# Segoe UI first to match the Microsoft Learn reading experience; system
# fallbacks keep it native on non-Windows visitors.
_FONT_STACK = (
    '"Segoe UI Variable Text", "Segoe UI", -apple-system, '
    'BlinkMacSystemFont, system-ui, Roboto, "Helvetica Neue", Arial, '
    "sans-serif"
)
_MONO_STACK = (
    '"Cascadia Code", "Cascadia Mono", SFMono-Regular, Menlo, '
    'Consolas, "Liberation Mono", monospace'
)

# Light syntax highlighting in BOTH modes. Combined with identical
# light/dark CSS variables below, this pins the site to the light
# Microsoft Learn look regardless of the visitor's OS dark-mode
# preference (Furo would otherwise swap in a dark Pygments sheet).
pygments_style = "friendly"
pygments_dark_style = "friendly"

_furo_vars = {
    "color-brand-primary": _PALETTE["aurora_blue"],
    "color-brand-content": _PALETTE["aurora_blue"],
    "color-background-primary": _PALETTE["page_bg"],
    "color-background-secondary": _PALETTE["surface"],
    "color-background-hover": _PALETTE["surface_alt"],
    "color-background-border": _PALETTE["border"],
    "color-foreground-primary": _PALETTE["text"],
    "color-foreground-secondary": _PALETTE["text_secondary"],
    "color-foreground-muted": _PALETTE["text_muted"],
    "color-foreground-border": _PALETTE["border"],
    "color-link": _PALETTE["aurora_blue"],
    "color-link--hover": _PALETTE["aurora_blue_hover"],
    "color-link-underline": "transparent",
    "color-link-underline--hover": _PALETTE["aurora_blue_hover"],
    "color-highlight-on-target": "rgba(74, 125, 255, 0.12)",
    "color-api-background": _PALETTE["surface"],
    "color-api-background-hover": _PALETTE["surface_alt"],
    "color-admonition-background": _PALETTE["page_bg"],
    "color-sidebar-background": _PALETTE["page_bg"],
    "color-sidebar-background-border": _PALETTE["border"],
    "color-sidebar-link-text": _PALETTE["text_secondary"],
    "color-sidebar-link-text--top-level": _PALETTE["text"],
    "color-sidebar-item-background--current": _PALETTE["accent_soft"],
    "color-sidebar-item-background--hover": _PALETTE["surface_alt"],
    "font-stack": _FONT_STACK,
    "font-stack--monospace": _MONO_STACK,
}

html_theme_options = {
    # Force the light Microsoft Learn look in both modes — Aurora Blue
    # accents are tuned for a white background and the theme toggle is
    # hidden in luminalshine.css.
    "light_css_variables": _furo_vars,
    "dark_css_variables": _furo_vars,
    "sidebar_hide_name": False,
    "navigation_with_keys": True,
    "top_of_page_buttons": ["view", "edit"],
    "source_repository": "https://github.com/NortheBridge/luminalshine",
    "source_branch": "main",
    "source_directory": "docs/",
    "footer_icons": [
        {
            "name": "GitHub",
            "url": "https://github.com/NortheBridge/luminalshine",
            "html": "",
            "class": "fa-brands fa-github fa-2x",
        },
    ],
}
