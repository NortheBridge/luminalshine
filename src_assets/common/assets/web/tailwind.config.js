const plugin = require('tailwindcss/plugin');

/** @type {import('tailwindcss').Config} */
module.exports = {
  darkMode: 'class',
  // Limit scanning to source files; avoid node_modules for performance
  content: [
    './index.html',
    './*.{vue,js,ts,html}',
    './components/**/*.{vue,js,ts}',
    './views/**/*.{vue,js,ts}',
    './configs/**/*.{vue,js,ts}',
    './stores/**/*.{js,ts}',
  ],
  theme: {
    extend: {
      // Single source of truth for semantic colors.
      // The app is dark-only; both `light` and `dark` resolve to the same
      // sunshine palette so anything that lands before `.dark` is applied
      // still gets a coherent surface. Values mirror the LuminalShine docs
      // theme (appdocs.northebridge.com — docs/_static/luminalshine.css).
      semanticColors: {
        light: {
          // Mirror of dark — see below. Kept identical so `:root` defaults
          // stay coherent if `.dark` is ever absent.
          dark: '17 17 17',
          surface: '26 26 26',
          light: '230 230 230',
          primary: '255 176 32',
          secondary: '255 122 26',
          accent: '255 61 0',
          info: '255 200 87',
          success: '76 175 80',
          warning: '255 167 38',
          danger: '244 67 54',
          onPrimary: '26 18 8',
          onSecondary: '26 18 8',
          onAccent: '255 248 240',
          onLight: '17 17 17',
          onDark: '240 235 228',
          brand: '255 200 87',
        },
        dark: {
          // LuminalShine sunshine palette on Learn-Dark surfaces.
          // Backgrounds: warm near-black.
          dark: '17 17 17', // #111111 base
          surface: '26 26 26', // #1A1A1A panels
          light: '230 230 230', // #E6E6E6 (text helpers)

          // Sunshine accents — sun-gold leads, with mid-orange / red-orange
          // as supporting hues for halos, gradients, and emphasis.
          primary: '255 176 32', // #FFB020  sun gold (PRIMARY)
          secondary: '255 122 26', // #FF7A1A  mid orange
          accent: '255 61 0', // #FF3D00  deep red-orange (outer flare)
          info: '255 200 87', // #FFC857  soft gold

          // Status colors — dark-safe semantics from the docs theme.
          success: '76 175 80', // #4CAF50  green
          warning: '255 167 38', // #FFA726  amber
          danger: '244 67 54', // #F44336  red

          // Text-on-color (AA+).
          onDark: '240 235 228',
          onLight: '17 17 17',
          onPrimary: '26 18 8',
          onSecondary: '26 18 8',
          onAccent: '255 248 240',

          // Soft-gold brand tint for logos and illustrations.
          brand: '255 200 87', // #FFC857
        },
      },
      colors: {
        // Semantic tokens resolved via CSS variables (light defaults, dark overrides via .dark)
        primary: 'rgb(var(--color-primary) / <alpha-value>)',
        secondary: 'rgb(var(--color-secondary) / <alpha-value>)',
        success: 'rgb(var(--color-success) / <alpha-value>)',
        warning: 'rgb(var(--color-warning) / <alpha-value>)',
        danger: 'rgb(var(--color-danger) / <alpha-value>)',
        info: 'rgb(var(--color-info) / <alpha-value>)',
        light: 'rgb(var(--color-light) / <alpha-value>)',
        dark: 'rgb(var(--color-dark) / <alpha-value>)',
        surface: 'rgb(var(--color-surface) / <alpha-value>)',
        accent: 'rgb(var(--color-accent) / <alpha-value>)',
        onPrimary: 'rgb(var(--color-on-primary) / <alpha-value>)',
        onSecondary: 'rgb(var(--color-on-secondary) / <alpha-value>)',
        onAccent: 'rgb(var(--color-on-accent) / <alpha-value>)',
        onLight: 'rgb(var(--color-on-light) / <alpha-value>)',
        onDark: 'rgb(var(--color-on-dark) / <alpha-value>)',
        // Optional brand token for places that previously mixed solar-secondary + lunar-onSecondary
        brand: 'rgb(var(--color-brand) / <alpha-value>)',
      },
    },
  },
  // Enable Tailwind preflight now that Bootstrap is removed. Keep visibility disabled if not needed.
  corePlugins: {
    preflight: true,
    visibility: false,
  },
  plugins: [
    // Emit CSS variables for semantic tokens from theme.semanticColors
    plugin(function ({ addBase, theme }) {
      const light = theme('semanticColors.light') || {};
      const dark = theme('semanticColors.dark') || {};
      const toVars = (src) => ({
        '--color-primary': src.primary,
        '--color-secondary': src.secondary,
        '--color-success': src.success,
        '--color-warning': src.warning,
        '--color-danger': src.danger,
        '--color-info': src.info,
        '--color-light': src.light,
        '--color-dark': src.dark,
        '--color-surface': src.surface,
        '--color-accent': src.accent,
        '--color-on-primary': src.onPrimary,
        '--color-on-secondary': src.onSecondary,
        '--color-on-accent': src.onAccent,
        '--color-on-light': src.onLight,
        '--color-on-dark': src.onDark,
        '--color-brand': src.brand,
      });
      addBase({
        ':root': toVars(light),
        '.dark': toVars(dark),
      });
    }),
  ],
};
