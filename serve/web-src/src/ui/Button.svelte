<script>
  // Buttons: "cell" (default, raised grey), "primary" (orange), "ghost" (no fill). `pressed` makes it a toggle
  // button that fills cyan while on. `icon` alone gives a square icon button.
  import Icon from "./Icon.svelte";
  let {variant = "cell", icon = "", pressed = undefined, children = null, type = "button", class: cls = "", ...rest} = $props();
</script>

<button {type} class="btn btn--{variant} {cls}" class:btn--icon={icon && !children} aria-pressed={pressed} {...rest}>
  {#if icon}<Icon name={icon} small={!!children} />{/if}{#if children}{@render children()}{/if}
</button>

<style>
  .btn { display: inline-flex; align-items: center; justify-content: center; gap: 5px; height: var(--ctl-h); padding: 0 8px;
         border: 0; border-radius: var(--r); font-size: var(--fs-s); white-space: nowrap; cursor: pointer; }
  .btn--cell { background: var(--cell); color: var(--text); }
  .btn--cell:hover { background: var(--cell-hover); }
  .btn--primary { background: var(--accent); color: var(--accent-ink); font-weight: var(--fw-b); }
  .btn--primary:hover { filter: brightness(1.08); }
  .btn--ghost { background: transparent; color: var(--dim); }
  .btn--ghost:hover { background: var(--cell-hover); color: var(--text); }
  .btn[aria-pressed="true"] { background: var(--value); color: var(--value-ink); }
  .btn--icon { width: var(--ctl-h); padding: 0; }
  .btn:disabled { opacity: .45; cursor: default; filter: none; }
</style>
