<script>
  // An on/off setting: a small square box (Bits UI checkbox) with its label and an optional hint.
  import {Checkbox, Label} from "bits-ui";
  let {checked = $bindable(false), label, hint = "", id, disabled = false, onchange = undefined} = $props();
</script>

<div class="check">
  <Checkbox.Root {id} bind:checked {disabled} onCheckedChange={onchange} class="check__box">
    {#snippet children({checked})}{#if checked}<svg viewBox="0 0 12 12" aria-hidden="true"><path d="M2.5 6.2l2.3 2.3 4.7-5" /></svg>{/if}{/snippet}
  </Checkbox.Root>
  <Label.Root for={id} class="check__label">{label}{#if hint}<span class="check__hint">{hint}</span>{/if}</Label.Root>
</div>

<style>
  .check { display: flex; align-items: flex-start; gap: 8px; }
  .check :global(.check__box) { flex: none; display: grid; place-items: center; width: 14px; height: 14px; margin-top: 1px; padding: 0;
                                 border: 0; border-radius: 2px; background: var(--well); box-shadow: inset 0 0 0 1px var(--line); cursor: pointer; }
  .check :global(.check__box[data-state="checked"]) { background: var(--value); box-shadow: none; }
  .check :global(.check__box svg) { width: 11px; height: 11px; fill: none; stroke: var(--value-ink); stroke-width: 1.8; stroke-linecap: round; stroke-linejoin: round; }
  .check :global(.check__label) { font-size: var(--fs-m); cursor: pointer; }
  .check__hint { display: block; font-size: var(--fs-s); color: var(--dim); }
</style>
