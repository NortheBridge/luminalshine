import { computed, ref } from 'vue';

/**
 * Inspector column state.
 *
 * The app shell renders one right-hand column (`#mc-inspector`). A page that
 * wants to fill it mounts an <InspectorPanel>, which teleports its content
 * into that column and bumps this counter so the shell knows to show it.
 * Pages without a panel get the full content width automatically.
 */
export const INSPECTOR_TARGET_ID = 'mc-inspector';

const mounted = ref(0);

export function useInspector() {
  const open = computed(() => mounted.value > 0);
  function attach(): void {
    mounted.value += 1;
  }
  function detach(): void {
    mounted.value = Math.max(0, mounted.value - 1);
  }
  return { open, attach, detach };
}
