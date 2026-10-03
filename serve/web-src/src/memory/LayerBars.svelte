<script>
  // One bar per layer: `values` 0..1 (null: nothing to show), on 0..max; hover names the layer.
  let {values, max = 1, summary, describe} = $props();
  let hover = $state(null);
</script>

<div class="layers">
  <div class="head">
    {#if hover != null}<span>Layer {hover}</span><strong>{describe(hover)}</strong>{:else}<span>{summary}</span>{/if}
  </div>
  <div class="bars" onmouseleave={() => (hover = null)} role="img" aria-label={summary}>
    {#each values as v, l}
      <span class="bar" class:on={hover === l} style:height="{v == null ? 0 : (100 * v) / max}%" onmouseenter={() => (hover = l)} role="presentation"></span>
    {/each}
  </div>
  <div class="axis" aria-hidden="true"><span>layer 0</span><span>{values.length - 1}</span></div>
</div>

<style>
  .layers { display: flex; flex-direction: column; gap: 6px; min-width: 0; }
  .head { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 10px; font-size: var(--fs-s); color: var(--dim); min-height: 16px; }
  .head strong { color: var(--value); font-weight: var(--fw-b); font-size: var(--fs-m); }
  .bars { display: flex; align-items: flex-end; gap: 2px; height: 90px; padding: 4px; background: var(--chart); border-radius: 2px; }
  .bar { flex: 1; min-width: 1px; background: var(--value); opacity: .75; border-radius: 1px 1px 0 0; }
  .bar.on { opacity: 1; }
  .axis { display: flex; justify-content: space-between; font: 10px/1 var(--mono); color: var(--off); padding: 0 4px; }
</style>
