/*
 * Docs WIP modal.
 *
 * Any element with class `js-docs-modal` triggers the modal instead of
 * navigating. The element's href is preserved as a no-JS fallback (typically
 * pointing at GitHub Discussions, which is where the modal also redirects).
 */
(function () {
  "use strict";

  var DISCUSSIONS_URL = "https://github.com/NortheBridge/luminalshine/discussions";

  function buildModal() {
    var modal = document.createElement("div");
    modal.className = "modal";
    modal.id = "docs-modal";
    modal.setAttribute("role", "dialog");
    modal.setAttribute("aria-modal", "true");
    modal.setAttribute("aria-labelledby", "docs-modal-title");
    modal.hidden = true;
    modal.innerHTML = [
      '<div class="modal-backdrop" data-modal-close></div>',
      '<div class="modal-dialog" role="document">',
      '  <button class="modal-close" type="button" data-modal-close aria-label="Close">',
      '    <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18" /></svg>',
      '  </button>',
      '  <div class="modal-icon" aria-hidden="true">',
      '    <svg viewBox="0 0 24 24"><path d="M4 5a2 2 0 0 1 2-2h12a2 2 0 0 1 2 2v14H6a2 2 0 0 1-2-2V5z" /><path d="M4 17a2 2 0 0 1 2-2h14" /></svg>',
      '  </div>',
      '  <h2 class="modal-title" id="docs-modal-title">Documentation is a work in progress</h2>',
      '  <p class="modal-body">Full documentation on LuminalShine is currently a work in progress and will be released soon. In the meantime, use GitHub Discussions for any questions, comments, ideas, or concerns.</p>',
      '  <div class="modal-actions">',
      '    <a class="btn btn-primary" href="' + DISCUSSIONS_URL + '" target="_blank" rel="noopener">',
      '      <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M21 11.5a8.38 8.38 0 0 1-9 8.4 8.5 8.5 0 0 1-4-1L3 20l1.1-4.8a8.38 8.38 0 0 1-.6-3.2 8.5 8.5 0 0 1 8.5-8.5 8.38 8.38 0 0 1 8 8z" /></svg>',
      '      Go to GitHub Discussions',
      '    </a>',
      '    <button class="btn" type="button" data-modal-close>Close</button>',
      '  </div>',
      '</div>'
    ].join("\n");
    return modal;
  }

  function init() {
    if (document.getElementById("docs-modal")) return;

    var modal = buildModal();
    document.body.appendChild(modal);

    var lastFocus = null;

    function openModal(event) {
      if (event) event.preventDefault();
      lastFocus = document.activeElement;
      modal.classList.add("is-open");
      modal.hidden = false;
      document.body.classList.add("modal-open");
      var closeBtn = modal.querySelector(".modal-close");
      if (closeBtn) closeBtn.focus();
    }

    function closeModal() {
      modal.classList.remove("is-open");
      modal.hidden = true;
      document.body.classList.remove("modal-open");
      if (lastFocus && typeof lastFocus.focus === "function") {
        lastFocus.focus();
      }
    }

    var triggers = document.querySelectorAll(".js-docs-modal");
    for (var i = 0; i < triggers.length; i++) {
      triggers[i].addEventListener("click", openModal);
    }

    var closers = modal.querySelectorAll("[data-modal-close]");
    for (var j = 0; j < closers.length; j++) {
      closers[j].addEventListener("click", closeModal);
    }

    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape" && modal.classList.contains("is-open")) {
        closeModal();
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
