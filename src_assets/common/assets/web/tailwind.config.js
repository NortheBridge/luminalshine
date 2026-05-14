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
      // aurora palette so anything that lands before `.dark` is applied
      // still gets a coherent surface. Values mirror gitdocs.northebridge.com.
      semanticColors: {
        light: {
          // Mirror of dark — see below. Kept identical so `:root` defaults
          // stay coherent if `.dark` is ever absent.
          dark: '6 10 24',
          surface: '14 20 36',
          light: '224 236 255',
          primary: '30 200 255',
          secondary: '74 125 255',
          accent: '138 92 255',
          info: '0 224 198',
          success: '16 185 129',
          warning: '245 158 11',
          danger: '225 29 72',
          onPrimary: '6 10 24',
          onSecondary: '245 249 255',
          onAccent: '6 10 24',
          onLight: '6 10 24',
          onDark: '245 249 255',
          brand: '144 224 239',
        },
        dark: {
          // Aurora-glass palette (gitdocs.northebridge.com).
          // Backgrounds: deep navy near-black starfield.
          dark: '6 10 24', // #06 0A 18 base
          surface: '14 20 36', // #0E 14 24 panels
          light: '224 236 255', // #E0 EC FF pale moon (text helpers)

          // Aurora accents — Alexa+ cyan leads, with periwinkle/violet/teal
          // as supporting hues used for halos, gradients, and info states.
          primary: '30 200 255', // #1EC8FF  Alexa+ cyan
          secondary: '74 125 255', // #4A7DFF  periwinkle
          accent: '138 92 255', // #8A5CFF  violet
          info: '0 224 198', // #00E0C6  teal

          // Status colors retain salience by sitting outside the cyan/violet axis.
          success: '16 185 129', // #10B981  emerald-teal
          warning: '245 158 11', // #F59E0B  amber
          danger: '225 29 72', // #E11D48  rose

          // Text-on-color (AA+).
          onDark: '245 249 255',
          onLight: '6 10 24',
          onPrimary: '6 10 24',
          onSecondary: '245 249 255',
          onAccent: '6 10 24',

          // Pale-cyan brand tint for logos and illustrations.
          brand: '144 224 239', // #90E0EF
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
