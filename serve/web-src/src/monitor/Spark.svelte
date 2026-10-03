<script>
  // One track's line over the chosen time range, one sample a second, the newest at the right edge. A gap in the
  // samples (null) leaves a gap in the line. The top of the chart is labelled with its scale; hovering reads a sample.
  import {fmt} from "../lib/format.js";

  let {values = [], max = 0, range = 60, label = "", unit = "", folded = false} = $props();

  const v = $derived((values || []).slice(-range));
  const top = $derived(Math.max(max || 0, ...v.filter((x) => x != null), 1e-9));
  const x = (i, n) => 100 - ((n - 1 - i) / Math.max(1, range - 1)) * 100;
  const y = (val) => 31 - (val / top) * 29;
  const paths = $derived.by(() => {
    let line = "", area = "", seg = [];
    const flush = () => {
      if (seg.length > 1) {
        const d = seg.map((p, i) => `${i ? "L" : "M"}${p[0].toFixed(2)},${p[1].toFixed(2)}`).join("");
        line += d;
        area += `${d}L${seg[seg.length - 1][0].toFixed(2)},32L${seg[0][0].toFixed(2)},32Z`;
      }
      seg = [];
    };
    v.forEach((val, i) => { if (val == null) flush(); else seg.push([x(i, v.length), y(val)]); });
    flush();
    return {line, area};
  });
  const num = (n) => fmt(n, n < 10 && n % 1 ? 1 : 0);

  let hover = $state(null);   // {i, left}
  function move(e) {
    const b = e.currentTarget.getBoundingClientRect();
    const frac = (e.clientX - b.left) / b.width;
    const i = Math.round(v.length - 1 - (1 - frac) * (range - 1));
    hover = i >= 0 && i < v.length ? {i, left: frac * 100} : null;
  }
</script>

<div class="spark" class:folded role="img" aria-label="{label} history" onpointermove={move} onpointerleave={() => (hover = null)}>
  <svg viewBox="0 0 100 32" preserveAspectRatio="none">
    <path d={paths.area} class="area" />
    <path d={paths.line} class="line" vector-effect="non-scaling-stroke" />
  </svg>
  {#if !folded && top > 1e-6}<span class="scale">{num(top)}{unit ? ` ${unit}` : ""}</span>{/if}
  {#if hover}
    <i class="rule" style:left="{hover.left}%"></i>
    <span class="read" class:read--left={hover.left > 60} style:left="{hover.left}%">
      {v[hover.i] == null ? "–" : num(v[hover.i])}{unit ? ` ${unit}` : ""} · {v.length - 1 - hover.i ? `${v.length - 1 - hover.i} s ago` : "now"}
    </span>
  {/if}
</div>

<style>
  .spark { position: relative; align-self: stretch; min-width: 0; overflow: hidden; background-color: var(--chart); cursor: crosshair;
           background-image: repeating-linear-gradient(to right, transparent 0, transparent calc(25% - 1px), var(--chart-grid) calc(25% - 1px), var(--chart-grid) 25%); }
  svg { position: absolute; inset: 4px 0 2px; width: 100%; height: calc(100% - 6px); overflow: visible; }
  .area { fill: var(--value); opacity: .14; }
  .line { fill: none; stroke: var(--value); stroke-width: 1.6; stroke-linejoin: round; }
  .folded svg { inset: 2px 0 1px; height: calc(100% - 3px); }
  .folded .line { stroke-width: 1.2; }
  .scale { position: absolute; top: 2px; left: 3px; padding: 0 3px; border-radius: 2px; background: var(--chart); font-size: var(--fs-s); color: var(--dim); pointer-events: none; }
  .rule { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--text); opacity: .35; pointer-events: none; }
  .read { position: absolute; top: 2px; margin-left: 6px; padding: 0 4px; border-radius: 2px; font-size: var(--fs-s); white-space: nowrap;
          background: var(--head); color: var(--text); pointer-events: none; }
  .read--left { transform: translateX(calc(-100% - 12px)); }
</style>
