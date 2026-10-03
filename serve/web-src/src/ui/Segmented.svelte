<script>
  // One choice out of a few, as boxed buttons side by side (Bits UI toggle group; arrow keys move).
  import {ToggleGroup} from "bits-ui";
  let {value = $bindable(), options, label, id = undefined} = $props();   // options: [[value, text], ...]
</script>

<ToggleGroup.Root type="single" {value} onValueChange={(v) => { if (v) value = v; }} aria-label={label} {id} class="seg">
  {#each options as [v, text] (v)}
    <ToggleGroup.Item value={v} class="seg__item">{text}</ToggleGroup.Item>
  {/each}
</ToggleGroup.Root>

<style>
  :global(.seg) { display: flex; gap: 2px; }
  :global(.seg__item) { flex: 1; height: var(--ctl-h); padding: 0 8px; border: 0; border-radius: var(--r); background: var(--cell);
                        color: var(--dim); font-size: var(--fs-s); cursor: pointer; white-space: nowrap; }
  :global(.seg__item:hover) { color: var(--text); }
  :global(.seg__item[data-state="on"]) { background: var(--value); color: var(--value-ink); font-weight: var(--fw-b); }
</style>
