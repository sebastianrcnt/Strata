<script>
  // How the cache is spread over the layers: each bar is the share of one layer's experts that the profile puts in
  // VRAM at start.
  import {fmt} from "../lib/format.js";

  let {perLayer, experts} = $props();

  const max = $derived(Math.max(1, ...perLayer));
  const lo = $derived(Math.min(...perLayer)), hi = $derived(Math.max(...perLayer));
  let hover = $state(null);
</script>

<div class="layers">
  <div class="head">
    {#if hover != null}
      <span>Layer {hover}</span><strong>{fmt(perLayer[hover])} of {fmt(experts)} experts cached</strong>
      <span class="muted">{fmt((100 * perLayer[hover]) / experts, 1)}%</span>
    {:else}
      <span>From {fmt(lo)} to {fmt(hi)} of {fmt(experts)} experts a layer</span>
    {/if}
  </div>
  <div class="bars" onmouseleave={() => (hover = null)} role="img" aria-label="Cached experts per layer, from {lo} to {hi}">
    {#each perLayer as n, l}
      <span class="bar" class:on={hover === l} style:height="{(100 * n) / max}%" onmouseenter={() => (hover = l)} role="presentation"></span>
    {/each}
  </div>
  <div class="axis" aria-hidden="true"><span>layer 0</span><span>{perLayer.length - 1}</span></div>
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
