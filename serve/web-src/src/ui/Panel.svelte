<script>
  // A panel: a title bar (status lamp, title, tools on the right) over a body. Panels sit on the page background
  // with a small gap, so no outlines are needed.
  let {title, lamp = "", tools = null, children, flush = false, class: cls = "", ...rest} = $props();
</script>

<section class="panel {cls}" {...rest}>
  <header class="panel__head">
    {#if lamp !== null}<span class="lamp" data-lamp={lamp} aria-hidden="true"></span>{/if}
    <h2>{title}</h2>
    {#if tools}<div class="panel__tools">{@render tools()}</div>{/if}
  </header>
  <div class="panel__body" class:flush>{@render children()}</div>
</section>

<style>
  .panel { display: flex; flex-direction: column; min-width: 0; background: var(--panel); border-radius: var(--r); overflow: hidden; }
  .panel__head { display: flex; align-items: center; gap: 7px; flex: none; height: var(--head-h); padding: 0 4px 0 7px; background: var(--head); }
  h2 { margin: 0; font-size: var(--fs-m); font-weight: var(--fw-b); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .panel__tools { margin-left: auto; display: flex; align-items: center; gap: 4px; min-width: 0; font-size: var(--fs-s); color: var(--dim); }
  .panel__body { flex: 1; min-height: 0; padding: 8px 10px; }
  .panel__body.flush { padding: 0; }
  /* the lamp: off (grey), on (orange), value (cyan), error (red) */
  .lamp { flex: none; width: 10px; height: 10px; border-radius: 50%; background: var(--off); }
  .lamp[data-lamp="on"] { background: var(--accent); box-shadow: 0 0 6px var(--accent-tint); }
  .lamp[data-lamp="value"] { background: var(--value); }
  .lamp[data-lamp="error"] { background: var(--danger); }
</style>
