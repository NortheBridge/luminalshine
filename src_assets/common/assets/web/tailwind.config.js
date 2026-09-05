const plugin = require('tailwindcss/plugin');

/** @type {import('tailwindcss').Config} */
module.exports = {
  darkMode: 'class',
  // Limit scanning to source files; avoid node_modules for performance
  content: [
    './index.html',
    './*.{vue,js,ts,html}',
    './components/**/*.{vue,js,ts}',
    './composables/**/*.{js,ts}',
    './views/**/*.{vue,js,ts}',
    './configs/**/*.{vue,js,ts}',
    './stores/**/*.{js,ts}',
  ],
  theme: {
    extend: {
      fontFamily: {
        sans: ['IBM Plex Sans', 'Segoe UI Variable', 'Segoe UI', 'system-ui', 'sans-serif'],
        mono: ['IBM Plex Mono', 'Consolas', 'Courier New', 'monospace'],
      },
      // Single source of truth for semantic colors.
      // The app is dark-only; both `light` and `dark` resolve to the same
      // "Mission Control" palette so anything that lands before `.dark` is
      // applied still gets a coherent surface. Cool near-black surfaces,
      // sun gold as the ONLY accent, green/amber/red reserved for state.
      semanticColors: {
        light: {
          dark: '14 16 19',
          chrome: '11 13 16',
          surface: '20 23 27',
          light: '200 204 208',
          primary: '255 176 32',
          secondary: '255 200 87',
          accent: '255 176 32',
          info: '255 200 87',
          success: '76 175 80',
          warning: '255 167 38',
          danger: '244 67 54',
          onPrimary: '26 18 8',
          onSecondary: '26 18 8',
          onAccent: '26 18 8',
          onLight: '14 16 19',
          onDark: '232 234 237',
          brand: '255 200 87',
        },
        dark: {
          // Backgrounds: cool near-black.
          dark: '14 16 19', // #0E1013 page
          chrome: '11 13 16', // #0B0D10 rail, strip, inspector
          surface: '20 23 27', // #14171B panels
          light: '200 204 208', // text helpers

          // Sun gold is the single accent. `secondary`/`accent`/`info` are
          // soft-gold variants kept for existing callers; no orange remains.
          primary: '255 176 32', // #FFB020
          secondary: '255 200 87', // #FFC857
          accent: '255 176 32',
          info: '255 200 87',

          // Status colors — reserved for state, never decoration.
          success: '76 175 80', // #4CAF50
          warning: '255 167 38', // #FFA726
          danger: '244 67 54', // #F44336

          // Text-on-color (AA+).
          onDark: '232 234 237', // #E8EAED
          onLight: '14 16 19',
          onPrimary: '26 18 8',
          onSecondary: '26 18 8',
          onAccent: '26 18 8',

          brand: '255 200 87',
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
        chrome: 'rgb(var(--color-chrome) / <alpha-value>)',
        surface: 'rgb(var(--color-surface) / <alpha-value>)',
        accent: 'rgb(var(--color-accent) / <alpha-value>)',
        onPrimary: 'rgb(var(--color-on-primary) / <alpha-value>)',
        onSecondary: 'rgb(var(--color-on-secondary) / <alpha-value>)',
        onAccent: 'rgb(var(--color-on-accent) / <alpha-value>)',
        onLight: 'rgb(var(--color-on-light) / <alpha-value>)',
        onDark: 'rgb(var(--color-on-dark) / <alpha-value>)',
        // Optional brand token for places that previously mixed solar-secondary + lunar-onSecondary
        brand: 'rgb(var(--color-brand) / <alpha-value>)',
        // Mission Control text ramp (literal, not themed — the app is dark-only).
        ink: {
          DEFAULT: '#E8EAED',
          2: '#C3C7CC',
          3: '#8B9096',
          4: '#6F757C',
        },
      },
      borderColor: {
        line: 'rgba(255, 255, 255, 0.07)',
        'line-strong': 'rgba(255, 255, 255, 0.12)',
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
        '--color-chrome': src.chrome,
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
