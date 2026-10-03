<script>
  // A reading: the number and its unit at the same size; cyan while it is live ("active").
  let {value, unit = "", active = false, large = false} = $props();
</script>

<span class="value" class:active class:large>{value ?? "–"}{#if value != null && unit}<span class="unit">&#8239;{unit}</span>{/if}</span>

<style>
  .value { font-size: var(--fs-m); font-weight: var(--fw-b); white-space: nowrap; }
  .large { font-size: var(--fs-l); }
  .active { color: var(--value); }
  .unit { font-weight: var(--fw); color: var(--dim); }
  .active .unit { color: inherit; opacity: .8; }
</style>
