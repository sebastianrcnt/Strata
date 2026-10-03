<script>
  // A small state label. Use it for the exceptions only (stopped, error, queued...), not for the normal case.
  let {tone = "neutral", title = undefined, children} = $props();
</script>

<span class="badge badge--{tone}" {title}>{@render children()}</span>

<style>
  .badge { display: inline-flex; align-items: center; height: 17px; padding: 0 5px; border-radius: var(--r); font-size: var(--fs-s);
           white-space: nowrap; vertical-align: middle; }
  .badge--neutral { background: var(--cell); color: var(--dim); }
  .badge--on { background: var(--accent-tint); color: var(--accent); }
  .badge--value { background: var(--value-tint); color: var(--value); }
  .badge--warn { box-shadow: inset 0 0 0 1px var(--accent); color: var(--accent); }
  .badge--error { background: var(--danger-tint); color: var(--danger); }
</style>
