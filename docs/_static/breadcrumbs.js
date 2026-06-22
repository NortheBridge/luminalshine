/*
 * LuminalShine docs — Microsoft Learn-style breadcrumb trail.
 *
 * Furo has no breadcrumb, so we synthesize one client-side from the
 * rendered DOM: Home / <section caption> / <page title>. This is
 * deliberately defensive — every lookup is guarded and any failure
 * leaves the page untouched rather than throwing, so a Furo markup
 * change can never break the docs render.
 */
(function () {
  "use strict";

  function ready(fn) {
    if (document.readyState !== "loading") {
      fn();
    } else {
      document.addEventListener("DOMContentLoaded", fn);
    }
  }

  // The top-level <ul> that holds the current page lives directly under
  // .sidebar-tree, immediately preceded by its <p class="caption">.
  function sectionCaptionFor(currentLink) {
    var el = currentLink;
    while (
      el &&
      !(
        el.tagName === "UL" &&
        el.parentElement &&
        el.parentElement.classList.contains("sidebar-tree")
      )
    ) {
      el = el.parentElement;
    }
    if (!el) {
      return null;
    }
    var prev = el.previousElementSibling;
    if (prev && prev.classList && prev.classList.contains("caption")) {
      var text = prev.querySelector(".caption-text") || prev;
      var value = (text.textContent || "").trim();
      return value || null;
    }
    return null;
  }

  function pageTitle(article) {
    var h1 = article.querySelector("h1");
    if (!h1) {
      return null;
    }
    // Clone so we can drop Furo's "¶" headerlink before reading text.
    var clone = h1.cloneNode(true);
    clone.querySelectorAll(".headerlink").forEach(function (n) {
      n.remove();
    });
    return (clone.textContent || "").trim() || null;
  }

  function makeSep() {
    var sep = document.createElement("span");
    sep.className = "ls-breadcrumb-sep";
    sep.setAttribute("aria-hidden", "true");
    sep.textContent = "/";
    return sep;
  }

  function build() {
    try {
      var article =
        document.querySelector("article[role='main']") ||
        document.querySelector(".content article") ||
        document.querySelector("article");
      if (!article || article.querySelector(".ls-breadcrumb")) {
        return;
      }

      var items = [];

      // Home — reuse the sidebar brand link so the relative href is
      // always correct regardless of page depth.
      var brand = document.querySelector("a.sidebar-brand");
      var homeText = "LuminalShine";
      if (brand) {
        var brandText = brand.querySelector(".sidebar-brand-text");
        if (brandText && brandText.textContent.trim()) {
          homeText = brandText.textContent.trim();
        }
        items.push({ text: homeText, href: brand.getAttribute("href") });
      } else {
        items.push({ text: homeText, href: null });
      }

      // Section caption (from the current item in the sidebar tree).
      var currentLink =
        document.querySelector(".sidebar-tree .current-page > a.reference") ||
        document.querySelector(".sidebar-tree a.reference.current") ||
        document.querySelector(".sidebar-tree .current > a.reference");
      if (currentLink) {
        var caption = sectionCaptionFor(currentLink);
        if (caption) {
          items.push({ text: caption, href: null });
        }
      }

      // Current page title (non-link).
      var title = pageTitle(article);
      if (title) {
        items.push({ text: title, href: null, current: true });
      }

      // Nothing meaningful beyond Home (e.g. the landing page) — skip.
      if (items.length <= 1) {
        return;
      }

      var nav = document.createElement("nav");
      nav.className = "ls-breadcrumb";
      nav.setAttribute("aria-label", "Breadcrumb");

      items.forEach(function (item, i) {
        if (i > 0) {
          nav.appendChild(makeSep());
        }
        var node;
        if (item.href && !item.current) {
          node = document.createElement("a");
          node.setAttribute("href", item.href);
        } else {
          node = document.createElement("span");
          if (item.current) {
            node.className = "ls-breadcrumb-current";
            node.setAttribute("aria-current", "page");
          }
        }
        node.textContent = item.text;
        nav.appendChild(node);
      });

      article.insertBefore(nav, article.firstChild);
    } catch (e) {
      /* never break the page over a breadcrumb */
    }
  }

  ready(build);
})();
