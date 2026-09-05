import type { GlobalThemeOverrides } from 'naive-ui';
import { ref, onMounted, onBeforeUnmount, watch } from 'vue';

// Use your existing CSS variables to keep the current color scheme.
// Naive UI accepts any valid CSS color string, so we reference the
// same tokens to maintain visual consistency across light/dark.

// Resolve `--color-xxx` (space-separated RGB like "77 163 255") to 'rgb(r, g, b)'
function cssVarRgb(name: string, fallback: string): string {
  if (typeof window === 'undefined') return fallback;
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  if (!raw) return fallback;
  // Accept formats like "77 163 255" or "77, 163, 255"
  const parts = raw.replace(/\s+/g, ' ').replace(/,/g, ' ').trim().split(' ');
  if (parts.length < 3) return fallback;
  const [r, g, b] = parts;
  const nr = Number(r),
    ng = Number(g),
    nb = Number(b);
  if ([nr, ng, nb].some((n) => !isFinite(n))) return fallback;
  return `rgb(${nr}, ${ng}, ${nb})`;
}

export function useNaiveThemeOverrides() {
  const overrides = ref<GlobalThemeOverrides>({});
  const clamp = (n: number) => Math.max(0, Math.min(255, Math.round(n)));
  const parse = (rgb: string): [number, number, number] => {
    const m = rgb.match(/(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/);
    if (m) return [Number(m[1]), Number(m[2]), Number(m[3])];
    const mm = rgb.match(/(\d+)\s+(\d+)\s+(\d+)/);
    if (mm) return [Number(mm[1]), Number(mm[2]), Number(mm[3])];
    return [0, 0, 0];
  };
  const toCss = (r: number, g: number, b: number) => `rgb(${clamp(r)}, ${clamp(g)}, ${clamp(b)})`;
  const lighten = (rgb: string, amt: number) => {
    const [r, g, b] = parse(rgb);
    return toCss(r + (255 - r) * amt, g + (255 - g) * amt, b + (255 - b) * amt);
  };
  const darken = (rgb: string, amt: number) => {
    const [r, g, b] = parse(rgb);
    return toCss(r * (1 - amt), g * (1 - amt), b * (1 - amt));
  };
  const compute = () => {
    const primary = cssVarRgb('--color-primary', 'rgb(255, 176, 32)');
    const info = cssVarRgb('--color-info', 'rgb(255, 200, 87)');
    const success = cssVarRgb('--color-success', 'rgb(76, 175, 80)');
    const warning = cssVarRgb('--color-warning', 'rgb(255, 167, 38)');
    const danger = cssVarRgb('--color-danger', 'rgb(244, 67, 54)');
    // Mission Control surfaces — flat, opaque panels on a cool near-black
    // page. (Variable names kept so the overrides below read unchanged.)
    const glassSurface = '#14171B';
    const glassPopover = '#1A1E24';
    const glassTable = '#111418';
    const glassBorder = 'rgba(255, 255, 255, 0.08)';
    overrides.value = {
      common: {
        primaryColor: primary,
        primaryColorHover: lighten(primary, 0.1),
        primaryColorPressed: darken(primary, 0.12),
        primaryColorSuppl: lighten(primary, 0.18),
        infoColor: info,
        infoColorHover: lighten(info, 0.1),
        infoColorPressed: darken(info, 0.12),
        infoColorSuppl: lighten(info, 0.18),
        successColor: success,
        successColorHover: lighten(success, 0.08),
        successColorPressed: darken(success, 0.12),
        successColorSuppl: lighten(success, 0.16),
        warningColor: warning,
        warningColorHover: lighten(warning, 0.08),
        warningColorPressed: darken(warning, 0.12),
        warningColorSuppl: lighten(warning, 0.16),
        errorColor: danger,
        errorColorHover: lighten(danger, 0.08),
        errorColorPressed: darken(danger, 0.14),
        errorColorSuppl: lighten(danger, 0.16),

        baseColor: cssVarRgb('--color-dark', 'rgb(14, 16, 19)'),
        bodyColor: 'rgba(0, 0, 0, 0)', // the page background is painted by <html>
        textColorBase: cssVarRgb('--color-onDark', 'rgb(232, 234, 237)'),
        cardColor: glassSurface,
        modalColor: glassSurface,
        popoverColor: glassPopover,
        tableColor: glassTable,

        borderColor: glassBorder,
        dividerColor: glassBorder,
        hoverColor: 'rgba(255, 176, 32, 0.08)',
      },
      Card: {
        borderRadius: '12px',
        paddingMedium: '16px 18px',
        paddingLarge: '20px 22px',
      },
      Button: {
        borderRadiusMedium: '8px',
        borderRadiusLarge: '10px',
        borderRadiusSmall: '7px',
      },
      Input: {
        borderRadius: '8px',
        color: '#0E1013',
        colorFocus: '#0E1013',
      },
      Tag: {
        borderRadius: '999px',
      },
      Alert: {
        borderRadius: '10px',
      },
      Modal: {
        peers: {
          Card: {
            borderRadius: '12px',
          },
        },
      },
      Dropdown: {
        borderRadius: '10px',
      },
    } as GlobalThemeOverrides;
  };

  onMounted(compute);
  // Also export a small hook below that flags dark changes; recompute on changes
  const isDark = useDarkModeClassRef();
  watch(isDark, () => compute());

  return overrides;
}

// Small helper to sync Naive's theme with your existing dark-mode class.
// Usage: const isDark = useDarkModeClass();
export function useDarkModeClassRef() {
  const isDark = ref<boolean>(false);
  let observer: MutationObserver | null = null;

  const update = () => {
    if (typeof document !== 'undefined') {
      isDark.value = document.documentElement.classList.contains('dark');
    }
  };

  if (typeof window !== 'undefined') {
    update();
    onMounted(() => {
      update();
      observer = new MutationObserver(update);
      observer.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] });
    });
    onBeforeUnmount(() => {
      observer?.disconnect();
      observer = null;
    });
  }

  return isDark;
}
