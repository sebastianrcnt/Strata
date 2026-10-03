<script>
  // The Memory tab: how well the VRAM expert cache works, and where the model's parts live. With an engine that reports
  // its expert usage (GET /experts) every number is measured: lookups per expert by where they ran, and what the
  // cache holds now. Without it the cache's share and spread come from the expert profile (web/expert-profile.js,
  // built from data/expert-profile.bin) and the hit rates from the requests in /metrics.
  import {onMount} from "svelte";
  import {server, headers} from "../lib/server.svelte.js";
  import {fmt, gb} from "../lib/format.js";
  import {decodeProfile, vramPerLayer, meanHitRate} from "../lib/experts.js";
  import {decodeUsage, since, summarize} from "../lib/usage.js";
  import {store} from "../lib/storage.js";
  import Panel from "../ui/Panel.svelte";
  import Segmented from "../ui/Segmented.svelte";
  import HitChart from "./HitChart.svelte";
  import LayerBars from "./LayerBars.svelte";
  import UsageMap from "./UsageMap.svelte";

  let profile = $state.raw(null), failed = $state(false);
  let requests = $state.raw([]);   // every request the server keeps (the 1-a-second poll carries only the last 12)
  let usage = $state.raw(null);    // {layers, experts, now, before} decoded, or null: the engine does not report it
  let usageAt = 0;
  let scope = $state(store.get("memory.scope", "start"));
  let layout = $state(store.get("memory.layout", "id"));
  $effect(() => { store.set("memory.scope", scope); store.set("memory.layout", layout); });

  async function load() {
    try {
      const [r, x] = await Promise.all([fetch("metrics?requests=all", {headers: headers()}), fetch("experts", {headers: headers()})]);
      if (r.ok) requests = (await r.json()).requests || [];
      const e = x.ok ? await x.json() : null;
      if (!e?.available) usage = null;
      else if (e.at !== usageAt) {
        usageAt = e.at;
        const now = decodeUsage(e.now, e.layers, e.experts);
        usage = now && {layers: e.layers, experts: e.experts, now, before: e.before ? decodeUsage(e.before, e.layers, e.experts) : null};
      }
    } catch (err) { /* the 1-a-second poll reports the server state */ }
  }
  onMount(() => {
    import(/* @vite-ignore */ new URL("web/expert-profile.js", document.baseURI).href)
      .then((mod) => { profile = decodeProfile(mod.default); })
      .catch(() => { failed = true; });
    load();
    const t = setInterval(() => { if (!document.hidden) load(); }, 5000);
    return () => clearInterval(t);
  });

  const m = $derived(server.metrics);
  const eng = $derived(m?.engine || {});
  const hw = $derived(m?.hardware || {});
  const recent = $derived(requests.length ? requests : m?.requests || []);
  // the measured counts in the chosen scope: since the engine started, or the last request alone
  const U = $derived(usage ? (scope === "last" && usage.before ? since(usage.now, usage.before) : usage.now) : null);
  const S = $derived(U ? summarize(U, usage.layers, usage.experts) : null);
  const d = $derived.by(() => {
    const total = usage ? usage.layers * usage.experts : profile ? profile.layers * profile.experts : 0;
    const slots = S ? S.held : eng.expert_slots || 0;
    const share = total ? slots / total : null;
    const last = recent.find((r) => r.hit_rate != null);
    const mean = meanHitRate(recent), frac = Number(eng.pcie_frac) || 0;
    // measured: the lookups by where they ran; else the last request's hit rate, its misses split by pcie_frac
    const hit = S ? S.hit : last?.hit_rate ?? null;
    const gpu = S ? (S.all ? S.gpu / S.all : null) : hit == null ? null : (1 - hit) * frac;
    const cpu = S ? (S.all ? S.cpu / S.all : null) : hit == null ? null : (1 - hit) * (1 - frac);
    return {total, slots, share, hit, gpu, cpu, mean, n: recent.filter((r) => r.hit_rate != null).length,
            lift: share && hit != null ? hit / share : null};
  });
  const perLayerProfile = $derived(!S && profile && d.slots ? vramPerLayer(profile, d.slots) : null);
  const pct = (x, digits = 1) => (x == null ? "–" : `${fmt(100 * x, digits)}%`);
  const rate = (mb) => (mb == null ? "–" : mb >= 1000 ? `${fmt(mb / 1024, 2)} GB/s` : `${fmt(mb, mb < 10 ? 1 : 0)} MB/s`);
  const layerHits = $derived(S ? S.perLayer.map((l) => l.hit) : []);
  const hitRange = $derived.by(() => {
    const v = layerHits.filter((x) => x != null);
    return v.length ? `From ${pct(Math.min(...v), 0)} to ${pct(Math.max(...v), 0)} found in VRAM, by layer` : "No lookups yet";
  });
</script>

<div class="memory">
  <Panel title="Expert cache" lamp={d.hit != null ? "value" : ""}>
    {#snippet tools()}
      {#if usage}<Segmented bind:value={scope} label="Counted over" options={[["start", "Since start"], ["last", "Last request"]]} />{/if}
    {/snippet}
    {#if failed && !usage}
      <p class="muted">The expert profile (web/expert-profile.js) is not there: build the web app again.</p>
    {/if}
    <p class="lead">
      VRAM holds <b>{pct(d.share)}</b> of the experts and answers <b class="v">{pct(d.hit)}</b> of the lookups{#if d.lift}&nbsp;— <b>{fmt(d.lift, 1)}×</b> what a random pick of the same size would{/if}.
    </p>

    <div class="cmp">
      <span class="cmp__k">Experts in VRAM</span>
      <span class="track"><i class="f-share" style:width={pct(d.share)}></i></span>
      <span class="cmp__v">{pct(d.share)}</span>
      <span class="cmp__s">{fmt(d.slots)} of {fmt(d.total)}{S ? " now" : ""}</span>

      <span class="cmp__k">Lookups found there</span>
      <span class="track"><i class="f-hit" style:width={pct(d.hit)}></i><i class="f-pcie" style:width={pct(d.gpu)}></i><i class="f-cpu" style:width={pct(d.cpu)}></i></span>
      <span class="cmp__v v">{pct(d.hit)}</span>
      <span class="cmp__s">{S ? `${fmt(S.all)} lookups ${scope === "last" && usage.before ? "in the last request" : "since the engine started"}`
        : `last request · ${pct(d.mean)} over ${fmt(d.n)}`}</span>
    </div>
    {#if d.hit != null}
      <p class="miss">
        The other {pct(1 - d.hit)}: <span class="k f-pcie"></span>copied to the GPU over PCIe {S ? "" : "≈ "}{pct(d.gpu)} ·
        <span class="k f-cpu"></span>computed by the CPU in RAM {S ? "" : "≈ "}{pct(d.cpu)}
        {#if !S}<span class="muted">(split by the engine's pcie_frac {eng.pcie_frac ?? "–"})</span>{/if}
      </p>
    {/if}
    {#if S && S.all}
      <p class="miss">90% of the lookups went to <b>{fmt(S.top90)}</b> experts ({pct(S.top90 / S.n)}); the cache holds
        {fmt(S.held)}. {fmt(S.used)} of {fmt(S.n)} experts were used at all.</p>
    {/if}
  </Panel>

  <div class="two">
    <Panel title="Hit rate per request" lamp={null}>
      <HitChart requests={recent} share={d.share} />
    </Panel>
    {#if S}
      <Panel title="Hit rate per layer" lamp={null}>
        <LayerBars values={layerHits} summary={hitRange}
                   describe={(l) => `${pct(S.perLayer[l].hit)} found in VRAM · ${fmt(S.perLayer[l].held)} experts held · ${fmt(S.perLayer[l].all)} lookups`} />
      </Panel>
    {:else}
      <Panel title="Cache per layer" lamp={null}>
        {#if perLayerProfile}
          <LayerBars values={Array.from(perLayerProfile, (n) => n / profile.experts)} max={Math.max(...perLayerProfile) / profile.experts}
                     summary="From {fmt(Math.min(...perLayerProfile))} to {fmt(Math.max(...perLayerProfile))} of {fmt(profile.experts)} experts a layer"
                     describe={(l) => `${fmt(perLayerProfile[l])} of ${fmt(profile.experts)} experts cached`} />
        {:else}<p class="muted">–</p>{/if}
        <p class="note">The fill at start, from the expert profile: this engine does not report what it holds now.</p>
      </Panel>
    {/if}
  </div>

  {#if S}
    <Panel title="Expert usage" lamp={null}>
      {#snippet tools()}
        <Segmented bind:value={layout} label="Order of the experts in a row" options={[["id", "By expert"], ["use", "By use"]]} />
      {/snippet}
      <div class="usage">
        <div class="usage__map">
          <UsageMap u={U} total={S.total} layers={usage.layers} experts={usage.experts} {layout} />
          <div class="legend">
            <span><b class="sw held"></b>in VRAM, used (brighter = more lookups)</span>
            <span><b class="sw idle"></b>in VRAM, not used</span>
            <span><b class="sw missd"></b>used, not in VRAM: its lookups missed</span>
          </div>
        </div>
        <div class="usage__list">
          <h3>Most used outside VRAM</h3>
          {#each S.missed as x (x.layer * 1000 + x.expert)}
            <div class="row"><span class="mono">L{x.layer} · {x.expert}</span><span>{fmt(x.lookups)}</span>
              <span class="muted">CPU {pct(x.cpu, 0)}</span></div>
          {:else}
            <p class="muted">Every used expert is in VRAM.</p>
          {/each}
        </div>
      </div>
    </Panel>
  {/if}

  <Panel title="Where the model lives" lamp={null} flush>
    <div class="tiers">
      <section class="tier tier--hot">
        <span class="tag">VRAM</span>
        <span class="name">Expert cache</span>
        <span class="big">{fmt(eng.expert_slots)} <small>slots · {gb((eng.expert_cache_mib || 0) * 1048576)} GB</small></span>
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

  .usage { display: grid; grid-template-columns: minmax(0, 1fr) 230px; gap: 14px; }
  .usage__list { display: flex; flex-direction: column; gap: 3px; font-size: var(--fs-s); min-width: 0; }
  .usage__list h3 { margin: 0 0 4px; font-size: var(--fs-s); font-weight: var(--fw-b); color: var(--dim); }
  .row { display: grid; grid-template-columns: minmax(0, 1fr) auto 64px; gap: 8px; padding: 3px 6px; background: var(--well); border-radius: 2px;
         white-space: nowrap; }
  .row span:nth-child(2) { color: var(--accent); text-align: right; font-variant-numeric: tabular-nums; }
  .row span:nth-child(3) { text-align: right; }
  .legend { display: flex; flex-wrap: wrap; gap: 6px 16px; margin-top: 8px; padding-left: 26px; font-size: var(--fs-s); color: var(--dim); }
  .sw { display: inline-block; width: 9px; height: 9px; margin-right: 6px; border-radius: 1px; vertical-align: -1px; }
  .sw.held { background: var(--value); }
  .sw.idle { background: var(--value); opacity: .25; }
  .sw.missd { background: var(--accent); }
  .miss b { color: var(--text); font-weight: var(--fw-b); }

  @media (max-width: 800px) {
    .usage { grid-template-columns: minmax(0, 1fr); }
    .legend { padding-left: 0; }
    .two, .tiers { grid-template-columns: minmax(0, 1fr); }
    .cmp { grid-template-columns: minmax(0, 1fr) auto; }
    .cmp__k { grid-column: 1 / -1; margin-bottom: -4px; }
    .cmp__s { grid-column: 1 / -1; margin-top: -4px; }
  }
</style>
