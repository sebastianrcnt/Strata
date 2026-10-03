<script>
  // Thinking and sampling controls, shared by the chat's settings and the server-wide defaults in Setup.
  let {s = $bindable(), projection = false, id = "s"} = $props();
  const THINK = {none: "answers right away", low: "short", medium: "medium", high: "thorough (default)"};
</script>

<div class="st-field">
  <span class="st-label" id="{id}-thinking-label">Thinking <output>{THINK[s.thinking] || ""}</output></span>
  <div class="seg" role="radiogroup" aria-labelledby="{id}-thinking-label">
    {#each [["none", "Off"], ["low", "Low"], ["medium", "Medium"], ["high", "High"]] as [v, label]}
      <button type="button" role="radio" aria-checked={s.thinking === v} onclick={() => (s.thinking = v)}>{label}</button>
    {/each}
  </div>
</div>
<div class="st-field">
  <label class="st-label" for="{id}-temp">Temperature <output>{+s.temperature === 0 ? "0 · greedy" : (+s.temperature).toFixed(2)}</output></label>
  <input class="st-slider" type="range" id="{id}-temp" min="0" max="1.5" step="0.05" bind:value={s.temperature}>
  <span class="muted small">0 = always the most likely word (exact, repeatable)</span>
</div>
<div class="st-field">
  <label class="st-label" for="{id}-topp">Top-p <output>{(+s.top_p).toFixed(2)}</output></label>
  <input class="st-slider" type="range" id="{id}-topp" min="0.05" max="1" step="0.05" bind:value={s.top_p} disabled={+s.temperature === 0}>
</div>
<div class="st-field">
  <label class="st-label" for="{id}-topk">Top-k <output>{s.top_k}</output></label>
  <input class="st-slider" type="range" id="{id}-topk" min="1" max="64" step="1" bind:value={s.top_k} disabled={+s.temperature === 0}>
</div>
<div class="row2">
  <div class="st-field">
    <label class="st-label" for="{id}-max">Max tokens</label>
    <input class="st-input" id="{id}-max" type="number" min="1" step="1" placeholder="Until done" bind:value={s.max}>
  </div>
  <div class="st-field">
    <label class="st-label" for="{id}-seed">Seed</label>
    <input class="st-input" id="{id}-seed" type="number" min="1" step="1" placeholder="Random" bind:value={s.seed}>
  </div>
</div>
{#if projection}
  <div class="toggle-row"><span>Experimental speed projection<br><span class="muted small">the engine's control vector; off = the
    stock model. Switching reads the chat again once</span></span>
    <button class="st-toggle" role="switch" aria-checked={s.esp !== false} aria-label="Experimental speed projection"
            onclick={() => (s.esp = s.esp === false)}></button></div>
{/if}

<style>
  .seg { display: grid; grid-template-columns: repeat(4, 1fr); gap: 2px; padding: 2px; background: var(--st-surface-2);
         border: 1px solid var(--st-line); border-radius: var(--st-r-md); }
  .seg button { height: 28px; border: 0; border-radius: 2px; background: transparent; color: var(--st-ink-muted);
                font: 500 var(--st-fs-sm)/1 var(--st-font); cursor: pointer; }
  .seg button[aria-checked="true"] { background: var(--st-surface); color: var(--st-ink); font-weight: var(--st-fw-bold); }
  .row2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  .st-field { gap: 6px; }
</style>
