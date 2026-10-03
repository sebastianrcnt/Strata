<script>
  // The Memory tab: how well the VRAM expert cache works, and where the model's parts live. The cache's share of the
  // experts and how it is spread over the layers come from the expert profile (web/expert-profile.js, built from
  // data/expert-profile.bin) and the engine's expert_slots; the hit rates and the traffic from /metrics.
  import {onMount} from "svelte";
  import {server, headers} from "../lib/server.svelte.js";
  import {fmt, gb} from "../lib/format.js";
  import {decodeProfile, vramPerLayer, meanHitRate} from "../lib/experts.js";
  import Panel from "../ui/Panel.svelte";
  import HitChart from "./HitChart.svelte";
  import LayerBars from "./LayerBars.svelte";

  let profile = $state.raw(null), failed = $state(false);
  let requests = $state.raw([]);   // every request the server keeps (the 1-a-second poll carries only the last 12)

  async function loadRequests() {
    try {
      const r = await fetch("metrics?requests=all", {headers: headers()});
      if (r.ok) requests = (await r.json()).requests || [];
    } catch (e) { /* the 1-a-second poll reports the server state */ }
  }
  onMount(() => {
    import(/* @vite-ignore */ new URL("web/expert-profile.js", document.baseURI).href)
      .then((mod) => { profile = decodeProfile(mod.default); })
      .catch(() => { failed = true; });
    loadRequests();
    const t = setInterval(() => { if (!document.hidden) loadRequests(); }, 5000);
    return () => clearInterval(t);
  });

  const m = $derived(server.metrics);
  const eng = $derived(m?.engine || {});
  const hw = $derived(m?.hardware || {});
  const recent = $derived(requests.length ? requests : m?.requests || []);
  const d = $derived.by(() => {
    const total = profile ? profile.layers * profile.experts : 0, slots = eng.expert_slots || 0;
    const share = total ? slots / total : null;
    const last = recent.find((r) => r.hit_rate != null);
    const hit = last?.hit_rate ?? null, mean = meanHitRate(recent);
    const frac = Number(eng.pcie_frac) || 0;
    return {
      total, slots, share, hit, mean, frac, n: recent.filter((r) => r.hit_rate != null).length,
      lift: share && hit != null ? hit / share : null,
      miss: hit == null ? null : 1 - hit,
    };
  });
  const perLayer = $derived(profile && d.slots ? vramPerLayer(profile, d.slots) : null);
  const pct = (x, digits = 1) => (x == null ? "–" : `${fmt(100 * x, digits)}%`);
  const rate = (mb) => (mb == null ? "–" : mb >= 1000 ? `${fmt(mb / 1024, 2)} GB/s` : `${fmt(mb, mb < 10 ? 1 : 0)} MB/s`);
</script>

<div class="memory">
  <Panel title="Expert cache" lamp={d.hit != null ? "value" : ""}>
    {#if failed}
      <p class="muted">The expert profile (web/expert-profile.js) is not there: build the web app again.</p>
    {/if}
    <p class="lead">
      VRAM holds <b>{pct(d.share)}</b> of the experts and answers <b class="v">{pct(d.hit)}</b> of the lookups{#if d.lift}&nbsp;— <b>{fmt(d.lift, 1)}×</b> what a random pick of the same size would{/if}.
    </p>

    <div class="cmp">
      <span class="cmp__k">Experts in VRAM</span>
      <span class="track"><i class="f-share" style:width={pct(d.share)}></i></span>
      <span class="cmp__v">{pct(d.share)}</span>
      <span class="cmp__s">{fmt(d.slots)} of {fmt(d.total)}</span>

      <span class="cmp__k">Lookups found there</span>
      <span class="track"><i class="f-hit" style:width={pct(d.hit)}></i>{#if d.miss}<i class="f-pcie" style:width={pct(d.miss * d.frac)}></i><i class="f-cpu" style:width={pct(d.miss * (1 - d.frac))}></i>{/if}</span>
      <span class="cmp__v v">{pct(d.hit)}</span>
      <span class="cmp__s">last request · {pct(d.mean)} over {fmt(d.n)}</span>
    </div>
    {#if d.miss != null}
      <p class="miss">
        The other {pct(d.miss)}: <span class="k f-pcie"></span>copied to the GPU over PCIe ≈ {pct(d.miss * d.frac)} ·
        <span class="k f-cpu"></span>computed by the CPU in RAM ≈ {pct(d.miss * (1 - d.frac))}
        <span class="muted">(split by the engine's pcie_frac {eng.pcie_frac ?? "–"})</span>
      </p>
    {/if}
  </Panel>

  <div class="two">
    <Panel title="Hit rate per request" lamp={null}>
      <HitChart requests={recent} share={d.share} />
    </Panel>
    <Panel title="Cache per layer" lamp={null}>
      {#if perLayer}<LayerBars {perLayer} experts={profile.experts} />{:else}<p class="muted">–</p>{/if}
      <p class="note">The fill at start, from the expert profile. While you chat the engine swaps a few experts toward the
        conversation; it does not report which.</p>
    </Panel>
  </div>

  <Panel title="Where the model lives" lamp={null} flush>
    <div class="tiers">
      <section class="tier tier--hot">
        <span class="tag">VRAM</span>
        <span class="name">Expert cache</span>
        <span class="big">{fmt(d.slots)} <small>experts · {gb((eng.expert_cache_mib || 0) * 1048576)} GB</small></span>
        <span class="sub">computed by the GPU · PCIe in now {rate(hw.gpu_pcie_rx_mb)}</span>
        <span class="gauge" title="VRAM used"><i style:width={pct(hw.gpu_mem_total ? hw.gpu_mem_used / hw.gpu_mem_total : 0)}></i></span>
      </section>
      <section class="tier">
        <span class="tag">RAM</span>
        <span class="name">Expert arena</span>
        <span class="big">{fmt(d.total)} <small>experts · {gb((eng.arena_mib || 0) * 1048576)} GB</small></span>
        <span class="sub">every expert; the CPU computes the misses in place · CPU {fmt(hw.cpu)}%</span>
        <span class="gauge" title="System RAM used"><i style:width={pct(hw.ram_total ? hw.ram_used / hw.ram_total : 0)}></i></span>
      </section>
      <section class="tier">
        <span class="tag">NVMe SSD</span>
        <span class="name">PLE n-gram table</span>
        <span class="big">{rate(hw.disk_read_mb)} <small>read now</small></span>
        <span class="sub">only the rows each token needs, read directly; the table never goes to RAM or the GPU</span>
      </section>
    </div>
  </Panel>
</div>

<style>
  .memory { display: flex; flex-direction: column; gap: var(--pgap); padding: var(--pgap); }
  .memory > :global(*) { flex: none; }
  .lead { margin: 2px 0 12px; font-size: var(--fs-l); color: var(--dim); line-height: 1.4; }
  .lead b { color: var(--text); font-weight: var(--fw-b); }
  .lead b.v, .cmp__v.v { color: var(--value); }
  .cmp { display: grid; grid-template-columns: 130px minmax(0, 1fr) 56px auto; align-items: center; gap: 8px 12px; max-width: 1100px; }
  .cmp__k { font-size: var(--fs-s); color: var(--dim); }
  .cmp__v { font: var(--fw-b) var(--fs-l) var(--mono); text-align: right; font-variant-numeric: tabular-nums; }
  .cmp__s { font-size: var(--fs-s); color: var(--off); white-space: nowrap; }
  .track { display: flex; height: 14px; background: var(--well); border-radius: 2px; overflow: hidden; }
  .track i { display: block; height: 100%; }
  .f-share { background: var(--dim); }
  .f-hit { background: var(--value); }
  .f-pcie { background: var(--text); opacity: .55; }
  .f-cpu { background: var(--accent); opacity: .8; }
  .miss { margin: 10px 0 0; font-size: var(--fs-s); color: var(--dim); }
  .k { display: inline-block; width: 8px; height: 8px; margin: 0 5px 0 4px; border-radius: 1px; }
  .two { display: grid; grid-template-columns: minmax(0, 1.6fr) minmax(0, 1fr); gap: var(--pgap); }
  .note { margin: 8px 0 0; font-size: var(--fs-s); color: var(--off); }

  .tiers { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: var(--pgap); padding: var(--pgap); }
  .tier { display: flex; flex-direction: column; gap: 4px; min-width: 0; padding: 10px 12px; background: var(--well); border-radius: var(--r); }
  .tag { font: var(--fw-b) 10px/1 var(--mono); letter-spacing: .12em; color: var(--off); }
  .tier--hot .tag, .tier--hot .big { color: var(--value); }
  .name { font-size: var(--fs-s); color: var(--dim); }
  .big { font: var(--fw-b) 18px/1.2 var(--mono); font-variant-numeric: tabular-nums; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .big small { font: var(--fw) var(--fs-s) var(--font); color: var(--dim); }
  .sub { font-size: var(--fs-s); color: var(--dim); line-height: 1.35; }
  .gauge { margin-top: auto; height: 3px; background: var(--cell); border-radius: 2px; overflow: hidden; }
  .gauge i { display: block; height: 100%; background: var(--dim); }
  .tier--hot .gauge i { background: var(--value); }

  @media (max-width: 800px) {
    .two, .tiers { grid-template-columns: minmax(0, 1fr); }
    .cmp { grid-template-columns: minmax(0, 1fr) auto; }
    .cmp__k { grid-column: 1 / -1; margin-bottom: -4px; }
    .cmp__s { grid-column: 1 / -1; margin-top: -4px; }
  }
</style>
