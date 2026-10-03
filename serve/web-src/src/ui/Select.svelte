<script>
  // A drop-down in the console's style (Bits UI select): a box with the value and a triangle.
  import {Select} from "bits-ui";
  let {value = $bindable(), options, label} = $props();   // options: [{value, label}], values as strings
  const current = $derived(options.find((o) => o.value === value)?.label ?? "");
</script>

<Select.Root type="single" {value} onValueChange={(v) => (value = v)} items={options}>
  <Select.Trigger class="select" aria-label={label}>{current}<span class="select__tri" aria-hidden="true"></span></Select.Trigger>
  <Select.Portal>
    <Select.Content class="select__menu" sideOffset={2}>
      <Select.Viewport>
        {#each options as o (o.value)}
          <Select.Item value={o.value} label={o.label} class="select__item">{o.label}</Select.Item>
        {/each}
      </Select.Viewport>
    </Select.Content>
  </Select.Portal>
</Select.Root>

<style>
  :global(.select) { display: inline-flex; align-items: center; justify-content: space-between; gap: 8px; min-width: 64px; height: var(--ctl-h);
                     padding: 0 6px 0 8px; border: 0; border-radius: var(--r); background: var(--well); color: var(--text);
                     font-size: var(--fs-s); cursor: pointer; }
  .select__tri { width: 0; height: 0; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 5px solid var(--dim); }
  :global(.select__menu) { z-index: 60; min-width: var(--bits-select-anchor-width); padding: 2px; border-radius: var(--r);
                           background: var(--head); box-shadow: var(--shadow); }
  :global(.select__item) { height: var(--ctl-h); padding: 0 8px; display: flex; align-items: center; border-radius: 2px;
                           font-size: var(--fs-s); cursor: pointer; outline: none; }
  :global(.select__item[data-highlighted]) { background: var(--cell-hover); }
  :global(.select__item[data-selected]) { color: var(--value); }
</style>
