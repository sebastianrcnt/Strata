<script>
  // A side sheet (Bits UI dialog): focus stays inside while open and returns to the opener when it closes.
  import {Dialog} from "bits-ui";
  import Icon from "./Icon.svelte";
  let {open = $bindable(false), title, children} = $props();
</script>

<Dialog.Root bind:open>
  <Dialog.Portal>
    <Dialog.Overlay class="sheet__scrim" />
    <Dialog.Content class="sheet">
      <header class="sheet__head">
        <Dialog.Title class="sheet__title">{title}</Dialog.Title>
        <Dialog.Close class="iconbtn" aria-label="Close"><Icon name="close" /></Dialog.Close>
      </header>
      <div class="sheet__body">{@render children()}</div>
    </Dialog.Content>
  </Dialog.Portal>
</Dialog.Root>

<style>
  :global(.sheet__scrim) { position: fixed; inset: 0; z-index: 40; background: var(--scrim); }
  :global(.sheet) { position: fixed; top: 0; right: 0; bottom: 0; z-index: 41; width: min(380px, 100vw); display: flex; flex-direction: column;
                    background: var(--panel); box-shadow: var(--shadow); outline: none; }
  .sheet__head { display: flex; align-items: center; height: var(--head-h); padding: 0 4px 0 10px; background: var(--head); }
  :global(.sheet__title) { flex: 1; margin: 0; font-size: var(--fs-m); font-weight: var(--fw-b); }
  .sheet__body { flex: 1; overflow: auto; padding: 12px; display: flex; flex-direction: column; gap: 14px; }
</style>
