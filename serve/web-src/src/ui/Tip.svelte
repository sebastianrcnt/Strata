<script>
  // A short explanation on hover or focus (Bits UI tooltip). Needs <Tooltip.Provider> around the app.
  import {Tooltip} from "bits-ui";
  let {text, children} = $props();
</script>

{#if text}
  <Tooltip.Root>
    <Tooltip.Trigger>
      {#snippet child({props})}<button type="button" {...props} class="tip__trigger">{@render children()}</button>{/snippet}
    </Tooltip.Trigger>
    <Tooltip.Portal>
      <Tooltip.Content class="tip" sideOffset={4}>{text}</Tooltip.Content>
    </Tooltip.Portal>
  </Tooltip.Root>
{:else}
  {@render children()}
{/if}

<style>
  :global(.tip__trigger) { all: unset; cursor: help; text-decoration: underline dotted var(--off); text-underline-offset: 3px; }
  :global(.tip__trigger:focus-visible) { outline: 1.5px solid var(--value); }
  :global(.tip) { z-index: 70; max-width: 300px; padding: 6px 8px; border-radius: var(--r); background: var(--head); color: var(--text);
                  font-size: var(--fs-s); line-height: 1.4; box-shadow: var(--shadow); }
</style>
