<script>
  // Thinking and sampling controls, shared by the chat's settings and the server-wide defaults in Setup.
  import Segmented from "../ui/Segmented.svelte";
  import Check from "../ui/Check.svelte";
  let {s = $bindable(), projection = false, id = "s"} = $props();
  const THINK = {none: "answers right away", low: "short", medium: "medium", high: "thorough (default)"};
  const greedy = $derived(+s.temperature === 0);
</script>

<div class="field">
  <div class="label"><span id="{id}-thinking">Thinking</span><span class="hint">{THINK[s.thinking] || ""}</span></div>
  <Segmented bind:value={s.thinking} label="Thinking" options={[["none", "Off"], ["low", "Low"], ["medium", "Medium"], ["high", "High"]]} />
</div>
<div class="field">
  <label class="label" for="{id}-temp"><span>Temperature</span><span class="hint">{greedy ? "0 · greedy" : (+s.temperature).toFixed(2)}</span></label>
  <input class="slider" type="range" id="{id}-temp" min="0" max="1.5" step="0.05" bind:value={s.temperature}>
  <span class="hint">0 = always the most likely word (exact, repeatable)</span>
</div>
<div class="pair">
  <div class="field">
    <label class="label" for="{id}-topp"><span>Top-p</span><span class="hint">{(+s.top_p).toFixed(2)}</span></label>
    <input class="slider" type="range" id="{id}-topp" min="0.05" max="1" step="0.05" bind:value={s.top_p} disabled={greedy}>
  </div>
  <div class="field">
    <label class="label" for="{id}-topk"><span>Top-k</span><span class="hint">{s.top_k}</span></label>
    <input class="slider" type="range" id="{id}-topk" min="1" max="64" step="1" bind:value={s.top_k} disabled={greedy}>
  </div>
  <div class="field">
    <label class="label" for="{id}-max">Max tokens</label>
    <input class="input" id="{id}-max" type="number" min="1" step="1" placeholder="until done" bind:value={s.max}>
  </div>
  <div class="field">
    <label class="label" for="{id}-seed">Seed</label>
    <input class="input" id="{id}-seed" type="number" min="1" step="1" placeholder="random" bind:value={s.seed}>
  </div>
</div>
{#if projection}
  <Check id="{id}-esp" checked={s.esp !== false} onchange={(v) => (s.esp = v)} label="Experimental speed projection"
         hint="the engine's control vector; off = the stock model. Switching reads the chat again once" />
{/if}

<style>
  .field { display: flex; flex-direction: column; gap: 5px; min-width: 0; }
  .label { display: flex; justify-content: space-between; gap: 8px; font-size: var(--fs-m); }
  .hint { font-size: var(--fs-s); color: var(--dim); }
  .pair { display: grid; grid-template-columns: 1fr 1fr; gap: 12px 14px; }
  .slider { width: 100%; margin: 0; accent-color: var(--value); }
  .slider:disabled { opacity: .4; }
  .input { height: var(--ctl-h); padding: 0 7px; border: 0; border-radius: var(--r); background: var(--well); box-shadow: inset 0 0 0 1px var(--line);
           font-size: var(--fs-m); width: 100%; }
  .input:focus { outline: none; box-shadow: inset 0 0 0 1.5px var(--value); }
</style>
