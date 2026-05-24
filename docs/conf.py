"""Sphinx configuration for LuminalShine.

Tunes the Furo theme to the NortheBridge "aurora" palette so the rendered
docs match https://gitdocs.northebridge.com/luminalshine: a near-black
background with cyan (#1ec8ff) and royal-blue (#4a7dff) accents on Inter.
"""

from __future__ import annotations

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
    "breathe",
]

# -- Breathe (Doxygen XML → Sphinx) -----------------------------------------
# Doxygen XML is emitted by docs/Doxyfile-xml during the RTD pre_build
# step (and by `doxygen Doxyfile-xml` locally). If the XML directory
# doesn't exist yet, breathe directives render an empty block with a
# warning instead of failing the build.
breathe_projects = {"luminalshine": "_doxygen/xml"}
breathe_default_project = "luminalshine"
breathe_default_members = ("members", "undoc-members")
breathe_show_include = True

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

# -- HTML / Furo theme ------------------------------------------------------

html_theme = "furo"
html_title = "LuminalShine"
html_static_path = ["_static"]
html_css_files = ["luminalshine.css"]

# Aurora palette from gitdocs.northebridge.com/luminalshine/assets/css/site.css
_AURORA = {
    "bg_base": "#04060f",
    "bg_deep": "#070b1f",
    "blue_primary": "#1ec8ff",   # --aurora-1, cyan-blue
    "blue_secondary": "#4a7dff", # --aurora-2, royal blue
    "violet": "#8a5cff",         # --aurora-3
    "teal": "#00e0c6",           # --aurora-4
    "text_primary": "#f3f6ff",
    "text_secondary": "rgba(225, 232, 255, 0.78)",
    "text_muted": "rgba(180, 195, 230, 0.65)",
    "border": "rgba(255, 255, 255, 0.12)",
    "glass": "rgba(20, 30, 60, 0.55)",
}

_FONT_STACK = (
    '"Inter", -apple-system, BlinkMacSystemFont, "Segoe UI Variable", '
    '"Segoe UI", system-ui, sans-serif'
)
_MONO_STACK = (
    '"JetBrains Mono", "Fira Code", SFMono-Regular, Menlo, '
    "Consolas, monospace"
)

_furo_vars = {
    "color-brand-primary": _AURORA["blue_primary"],
    "color-brand-content": _AURORA["blue_primary"],
    "color-background-primary": _AURORA["bg_base"],
    "color-background-secondary": _AURORA["bg_deep"],
    "color-background-hover": _AURORA["glass"],
    "color-background-border": _AURORA["border"],
    "color-foreground-primary": _AURORA["text_primary"],
    "color-foreground-secondary": _AURORA["text_secondary"],
    "color-foreground-muted": _AURORA["text_muted"],
    "color-foreground-border": _AURORA["border"],
    "color-link": _AURORA["blue_primary"],
    "color-link--hover": _AURORA["blue_secondary"],
    "color-link-underline": "transparent",
    "color-link-underline--hover": _AURORA["blue_secondary"],
    "color-highlight-on-target": "rgba(30, 200, 255, 0.18)",
    "color-api-background": _AURORA["bg_deep"],
    "color-api-background-hover": _AURORA["glass"],
    "color-admonition-background": _AURORA["bg_deep"],
    "color-sidebar-background": _AURORA["bg_deep"],
    "color-sidebar-background-border": _AURORA["border"],
    "color-sidebar-link-text": _AURORA["text_secondary"],
    "color-sidebar-link-text--top-level": _AURORA["text_primary"],
    "color-sidebar-item-background--current": _AURORA["glass"],
    "color-sidebar-item-background--hover": _AURORA["glass"],
    "font-stack": _FONT_STACK,
    "font-stack--monospace": _MONO_STACK,
}

html_theme_options = {
    # Force the dark aurora look in both modes — the upstream site is
    # dark-only and the cyan accents are tuned for a black background.
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
