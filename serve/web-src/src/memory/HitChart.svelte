<script>
  // The hit rate of each recent request (oldest left), on 0-100%, over a dashed line at the cache's share of the
  // experts: what a cache picked at random would hit. The gap between the bars and the line is what the profile buys.
  import {fmt, clock} from "../lib/format.js";

  let {requests, share} = $props();   // requests newest first, as /metrics lists them

  const bars = $derived((requests || []).filter((r) => r.hit_rate != null).slice(0, 80).reverse());
  let hover = $state(null);
  const shown = $derived(hover ?? bars[bars.length - 1]);
</script>

<div class="chart">
  <div class="head">
    {#if shown}
      <span>{hover ? clock(shown.time) : "Last request"}</span>
      <strong>{fmt(100 * shown.hit_rate, 1)}% found in VRAM</strong>
      <span class="muted">{fmt(shown.output_tokens)} tokens · {fmt(shown.decode_tok_s, 1)} tok/s</span>
    {:else}
      <span class="muted">No request yet since the server started.</span>
    {/if}
  </div>
  <div class="plot" role="img" aria-label="Hit rate of the last {bars.length} requests">
    <div class="grid" aria-hidden="true">
      {#each [100, 75, 50, 25] as y}<span style:bottom="{y}%"><b>{y}%</b></span>{/each}
    </div>
    {#if share}
      <div class="base" style:bottom="{100 * share}%"><b>random: {fmt(100 * share, 1)}%</b></div>
    {/if}
    <div class="bars" onmouseleave={() => (hover = null)} role="presentation">
      {#each bars as r (r.time)}
        <span class="bar" class:on={hover === r} style:height="{100 * r.hit_rate}%" onmouseenter={() => (hover = r)} role="presentation"></span>
      {/each}
    </div>
  </div>
</div>

<style>
  .chart { display: flex; flex-direction: column; gap: 6px; min-width: 0; }
  .head { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 10px; font-size: var(--fs-s); color: var(--dim); min-height: 16px; }
  .head strong { color: var(--value); font-weight: var(--fw-b); font-size: var(--fs-m); }
  .plot { position: relative; height: 150px; margin-left: 34px; background: var(--chart); border-radius: 2px; }
  .grid span { position: absolute; left: 0; right: 0; border-top: 1px solid var(--chart-grid); }
  .grid b, .base b { position: absolute; font: var(--fw) 10px/1 var(--mono); color: var(--off); transform: translateY(-50%); }
  .grid b { right: calc(100% + 6px); }
  .base { position: absolute; left: 0; right: 0; border-top: 1px dashed var(--dim); z-index: 1; pointer-events: none; }
  .base b { left: 6px; top: -8px; transform: none; color: var(--dim); background: var(--chart); padding: 0 3px; }
  .bars { position: absolute; inset: 0 4px; display: flex; align-items: flex-end; justify-content: flex-end; gap: 2px; }
  .bar { flex: 1; max-width: 24px; min-width: 2px; background: var(--value); opacity: .75; border-radius: 1px 1px 0 0; }
  .bar.on, .bar:last-child { opacity: 1; }
</style>
