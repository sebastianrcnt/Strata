<script>
  // Every expert of the model as one cell: 48 rows (layers) x 512 columns. Cyan cells are the ones the VRAM cache holds
  // at start (brighter = ranked higher), grey ones live only in RAM. While the model works, a sweep runs down the
  // layers and lights the experts one token could route to: 10 a layer, from the cache at the measured hit rate,
  // else over PCIe or on the CPU. The engine does not report which experts it routes to, so the lights are an
  // impression at the real rates; the placement is the real profile.
  import {onMount} from "svelte";
  import {ui} from "../lib/ui.svelte.js";
  import {fmt} from "../lib/format.js";

  let {profile, slots, phase = "idle", hitRate = null, pcieFrac = 0, layout = "id"} = $props();

  let wrap, canvas, w = $state(0), hover = $state(null);
  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const L = $derived(profile.layers), E = $derived(profile.experts);
  // rows get taller on wide screens; a phone still shows all 48
  const rowH = $derived(Math.max(4, Math.min(9, Math.round(w / 150))));
  const height = $derived(L * rowH);

  // a cell's column: its expert id, or its place in the layer's ranking
  const colOf = (l, e) => (layout === "rank" ? posInLayer[l * E + e] : e);
  let posInLayer = new Uint16Array(0);
  $effect(() => {
    const pos = new Uint16Array(L * E);
    for (let l = 0; l < L; l++) for (let k = 0; k < E; k++) pos[l * E + profile.order[l * E + k]] = k;
    posInLayer = pos;
  });

  // ------------------------------------------------------------------ colors from the theme's tokens
  let colors = {};
  function readColors() {
    const s = getComputedStyle(document.documentElement);
    // as #rrggbb (the built CSS may shorten #dd7700 to #d70), so a two-digit alpha can be appended
    const norm = document.createElement("canvas").getContext("2d");
    const v = (n) => { norm.fillStyle = "#000"; norm.fillStyle = s.getPropertyValue(n).trim(); return norm.fillStyle; };
    colors = {value: v("--value"), accent: v("--accent"), text: v("--text"), off: v("--off"), chart: v("--chart"), well: v("--well"),
              light: document.documentElement.dataset.theme === "light"};
    glow = {hit: sprite(colors.value), pcie: sprite(colors.text), cpu: sprite(colors.accent)};
  }
  // a soft round light, drawn once per color and scaled onto the map
  let glow = {};
  function sprite(color) {
    const c = document.createElement("canvas");
    c.width = c.height = 64;
    const g = c.getContext("2d"), r = g.createRadialGradient(32, 32, 0, 32, 32, 32);
    r.addColorStop(0, color); r.addColorStop(.35, color + "66"); r.addColorStop(1, color + "00");
    g.fillStyle = r; g.fillRect(0, 0, 64, 64);
    return c;
  }

  // ------------------------------------------------------------------ the base picture, drawn once per size / layout
  let base = null, boot = 0;   // boot: 0..1, the cache filling in rank order when the tab opens
  function drawBase(fill) {
    const dpr = devicePixelRatio || 1, cw = (w * dpr) / E, rh = rowH * dpr;
    base ??= document.createElement("canvas");
    base.width = Math.round(w * dpr); base.height = Math.round(height * dpr);
    const g = base.getContext("2d");
    g.fillStyle = colors.well; g.fillRect(0, 0, base.width, base.height);
    const gapY = rh >= 5 ? dpr : 0, gapX = cw >= 2.5 ? dpr * .5 : 0;
    const lit = Math.floor(slots * fill);
    for (let l = 0; l < L; l++) {
      for (let e = 0; e < E; e++) {
        const r = profile.rank[l * E + e], x = colOf(l, e) * cw, y = l * rh;
        if (r < lit) {
          g.globalAlpha = .95 - .55 * (r / Math.max(1, slots));
          g.fillStyle = colors.value;
        } else {
          g.globalAlpha = (colors.light ? .30 : .20) - .12 * Math.min(1, (r - slots) / Math.max(1, profile.rank.length - slots));
          g.fillStyle = colors.text;
        }
        g.fillRect(x, y, Math.max(dpr * .6, cw - gapX), rh - gapY);
      }
    }
    g.globalAlpha = 1;
  }

  // ------------------------------------------------------------------ the sweep and its lights
  const sparks = [];          // {x, y, kind, t}
  let sweepT = 0, lastLayer = -1, last = 0, raf = 0;
  const SWEEP_MS = 1400, FADE_MS = 900;
  // pick one of layer l's experts: a hit from its cached share (skewed to the top ranks), a miss from the rest
  function pick(l, share) {
    const hit = Math.random() < (hitRate ?? .9);
    const k = hit ? Math.floor(share * Math.random() ** 2) : share + Math.floor((E - share) * Math.random() ** 1.6);
    const e = profile.order[l * E + Math.min(E - 1, k)];
    return {e, kind: hit ? "hit" : Math.random() < pcieFrac ? "pcie" : "cpu"};
  }
  let share = new Int32Array(0);
  $effect(() => {
    const s = new Int32Array(L);
    for (let i = 0; i < profile.rank.length; i++) if (profile.rank[i] < slots) s[(i / E) | 0]++;
    share = s;
  });

  function frame(now) {
    raf = 0;
    if (!canvas || !w) return;
    const dt = last ? Math.min(100, now - last) : 16;
    last = now;
    const busy = phase === "generating" || phase === "reading";
    if (boot < 1) { boot = reduce ? 1 : Math.min(1, boot + dt / 1600); drawBase(easeOut(boot)); }
    if (busy && !reduce) {
      sweepT = (sweepT + dt / SWEEP_MS) % 1;
      const layer = Math.floor(sweepT * L);
      // light the layers the sweep passed since the last frame: 10 experts each (reading a prompt touches far more)
      if (layer !== lastLayer) {
        const n = phase === "reading" ? 40 : 10;
        let l = lastLayer;
        do {
          l = (l + 1) % L;
          for (let i = 0; i < n; i++) { const p = pick(l, share[l]); sparks.push({l, e: p.e, kind: p.kind, t: now}); }
        } while (l !== layer);
      }
      lastLayer = layer;
    } else {
      lastLayer = -1;
    }
    paint(now, busy);
    if (boot < 1 || busy || sparks.length) raf = requestAnimationFrame(frame);
  }
  const easeOut = (t) => 1 - (1 - t) ** 3;

  function paint(now, busy) {
    const dpr = devicePixelRatio || 1, g = canvas.getContext("2d");
    const cw = (w * dpr) / E, rh = rowH * dpr;
    g.globalCompositeOperation = "source-over";
    g.globalAlpha = 1;
    g.drawImage(base, 0, 0);
    if (busy && !reduce) {
      // the sweep: a soft band with a bright leading edge
      const y = sweepT * L * rh, band = rh * 6;
      const grad = g.createLinearGradient(0, y - band, 0, y + rh);
      grad.addColorStop(0, "transparent");
      grad.addColorStop(.85, colors.value + "33");
      grad.addColorStop(1, colors.value + "aa");
      g.fillStyle = grad;
      g.fillRect(0, y - band, canvas.width, band + rh);
    }
    g.globalCompositeOperation = colors.light ? "source-over" : "lighter";
    let keep = 0;
    for (const s of sparks) {
      const age = (now - s.t) / FADE_MS;
      if (age >= 1) continue;
      sparks[keep++] = s;
      const a = (1 - age) ** 2, x = colOf(s.l, s.e) * cw, y = s.l * rh;
      const c = s.kind === "hit" ? colors.value : s.kind === "pcie" ? colors.text : colors.accent;
      const halo = rh * (2.2 + 2.5 * age);
      g.globalAlpha = a * .8;
      g.drawImage(glow[s.kind], x + cw / 2 - halo, y + rh / 2 - halo, halo * 2, halo * 2);
      g.globalAlpha = a;
      g.fillStyle = s.kind === "hit" && !colors.light ? "#e9fbff" : c;
      g.fillRect(x - dpr * .5, y, Math.max(cw, dpr * 1.5) + dpr, rh);
    }
    sparks.length = keep;
    g.globalAlpha = 1;
    g.globalCompositeOperation = "source-over";
  }

  const kick = () => { if (!raf && !document.hidden) raf = requestAnimationFrame(frame); };

  // redraw from scratch when the size, the layout, the theme or the cache size change
  $effect(() => {
    void [w, layout, ui.theme, slots, posInLayer, rowH];
    if (!canvas || !w) return;
    readColors();
    const dpr = devicePixelRatio || 1;
    canvas.width = Math.round(w * dpr); canvas.height = Math.round(height * dpr);
    drawBase(boot < 1 ? easeOut(boot) : 1);
    paint(performance.now(), false);
    kick();
  });
  $effect(() => { void phase; kick(); });

  onMount(() => {
    const ro = new ResizeObserver(([e]) => { w = Math.floor(e.contentRect.width); });
    ro.observe(wrap);
    const vis = () => { if (!document.hidden) { last = 0; kick(); } };
    document.addEventListener("visibilitychange", vis);
    return () => { ro.disconnect(); document.removeEventListener("visibilitychange", vis); cancelAnimationFrame(raf); };
  });

  function onMove(ev) {
    const r = canvas.getBoundingClientRect();
    const l = Math.floor(((ev.clientY - r.top) / r.height) * L), col = Math.floor(((ev.clientX - r.left) / r.width) * E);
    if (l < 0 || l >= L || col < 0 || col >= E) { hover = null; return; }
    const e = layout === "rank" ? profile.order[l * E + col] : col;
    const rank = profile.rank[l * E + e];
    hover = {l, e, rank, vram: rank < slots, x: ev.clientX - r.left, y: ev.clientY - r.top, right: ev.clientX - r.left > r.width - 220};
  }
</script>

<div class="map" bind:this={wrap}>
  <div class="axis" aria-hidden="true">
    {#each [0, 8, 16, 24, 32, 40, 47] as l}<span style:top="{(l + .5) * rowH}px">L{l}</span>{/each}
  </div>
  <div class="field" role="img" aria-label="{fmt(L * E)} experts, {fmt(slots)} of them in the VRAM cache">
    <canvas bind:this={canvas} style:height="{height}px" onmousemove={onMove} onmouseleave={() => (hover = null)}></canvas>
    {#if hover}
      <div class="tip" class:right={hover.right} style:left="{hover.x}px" style:top="{hover.y}px">
        <strong>Layer {hover.l} · expert {hover.e}</strong>
        <span>rank {fmt(hover.rank + 1)} of {fmt(L * E)}</span>
        <span class:vram={hover.vram}>{hover.vram ? "in the VRAM cache at start" : "in RAM (CPU, or copied over PCIe)"}</span>
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
  .tip .vram { color: var(--value); }
</style>
