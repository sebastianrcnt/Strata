<script>
  // The three places the experts live, left to right as the data moves: the model file on the SSD (read into RAM at
  // start), the RAM arena (every expert; the CPU computes the ones the GPU lacks in place) and the VRAM cache (the most
  // used ones). The links carry the measured disk and PCIe traffic; their dots move faster as it grows.
  import {fmt, gb} from "../lib/format.js";

  let {metrics, total} = $props();

  const d = $derived.by(() => {
    const hw = metrics?.hardware || {}, eng = metrics?.engine || {}, last = metrics?.requests?.[0] || {};
    const slots = eng.expert_slots || 0, mib = 1048576;
    const hit = last.hit_rate ?? null, frac = Number(eng.pcie_frac) || 0;
    // where a request's expert work ran: cached on the GPU, copied over PCIe to the GPU, or on the CPU in RAM
    const split = hit == null ? null : {gpu: hit, pcie: (1 - hit) * frac, cpu: (1 - hit) * (1 - frac)};
    return {
      slots, hit, split, frac,
      cacheB: (eng.expert_cache_mib || 0) * mib, arenaB: (eng.arena_mib || 0) * mib,
      disk: hw.disk_read_mb, pcie: hw.gpu_pcie_rx_mb,
      ram: hw.ram_total ? hw.ram_used / hw.ram_total : 0, vram: hw.gpu_mem_total ? hw.gpu_mem_used / hw.gpu_mem_total : 0,
      hw, fileBlobs: last.file_blobs, fileMb: last.file_mb,
    };
  });
  // dots per second along a link: none when nothing moves, then by the order of magnitude
  const speed = (mb) => (!mb || mb < .5 ? 0 : 2.6 / (1 + Math.log10(1 + mb)));
  const rate = (mb) => (mb == null ? "–" : mb >= 1000 ? `${fmt(mb / 1024, 2)} GB/s` : `${fmt(mb, mb < 10 ? 1 : 0)} MB/s`);
  const pct = (x, digits = 1) => `${fmt(100 * x, digits)}%`;
</script>

<div class="tiers">
  <section class="tier" aria-label="SSD">
    <span class="tag">SSD</span>
    <span class="name">Model file · disk read now</span>
    <span class="big">{rate(d.disk)}</span>
    <span class="sub">{d.fileBlobs ? `${fmt(d.fileBlobs)} experts (${fmt(d.fileMb, 0)} MB) read from it in the last request`
      : "read into RAM at start; the last request read no expert from it"}</span>
  </section>

  <div class="link" class:still={!speed(d.disk)} style:--dur="{speed(d.disk) || 1}s" aria-hidden="true">
    <span class="lk">disk</span><i></i><span>{rate(d.disk)}</span></div>

  <section class="tier" aria-label="RAM">
    <span class="tag">RAM</span>
    <span class="name">Expert arena</span>
    <span class="big">{fmt(total)} <small>experts</small></span>
    <span class="sub">every expert · {gb(d.arenaB)} GB · the CPU computes the ones the GPU lacks in place
      ({fmt(d.hw.cpu)}% CPU)</span>
    <span class="gauge" title="System RAM used"><i style:width={pct(d.ram)}></i></span>
  </section>

  <div class="link" class:still={!speed(d.pcie)} style:--dur="{speed(d.pcie) || 1}s" aria-hidden="true">
    <span class="lk">PCIe</span><i></i><span>{rate(d.pcie)}</span></div>

  <section class="tier tier--hot" aria-label="VRAM">
    <span class="tag">VRAM</span>
    <span class="name">Expert cache</span>
    <span class="big">{fmt(d.slots)} <small>experts</small></span>
    <span class="sub">the most used {total ? pct(d.slots / total) : "–"} · {gb(d.cacheB)} GB · the GPU computes them
      ({fmt(d.hw.gpu_util)}% GPU)</span>
    <span class="gauge" title="VRAM used"><i style:width={pct(d.vram)}></i></span>
  </section>

  <section class="split" aria-label="Where the expert work ran">
    <span class="name">Where the last request's expert work ran</span>
    {#if d.split}
      <span class="bar">
        <i class="gpu" style:width={pct(d.split.gpu)}></i><i class="pcie" style:width={pct(d.split.pcie)}></i><i class="cpu" style:width={pct(d.split.cpu)}></i>
      </span>
      <span class="keys">
        <span><b class="k gpu"></b>VRAM cache {pct(d.split.gpu)}</span>
        <span><b class="k pcie"></b>copied over PCIe ≈ {pct(d.split.pcie)}</span>
        <span><b class="k cpu"></b>CPU in RAM ≈ {pct(d.split.cpu)}</span>
      </span>
    {:else}
      <span class="sub">after the first request</span>
    {/if}
  </section>
</div>

<style>
  .tiers { display: grid; grid-template-columns: minmax(0, 1fr) 92px minmax(0, 1fr) 92px minmax(0, 1fr) minmax(0, 1.1fr); gap: 0;
           align-items: stretch; padding: 10px; }
  .tier, .split { position: relative; display: flex; flex-direction: column; gap: 4px; min-width: 0; padding: 10px 12px 12px;
                  background: var(--well); border-radius: var(--r); }
  /* corner brackets: the console's instrument frame */
  .tier::before, .tier::after { content: ""; position: absolute; width: 8px; height: 8px; border: 1px solid var(--off); pointer-events: none; }
  .tier::before { top: 3px; left: 3px; border-right: 0; border-bottom: 0; }
  .tier::after { bottom: 3px; right: 3px; border-left: 0; border-top: 0; }
  .tier--hot { background: linear-gradient(160deg, var(--value-tint), var(--well) 70%); }
  .tier--hot::before, .tier--hot::after { border-color: var(--value); }
  .split { margin-left: 10px; justify-content: center; }
  .tag { font: var(--fw-b) 10px/1 var(--mono); letter-spacing: .14em; color: var(--off); }
  .tier--hot .tag { color: var(--value); }
  .name { font-size: var(--fs-s); color: var(--dim); }
  .big { font: var(--fw-b) 22px/1.15 var(--mono); color: var(--text); font-variant-numeric: tabular-nums; white-space: nowrap; }
  .tier--hot .big { color: var(--value); }
  .big small { font: var(--fw) var(--fs-s) var(--font); color: var(--dim); }
  .sub { font-size: var(--fs-s); color: var(--dim); line-height: 1.35; }
  .gauge { margin-top: auto; height: 3px; background: var(--cell); border-radius: 2px; overflow: hidden; }
  .gauge i { display: block; height: 100%; background: var(--dim); }
  .tier--hot .gauge i { background: var(--value); }

  /* a link: a dashed conduit whose dots run left to right at the measured rate */
  .link { position: relative; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px; padding: 0 6px; }
  .link i { width: 100%; height: 2px;
            background: repeating-linear-gradient(90deg, var(--value) 0 3px, transparent 3px 11px);
            animation: flow var(--dur) linear infinite; box-shadow: 0 0 8px var(--value-tint); }
  .link span { font: 10px/1 var(--mono); color: var(--dim); white-space: nowrap; }
  .link .lk { color: var(--off); letter-spacing: .1em; text-transform: uppercase; }
  .link.still i { background: repeating-linear-gradient(90deg, var(--off) 0 3px, transparent 3px 11px); animation: none; box-shadow: none; }
  @keyframes flow { from { background-position: 0 0; } to { background-position: 11px 0; } }
  @media (prefers-reduced-motion: reduce) { .link i { animation: none; } }

  .bar { display: flex; height: 10px; border-radius: 2px; overflow: hidden; background: var(--cell); margin: 4px 0 2px; }
  .bar i { display: block; height: 100%; }
  .gpu { background: var(--value); }
  .pcie { background: var(--text); }
  .cpu { background: var(--accent); }
  .keys { display: flex; flex-direction: column; gap: 3px; font-size: var(--fs-s); color: var(--dim); }
  .k { display: inline-block; width: 8px; height: 8px; margin-right: 6px; border-radius: 1px; vertical-align: -1px; }

  @media (max-width: 1100px) {
    .tiers { grid-template-columns: minmax(0, 1fr) 72px minmax(0, 1fr) 72px minmax(0, 1fr); row-gap: 10px; }
    .split { grid-column: 1 / -1; margin-left: 0; }
  }
  @media (max-width: 640px) {
    .tiers { grid-template-columns: minmax(0, 1fr); }
    .link { height: 34px; flex-direction: row; }
    .link i { width: 2px; height: 100%; background: repeating-linear-gradient(180deg, var(--value) 0 3px, transparent 3px 11px);
              animation-name: flow-down; }
    .link.still i { background: repeating-linear-gradient(180deg, var(--off) 0 3px, transparent 3px 11px); }
    @keyframes flow-down { from { background-position: 0 0; } to { background-position: 0 11px; } }
  }
</style>
