import { mount } from '@vue/test-utils';
import { NCheckbox } from 'naive-ui';
import Checkbox from '@web/Checkbox.vue';

describe('Checkbox.vue', () => {
  const mountWith = (model: any, props: any = {}) =>
    mount(Checkbox as any, {
      props: { id: 'flag', localePrefix: 'playnite', label: 'Label', modelValue: model, ...props },
      global: { mocks: { $t: (k: string) => k } },
    });

  // Checkbox.vue wraps naive-ui's NCheckbox, which RENDERS a
  // <div role="checkbox" aria-checked="..."> in the DOM, but the VueWrapper
  // returned by findComponent(NCheckbox) has its .element pointed at an
  // outer wrapper that doesn't carry that attribute — so attribute reads
  // via the component wrapper return undefined. The pattern below splits
  // concerns:
  //   - w.find('[role="checkbox"]') returns a DOMWrapper for state reads
  //   - w.findComponent(NCheckbox) returns a VueWrapper for $emit
  // Together they exercise both the rendered initial state and the
  // v-model:checked event path through the parent's value mapping.

  test('maps boolean model to true/false values', async () => {
    const w = mountWith(true);
    const cbEl = w.find('[role="checkbox"]');
    const cbComp = w.findComponent(NCheckbox);
    expect(cbEl.exists()).toBe(true);
    expect(cbEl.attributes('aria-checked')).toBe('true');
    await cbComp.vm.$emit('update:checked', false);
    await w.vm.$nextTick();
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe(false);
  });

  test('maps string "enabled/disabled" and respects inverseValues', async () => {
    const w = mountWith('enabled', { inverseValues: true });
    const cbEl = w.find('[role="checkbox"]');
    const cbComp = w.findComponent(NCheckbox);
    // inverseValues flips truthy/falsy mapping; 'enabled' becomes falsy.
    expect(cbEl.attributes('aria-checked')).toBe('false');
    await cbComp.vm.$emit('update:checked', true);
    await w.vm.$nextTick();
    // When checked under inverseValues, the model updates to the *other*
    // member of the pair — 'disabled'.
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe('disabled');
  });

  test('numeric 1/0 mapping works', async () => {
    const w = mountWith(1);
    const cbEl = w.find('[role="checkbox"]');
    const cbComp = w.findComponent(NCheckbox);
    expect(cbEl.attributes('aria-checked')).toBe('true');
    await cbComp.vm.$emit('update:checked', false);
    await w.vm.$nextTick();
    expect(w.emitted()['update:modelValue']?.[0]?.[0]).toBe(0);
  });

  test('shows default value hint based on `default` prop', () => {
    const w = mountWith(true, { default: 'enabled' });
    expect(w.text()).toContain('_common.enabled_def_cbox');
  });
});
