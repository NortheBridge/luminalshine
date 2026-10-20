/* Native modal supplies focus containment, Escape dismissal and inert background. */
document.addEventListener('DOMContentLoaded', () => {
  const drawer = document.querySelector('#ls-navigation');
  const opener = document.querySelector('.ls-menu');
  if (!drawer || !opener) return;
  opener.hidden = false;
  opener.addEventListener('click', () => {
    drawer.showModal();
    opener.setAttribute('aria-expanded', 'true');
    document.documentElement.classList.add('ls-drawer-open');
  });
  drawer.querySelector('.ls-close').addEventListener('click', () => drawer.close());
  drawer.addEventListener('click', event => {
    const bounds = drawer.getBoundingClientRect();
    if (event.target === drawer && (event.clientX < bounds.left || event.clientX > bounds.right || event.clientY < bounds.top || event.clientY > bounds.bottom)) drawer.close();
    if (event.target.closest('a[href]')) drawer.close();
  });
  drawer.addEventListener('close', () => {
    opener.setAttribute('aria-expanded', 'false');
    document.documentElement.classList.remove('ls-drawer-open');
    opener.focus();
  });
});
