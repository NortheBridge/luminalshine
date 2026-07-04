"""Sphinx configuration for LuminalShine.

Tunes the Furo theme to a **Microsoft Learn Dark Mode** layout with
the LuminalShine sunshine palette. Where the prior light theme used
royal-blue "Aurora Blue" for links and accents, this dark variant
uses the sunburst gradient from sunshine.png: deep red-orange
(#ff3d00) → mid-orange (#ff7a1a) → sun-gold (#ffb020). Sun-gold is
the primary hyperlink and focus color — high-contrast on the
near-black page background and unambiguously "brand."

The sidebar shows the LuminalShine logo image with the site title
"LuminalShine" rendered below it in the Segoe UI font stack.
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
html_logo = "_static/luminalshine-logo.png"
html_favicon = "_static/luminalshine-logo.png"
html_static_path = ["_static"]
html_css_files = ["luminalshine.css"]
html_js_files = ["breadcrumbs.js"]

# Microsoft Learn Dark palette with LuminalShine sunshine branding.
# The warm brand triple (red → orange → gold) replaces every spot that
# used blue/purple in the previous light theme. Sun-gold is the primary
# link/focus color; on the near-black page background it lands around
# a 12:1 contrast ratio (WCAG AAA).
_PALETTE = {
    # Dark surfaces
    "page_bg": "#111111",          # near-black page background
    "surface": "#1a1a1a",          # sidebar / TOC rail / code
    "surface_alt": "#242424",      # hover / softer surface
    "text": "#e6e6e6",             # primary body text
    "text_secondary": "#b8b8b8",
    "text_muted": "#8b8b8b",
    "border": "#2f2f2f",
    # Sunshine brand triple (extracted from sunshine.png)
    "brand_red": "#ff3d00",        # outer flare / gradient start
    "brand_orange": "#ff7a1a",     # mid-tone / warnings
    "brand_gold": "#ffb020",       # PRIMARY — links, focus, brand
    "brand_soft": "#ffc857",       # hover / soft accent
    "accent_soft": "rgba(255, 176, 32, 0.10)",  # selected nav bg tint
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

# Dark syntax highlighting in BOTH modes. Combined with identical
# light/dark CSS variables below, this pins the site to the Learn
# Dark look regardless of the visitor's OS preference (Furo would
# otherwise swap in its own dark Pygments sheet).
pygments_style = "monokai"
pygments_dark_style = "monokai"

_furo_vars = {
    "color-brand-primary": _PALETTE["brand_gold"],
    "color-brand-content": _PALETTE["brand_gold"],
    "color-background-primary": _PALETTE["page_bg"],
    "color-background-secondary": _PALETTE["surface"],
    "color-background-hover": _PALETTE["surface_alt"],
    "color-background-border": _PALETTE["border"],
    "color-foreground-primary": _PALETTE["text"],
    "color-foreground-secondary": _PALETTE["text_secondary"],
    "color-foreground-muted": _PALETTE["text_muted"],
    "color-foreground-border": _PALETTE["border"],
    "color-link": _PALETTE["brand_gold"],
    "color-link--hover": _PALETTE["brand_soft"],
    "color-link-underline": "transparent",
    "color-link-underline--hover": _PALETTE["brand_soft"],
    "color-highlight-on-target": "rgba(255, 176, 32, 0.18)",
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
    # Force the Learn Dark look in both modes — brand-gold accents are
    # tuned for the near-black background and the theme toggle is
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
