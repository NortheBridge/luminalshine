import { useI18n } from 'vue-i18n';

/**
 * `t` with a fallback: returns the English fallback when the key is not in
 * the active locale (vue-i18n returns the key itself in that case).
 */
export function useT2() {
  const { t } = useI18n();
  return (key: string, fallback: string): string => {
    const v = t(key);
    return v === key ? fallback : v;
  };
}
