<script>
  // The Memory tab: where the model's experts live (SSD, RAM, VRAM) and how the work moves between them. The
  // placement comes from the expert profile (web/expert-profile.js, built from data/expert-profile.bin); the rates
  // from /metrics.
  import {onMount} from "svelte";
  import {server} from "../lib/server.svelte.js";
  import {fmt} from "../lib/format.js";
  import {decodeProfile, tokensPerPass} from "../lib/experts.js";
  import {store} from "../lib/storage.js";
  import Panel from "../ui/Panel.svelte";
  import Segmented from "../ui/Segmented.svelte";
  import Tiers from "./Tiers.svelte";
  import ExpertMap from "./ExpertMap.svelte";

  let profile = $state.raw(null), failed = $state(false);
  let layout = $state(store.get("memory.layout", "id"));
  $effect(() => store.set("memory.layout", layout));

  onMount(async () => {
    try {
      const mod = await import(/* @vite-ignore */ new URL("web/expert-profile.js", document.baseURI).href);
      profile = decodeProfile(mod.default);
    } catch (e) {
      failed = true;
    }
  });

  const m = $derived(server.metrics);
  const live = $derived(m?.live || {});
  const eng = $derived(m?.engine || {});
  const last = $derived(m?.requests?.[0] || {});
  const busy = $derived(live.state === "generating" || live.state === "reading");
  const total = $derived(profile ? profile.layers * profile.experts : 0);
  const topK = 10;   // experts each token routes to in every layer (qwen4exp: expert_used_count)
  const r = $derived.by(() => {
    const tps = tokensPerPass(m?.requests);
    const tok = live.state === "generating" ? live.tok_s : null;
    return {
      tps,
      passes: tok == null ? null : tok / tps,
      lookups: tok == null || !profile ? null : tok * profile.layers * topK,
      hit: last.hit_rate,
    };
  });
</script>

<div class="memory">
  <Panel title="Expert memory" lamp={live.state === "generating" ? "on" : live.state === "reading" ? "value" : ""} flush>
    {#snippet tools()}
      <Segmented bind:value={layout} label="Order of the experts in a row" options={[["id", "By expert"], ["rank", "By rank"]]} />
    {/snippet}

    <Tiers metrics={m} {total} />

    <div class="readouts">
      <div class="ro"><span class="ro__k">Expert lookups</span>
        <span class="ro__v" class:on={r.lookups != null}>{r.lookups == null ? "–" : `≈ ${fmt(r.lookups)}`}<small>/s</small></span>
        <span class="ro__s">tok/s × {profile?.layers ?? 48} layers × {topK}</span></div>
      <div class="ro"><span class="ro__k">Passes through the model</span>
        <span class="ro__v" class:on={r.passes != null}>{r.passes == null ? "–" : fmt(r.passes, 1)}<small>/s</small></span>
        <span class="ro__s">{fmt(r.tps, 2)} tokens a pass (MTP drafts)</span></div>
      <div class="ro"><span class="ro__k">Found in VRAM</span>
        <span class="ro__v value">{r.hit == null ? "–" : fmt(100 * r.hit, 1)}<small>%</small></span>
        <span class="ro__s">of the last request's lookups</span></div>
      <div class="ro"><span class="ro__k">Decode</span>
        <span class="ro__v" class:on={live.state === "generating"}>{live.state === "generating" ? fmt(live.tok_s, 1) : last.decode_tok_s != null ? fmt(last.decode_tok_s, 1) : "–"}<small>tok/s</small></span>
        <span class="ro__s">{live.state === "generating" ? "now" : live.state === "reading" ? "reading the input" : "last request"}</span></div>
    </div>

    <div class="stage">
      {#if profile}
        <ExpertMap {profile} slots={eng.expert_slots || 0} phase={live.state} hitRate={last.hit_rate} pcieFrac={Number(eng.pcie_frac) || 0} {layout} />
      {:else if failed}
        <p class="muted pad">The expert profile (web/expert-profile.js) is not there: build the web app again.</p>
      {:else}
        <p class="muted pad">Loading the expert profile…</p>
      {/if}
      <div class="legend">
        <span><b class="sw vram"></b>in the VRAM cache at start</span>
        <span><b class="sw ram"></b>in RAM only</span>
        {#if busy}
          <span><b class="sw hit"></b>looked up in VRAM</span>
          <span><b class="sw pcie"></b>copied over PCIe</span>
          <span><b class="sw cpu"></b>computed by the CPU</span>
        {/if}
        <span class="legend__note">{layout === "rank" ? "each row best first: the cyan run is that layer's share of the cache"
          : "48 layers × 512 experts, by expert number"}</span>
      </div>
    </div>
  </Panel>

  <Panel title="How to read this" lamp={null}>
    <ul class="notes">
      <li>Every expert of the model is in RAM. The GPU keeps the most used ones in VRAM, chosen by the expert profile
        (the cyan cells, brighter = used more); the CPU computes the others where they are, or they are copied to the
        GPU over PCIe.</li>
      <li>The cells show where the experts sit when the engine starts. While you chat it swaps a few of them toward the
        conversation, and it does not report which.</li>
      <li>While the model works, a sweep runs down the layers: one token's routing, slowed down, at the measured hit
        rate. Which experts light up is an impression; the engine does not report the routed experts.</li>
      <li>Every number above is measured: the disk and PCIe traffic each second, the hit rate and the drafts per request.</li>
    </ul>
  </Panel>
</div>

<style>
  .memory { display: flex; flex-direction: column; gap: var(--pgap); padding: var(--pgap); }
  .memory > :global(*) { flex: none; }
  .readouts { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: var(--pgap); padding: 0 10px 10px; }
  .ro { display: flex; flex-direction: column; gap: 2px; padding: 8px 12px; background: var(--well); border-radius: var(--r); min-width: 0; }
  .ro__k { font-size: var(--fs-s); color: var(--dim); }
  .ro__v { font: var(--fw-b) 26px/1.1 var(--mono); font-variant-numeric: tabular-nums; color: var(--text); white-space: nowrap; }
  .ro__v.on { color: var(--accent); text-shadow: 0 0 14px var(--accent-tint); }
  .ro__v.value { color: var(--value); }
  .ro__v small { margin-left: 4px; font: var(--fw) var(--fs-s) var(--font); color: var(--dim); }
  .ro__s { font-size: var(--fs-s); color: var(--off); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .stage { padding: 0 10px 10px; }
  .pad { padding: 40px 0; text-align: center; }
  .legend { display: flex; flex-wrap: wrap; align-items: center; gap: 6px 16px; margin-top: 8px; padding-left: 26px; font-size: var(--fs-s); color: var(--dim); }
  .legend__note { margin-left: auto; color: var(--off); }
  .sw { display: inline-block; width: 9px; height: 9px; margin-right: 6px; border-radius: 1px; vertical-align: -1px; }
  .sw.vram { background: var(--value); }
  .sw.ram { background: var(--text); opacity: .25; }
  .sw.hit { background: #e9fbff; box-shadow: 0 0 6px var(--value); }
  .sw.pcie { background: var(--text); }
  .sw.cpu { background: var(--accent); box-shadow: 0 0 6px var(--accent); }
  .notes { margin: 0; padding-left: 18px; display: flex; flex-direction: column; gap: 5px; font-size: var(--fs-m); color: var(--dim); line-height: 1.45; max-width: 980px; }
  @media (max-width: 800px) {
    .readouts { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    .ro__v { font-size: 20px; }
    .legend { padding-left: 0; }
    .legend__note { margin-left: 0; }
  }
</style>
