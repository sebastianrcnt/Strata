<script>
  // Every expert as one cell, 48 rows (layers) x 512: cyan when the VRAM cache holds it, orange when it was used but
  // is not held (its lookups went to the CPU or over PCIe), brighter = more lookups. Dim cyan: held but unused.
  import {onMount} from "svelte";
  import {ui} from "../lib/ui.svelte.js";
  import {fmt} from "../lib/format.js";

  let {u, total, layers, experts, layout = "id"} = $props();

  let wrap, canvas, w = $state(0), hover = $state(null);
  const rowH = $derived(Math.max(4, Math.min(8, Math.round(w / 170))));
  const height = $derived(layers * rowH);

  // per layer, its experts busiest first ("By use")
  const order = $derived.by(() => {
    const o = new Uint16Array(layers * experts);
    for (let l = 0; l < layers; l++) {
      const ids = Array.from({length: experts}, (_, e) => e).sort((a, b) => total[l * experts + b] - total[l * experts + a]);
      o.set(ids, l * experts);
    }
    return o;
  });

  function color(name) {
    const g = document.createElement("canvas").getContext("2d");
    g.fillStyle = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return g.fillStyle;
  }

  function draw() {
    if (!canvas || !w) return;
    const dpr = devicePixelRatio || 1, cw = (w * dpr) / experts, rh = rowH * dpr;
    canvas.width = Math.round(w * dpr); canvas.height = Math.round(height * dpr);
    const g = canvas.getContext("2d");
    const c = {held: color("--value"), miss: color("--accent"), idle: color("--text"), bg: color("--well")};
    g.fillStyle = c.bg; g.fillRect(0, 0, canvas.width, canvas.height);
    let max = 1;
    for (let i = 0; i < total.length; i++) if (total[i] > max) max = total[i];
    const lmax = Math.log1p(max), gapY = rh >= 5 ? dpr : 0, gapX = cw >= 2.5 ? dpr * .5 : 0;
    for (let l = 0; l < layers; l++)
      for (let k = 0; k < experts; k++) {
        const e = layout === "use" ? order[l * experts + k] : k, i = l * experts + e, t = total[i];
        const s = t > 0 ? .16 + .84 * (Math.log1p(t) / lmax) ** 2 : 0;   // the busy stand out, the rare stay faint
        if (u.held[i]) { g.fillStyle = c.held; g.globalAlpha = t > 0 ? s : .14; }
        else if (t > 0) { g.fillStyle = c.miss; g.globalAlpha = s; }
        else { g.fillStyle = c.idle; g.globalAlpha = .06; }
        g.fillRect(k * cw, l * rh, Math.max(dpr * .6, cw - gapX), rh - gapY);
      }
    g.globalAlpha = 1;
  }
  $effect(() => { void [u, total, layout, w, ui.theme, order]; draw(); });

  onMount(() => {
    const ro = new ResizeObserver(([e]) => { w = Math.floor(e.contentRect.width); });
    ro.observe(wrap);
    return () => ro.disconnect();
  });

  function onMove(ev) {
    const r = canvas.getBoundingClientRect();
    const l = Math.floor(((ev.clientY - r.top) / r.height) * layers), k = Math.floor(((ev.clientX - r.left) / r.width) * experts);
    if (l < 0 || l >= layers || k < 0 || k >= experts) { hover = null; return; }
    const e = layout === "use" ? order[l * experts + k] : k, i = l * experts + e;
    hover = {l, e, t: total[i], vram: u.vram[i], gpu: u.gpu[i], cpu: u.cpu[i], held: !!u.held[i],
             x: ev.clientX - r.left, y: ev.clientY - r.top, right: ev.clientX - r.left > r.width - 240};
  }
</script>

<div class="map" bind:this={wrap}>
  <div class="axis" aria-hidden="true">
    {#each [0, 8, 16, 24, 32, 40, 47] as l}<span style:top="{(l + .5) * rowH}px">L{l}</span>{/each}
  </div>
  <div class="field" role="img" aria-label="Lookups per expert, {layers} layers by {experts} experts">
    <canvas bind:this={canvas} style:height="{height}px" onmousemove={onMove} onmouseleave={() => (hover = null)}></canvas>
    {#if hover}
      <div class="tip" class:right={hover.right} style:left="{hover.x}px" style:top="{hover.y}px">
        <strong>Layer {hover.l} · expert {hover.e}</strong>
        <span>{fmt(hover.t)} lookups{#if hover.t} · VRAM {fmt(hover.vram)} · PCIe {fmt(hover.gpu)} · CPU {fmt(hover.cpu)}{/if}</span>
        <span class:held={hover.held}>{hover.held ? "in the VRAM cache now" : "not in the VRAM cache"}</span>
      </div>
    {/if}
  </div>
</div>

<style>
  .map { position: relative; display: grid; grid-template-columns: 26px minmax(0, 1fr); }
  .axis { position: relative; font: 10px/1 var(--mono); color: var(--off); }
  .axis span { position: absolute; left: 0; transform: translateY(-50%); }
  .field { position: relative; min-width: 0; }
  canvas { display: block; width: 100%; cursor: crosshair; border-radius: 2px; }
  .tip { position: absolute; z-index: 2; transform: translate(12px, 12px); display: flex; flex-direction: column; gap: 2px; padding: 6px 8px;
         background: var(--head); border-radius: var(--r); box-shadow: var(--shadow); font-size: var(--fs-s); color: var(--dim);
         pointer-events: none; white-space: nowrap; }
  .tip.right { transform: translate(calc(-100% - 12px), 12px); }
  .tip strong { color: var(--text); font-weight: var(--fw-b); }
  .tip .held { color: var(--value); }
</style>
