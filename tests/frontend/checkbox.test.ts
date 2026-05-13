import { mount, flushPromises } from '@vue/test-utils';
import Checkbox from '@web/Checkbox.vue';

describe('Checkbox.vue', () => {
  const mountWith = (model: any, props: any = {}) =>
    mount(Checkbox as any, {
      props: { id: 'flag', localePrefix: 'playnite', label: 'Label', modelValue: model, ...props },
      global: { mocks: { $t: (k: string) => k } },
    });

  // Checkbox.vue wraps naive-ui's NCheckbox, which renders a
  // <div role="checkbox" aria-checked="..."> in the DOM. To drive the
  // checkbox we click that DOM element — naive-ui's internal click
  // handler then calls its own emit('update:checked', ...), which Vue
  // routes through the v-model:checked binding on Checkbox.vue,
  // running the value-mapping setter and emitting update:modelValue
  // with the mapped result. Going through .trigger('click') exercises
  // the same path a real user takes; emitting on findComponent(NCheckbox).vm
  // bypasses naive-ui's own emit wrapper and the event doesn't propagate
  // to the parent listener via vue-test-utils 2.x in jsdom.

  test('maps boolean model to true/false values', async () => {
    const w = mountWith(true);
    const cb = w.find('[role="checkbox"]');
    expect(cb.exists()).toBe(true);
    expect(cb.attributes('aria-checked')).toBe('true');
    await cb.trigger('click');
    await flushPromises();
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe(false);
  });

  test('maps string "enabled/disabled" and respects inverseValues', async () => {
    const w = mountWith('enabled', { inverseValues: true });
    const cb = w.find('[role="checkbox"]');
    // inverseValues flips truthy/falsy mapping; 'enabled' becomes falsy.
    expect(cb.attributes('aria-checked')).toBe('false');
    await cb.trigger('click');
    await flushPromises();
    // When toggled on under inverseValues, the model updates to the
    // *other* member of the pair — 'disabled'.
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe('disabled');
  });

  test('numeric 1/0 mapping works', async () => {
    const w = mountWith(1);
    const cb = w.find('[role="checkbox"]');
    expect(cb.attributes('aria-checked')).toBe('true');
    await cb.trigger('click');
    await flushPromises();
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe(0);
  });

  test('shows default value hint based on `default` prop', () => {
    const w = mountWith(true, { default: 'enabled' });
    expect(w.text()).toContain('_common.enabled_def_cbox');
  });
});
