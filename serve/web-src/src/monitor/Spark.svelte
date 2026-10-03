<script>
  // One track's line over the chosen time range, one sample a second, the newest at the right edge. A gap in the
  // samples (null) leaves a gap in the line. Hovering reads a sample.
  import {fmt} from "../lib/format.js";

  let {values = [], max = 0, range = 60, label = "", unit = ""} = $props();

  const v = $derived((values || []).slice(-range));
  const top = $derived(Math.max(max || 0, ...v.filter((x) => x != null), 1e-9));
  const x = (i, n) => 100 - ((n - 1 - i) / Math.max(1, range - 1)) * 100;
  const y = (val) => 30 - (val / top) * 26;
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

  let hover = $state(null);   // {i, left}
  function move(e) {
    const b = e.currentTarget.getBoundingClientRect();
    const frac = (e.clientX - b.left) / b.width;
    const i = Math.round(v.length - 1 - (1 - frac) * (range - 1));
    hover = i >= 0 && i < v.length ? {i, left: frac * 100} : null;
  }
</script>

<div class="spark" role="img" aria-label="{label} history" onpointermove={move} onpointerleave={() => (hover = null)}>
  <svg viewBox="0 0 100 32" preserveAspectRatio="none">
    <path d={paths.area} fill="currentColor" opacity=".07" />
    <path d={paths.line} fill="none" stroke="currentColor" stroke-width="1.25" vector-effect="non-scaling-stroke" />
  </svg>
  {#if hover}
    <i class="spark__rule" style:left="{hover.left}%"></i>
    <span class="spark__read" class:spark__read--left={hover.left > 60} style:left="{hover.left}%">
      {v[hover.i] == null ? "–" : fmt(v[hover.i], v[hover.i] < 10 ? 1 : 0)}{unit ? ` ${unit}` : ""} · {v.length - 1 - hover.i ? `${v.length - 1 - hover.i} s ago` : "now"}
    </span>
  {/if}
</div>

<style>
  .spark { position: relative; align-self: stretch; min-height: 0; min-width: 0; overflow: hidden; color: var(--track-color); cursor: crosshair;
           background: repeating-linear-gradient(to right, transparent 0, transparent calc(25% - 1px), var(--st-line-soft) calc(25% - 1px), var(--st-line-soft) 25%); }
  svg { position: absolute; inset: 3px 0 1px; width: 100%; height: calc(100% - 4px); overflow: visible; }
  .spark__rule { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--st-ink-muted); opacity: .5; pointer-events: none; }
  .spark__read { position: absolute; top: 2px; margin-left: 6px; padding: 0 4px; font-size: 10px; white-space: nowrap;
                 background: var(--st-surface-2); color: var(--st-ink); pointer-events: none; }
  .spark__read--left { transform: translateX(calc(-100% - 12px)); }
</style>
