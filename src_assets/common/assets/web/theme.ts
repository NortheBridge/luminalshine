// The Vue UI is dark-only and styled to match gitdocs.northebridge.com.
// These helpers are retained as no-ops so existing callers (ThemeToggle,
// app bootstrap) keep compiling without behavior changes.

type Theme = 'light' | 'dark' | 'auto';

export const getPreferredTheme = (): Theme => 'dark';

const applyDarkClass = (): void => {
  if (typeof document === 'undefined') return;
  document.documentElement.classList.add('dark');
  document.documentElement.setAttribute('data-bs-theme', 'dark');
  document.documentElement.setAttribute('data-theme', 'dark');
};

export const setTheme = (_theme: Theme): void => applyDarkClass();

export const setStoredTheme = (_theme: string): void => {
  try {
    localStorage.setItem('theme', 'dark');
  } catch {
    /* ignore */
  }
};

export const showActiveTheme = (_theme: Theme, _focus = false): void => {
  /* no-op: theme toggle removed */
};

export function setupThemeToggleListener(): void {
  /* no-op: theme toggle removed */
}

export function loadAutoTheme(): void {
  applyDarkClass();
}
