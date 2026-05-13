import { mount } from '@vue/test-utils';
import { NCheckbox } from 'naive-ui';
import Checkbox from '@web/Checkbox.vue';

describe('Checkbox.vue', () => {
  const mountWith = (model: any, props: any = {}) =>
    mount(Checkbox as any, {
      props: { id: 'flag', localePrefix: 'playnite', label: 'Label', modelValue: model, ...props },
      global: { mocks: { $t: (k: string) => k } },
    });

  // Checkbox.vue wraps naive-ui's NCheckbox, which renders a
  // <div role="checkbox"> rather than a native <input>. Tests drive the
  // checked state through NCheckbox's v-model:checked event and assert on
  // the parent wrapper's emitted update:modelValue payload (which carries
  // the value-mapped result — boolean, "enabled/disabled", numeric, etc).

  test('maps boolean model to true/false values', async () => {
    const w = mountWith(true);
    const cb = w.findComponent(NCheckbox);
    expect(cb.exists()).toBe(true);
    expect(cb.props('checked')).toBe(true);
    await cb.vm.$emit('update:checked', false);
    await w.vm.$nextTick();
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe(false);
  });

  test('maps string "enabled/disabled" and respects inverseValues', async () => {
    const w = mountWith('enabled', { inverseValues: true });
    const cb = w.findComponent(NCheckbox);
    // inverseValues flips truthy/falsy mapping; 'enabled' is the falsy side.
    expect(cb.props('checked')).toBe(false);
    await cb.vm.$emit('update:checked', true);
    await w.vm.$nextTick();
    // When checked under inverseValues, the model updates to the *other*
    // member of the pair — 'disabled'.
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe('disabled');
  });

  test('numeric 1/0 mapping works', async () => {
    const w = mountWith(1);
    const cb = w.findComponent(NCheckbox);
    expect(cb.props('checked')).toBe(true);
    await cb.vm.$emit('update:checked', false);
    await w.vm.$nextTick();
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe(0);
  });

  test('shows default value hint based on `default` prop', () => {
    const w = mountWith(true, { default: 'enabled' });
    expect(w.text()).toContain('_common.enabled_def_cbox');
  });
});
