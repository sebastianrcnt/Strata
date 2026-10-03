<script>
  // The hardware and speed tracks: one row each, a label with the current value and a line over the last minutes.
  // Folded rows keep only a thin line; the choice is kept in this browser.
  import {monitor} from "../lib/server.svelte.js";
  import {store} from "../lib/storage.js";
  import {fmt, gb} from "../lib/format.js";
  import * as inputMetrics from "../lib/inputMetrics.js";
  import Panel from "../ui/Panel.svelte";
  import Button from "../ui/Button.svelte";
  import Select from "../ui/Select.svelte";
  import Value from "../ui/Value.svelte";
  import Tip from "../ui/Tip.svelte";
  import Spark from "./Spark.svelte";
  import {idleAsGap} from "../lib/history.js";

  let {metrics} = $props();

  // open by default: what moves while the model works; folded: what stays flat (VRAM sits near full, RAM and temperature
  // change slowly)
  let folded = $state(store.get("folded.v2", ["vram", "temp", "power", "cpu", "disk", "ram"]));
  const fold = (key) => {
    folded = folded.includes(key) ? folded.filter((k) => k !== key) : [...folded, key];
    store.set("folded.v2", folded);
  };
  const live = $derived(metrics?.live || {});
  const generating = $derived(live.state === "generating");

  const rows = $derived.by(() => {
    const m = metrics || {};
    const hw = m.hardware || {}, st = m.hardware_static || {}, eng = m.engine || {}, h = m.history || {};
    const last = (m.requests || [])[0];
    // a model split across several cards (issue #112): the cards show their total / mean / hottest, and each card's own
    const multi = (hw.gpus || []).length > 1;
    const per = (f) => (hw.gpus || []).map((g) => `GPU ${g.index} ${f(g)}`).join(" · ");
    const speed = generating ? live.tok_s : last ? last.decode_tok_s : null;
    const input = inputMetrics.card(live, last, fmt);
    const gen = hw.gpu_pcie_gen_max || hw.gpu_pcie_gen;
    const bigDisk = hw.disk_read_mb >= 1000;
    const toGb = (a) => a?.map((b) => (b == null ? null : b / 1073741824));
    return [
      {key: "speed", label: "Decode", value: speed == null ? null : fmt(speed, 1), unit: "tok/s", series: idleAsGap(h.tok_s), active: generating,
       sub: generating ? "now" : last ? "last request" : ""},
      {key: "prefill", label: "Input effective", value: input.rate == null ? null : fmt(input.rate), unit: "tok/s", series: idleAsGap(h.prefill_tok_s_mean),
       sub: input.detail, tip: inputMetrics.explanation, active: live.state === "reading"},
      {key: "gpu", label: "GPU load", value: hw.gpu_util == null ? null : fmt(hw.gpu_util), unit: "%", series: h.gpu_util, max: 100,
       sub: multi ? per((g) => (g.util == null ? "–" : `${fmt(g.util)}%`)) : st.gpu_name || ""},
      {key: "vram", label: "VRAM", value: hw.gpu_mem_used == null ? null : gb(hw.gpu_mem_used), unit: hw.gpu_mem_total ? `/ ${gb(hw.gpu_mem_total, 0)} GB` : "GB",
       series: toGb(h.gpu_mem_used), max: hw.gpu_mem_total ? hw.gpu_mem_total / 1073741824 : 0, sparkUnit: "GB",
       sub: multi ? per((g) => (g.mem_used == null ? "–" : `${gb(g.mem_used)} GB`)) : eng.expert_slots ? `${fmt(eng.expert_slots)} experts cached` : ""},
      {key: "temp", label: "GPU temp", value: hw.gpu_temp == null ? null : fmt(hw.gpu_temp), unit: "°C", series: h.gpu_temp, max: 90,
       sub: multi ? per((g) => (g.temp == null ? "–" : `${fmt(g.temp)}°`)) : ""},
      {key: "power", label: "Power", value: hw.gpu_power == null ? null : fmt(hw.gpu_power), unit: "W", series: h.gpu_power, max: hw.gpu_power_limit,
       sub: hw.gpu_power_limit ? `of ${fmt(hw.gpu_power_limit)} W limit` : ""},
      {key: "pcie", label: "PCIe to GPU", value: hw.gpu_pcie_rx_mb == null ? null : fmt(hw.gpu_pcie_rx_mb, hw.gpu_pcie_rx_mb < 10 ? 1 : 0), unit: "MB/s",
       series: h.gpu_pcie_rx_mb, sub: gen ? `Gen${gen}${hw.gpu_pcie_width ? ` x${hw.gpu_pcie_width}` : ""}` +
            (hw.gpu_pcie_gen && hw.gpu_pcie_gen < gen ? ` · idle Gen${hw.gpu_pcie_gen}` : "") : ""},
      {key: "cpu", label: "CPU", value: hw.cpu == null ? null : fmt(hw.cpu), unit: "%", series: h.cpu, max: 100,
       sub: st.threads ? `${st.cores ? `${st.cores} cores · ` : ""}${st.threads} threads` : ""},
      {key: "disk", label: "Disk read", value: hw.disk_read_mb == null ? null : bigDisk ? fmt(hw.disk_read_mb / 1024, 2) : fmt(hw.disk_read_mb, hw.disk_read_mb < 10 ? 1 : 0),
       unit: hw.disk_read_mb == null ? "" : bigDisk ? "GB/s" : "MB/s", sparkUnit: "MB/s", series: h.disk_read_mb,
       sub: hw.disk_read_mb == null ? (st.psutil ? "" : "needs psutil (setup installs it)") : hw.disk_write_mb == null ? "" : `write ${fmt(hw.disk_write_mb, 1)} MB/s`},
      {key: "ram", label: "System RAM", value: hw.ram_used == null ? null : gb(hw.ram_used), unit: hw.ram_total ? `/ ${gb(hw.ram_total, 0)} GB` : "GB",
       series: toGb(h.ram_used), max: hw.ram_total ? hw.ram_total / 1073741824 : 0, sparkUnit: "GB", sub: ""},
    ];
  });
  const allFolded = $derived(folded.length >= rows.length);
  const foldAll = () => { folded = allFolded ? [] : rows.map((r) => r.key); store.set("folded.v2", folded); };
  const RANGES = [{value: "30", label: "30 s"}, {value: "60", label: "1 min"}, {value: "300", label: "5 min"}];
  let range = $state(String(monitor.range));
  $effect(() => { monitor.range = Number(range); });
  // time ticks at the quarters, as m:ss
  const mss = (t) => `${Math.floor(t / 60)}:${String(Math.round(t % 60)).padStart(2, "0")}`;
  const ticks = $derived([1, .75, .5, .25].map((f) => `−${mss(monitor.range * f)}`));
</script>

<Panel title="Speed and hardware" lamp={generating ? "on" : ""} flush>
  {#snippet tools()}
    <Select bind:value={range} options={RANGES} label="Graph time range" />
    <Button onclick={foldAll}>{allFolded ? "Unfold all" : "Fold all"}</Button>
    <Button pressed={monitor.live} onclick={() => monitor.pause(monitor.live)} title="Pause to read the graphs; the server keeps running">Live</Button>
  {/snippet}
  <div class="grid">
    <div class="ruler-pad"></div>
    <div class="ruler" aria-hidden="true">{#each ticks as t}<span>{t}</span>{/each}<span>now</span></div>
    {#each rows as r (r.key)}
      {@const isFolded = folded.includes(r.key)}
      <div class="label" class:folded={isFolded}>
        <button class="fold" aria-expanded={!isFolded} onclick={() => fold(r.key)}><span class="tri" class:open={!isFolded}></span>{r.label}</button>
        {#if r.tip}<Tip text={r.tip}><span class="q" aria-label="What is {r.label}?">?</span></Tip>{/if}
        <span class="val"><Value value={r.value} unit={r.unit} active={r.active} /></span>
        {#if !isFolded && r.sub}<span class="sub">{r.sub}</span>{/if}
      </div>
      <Spark values={r.series} max={r.max} range={monitor.range} label={r.label} unit={r.sparkUnit || r.unit} folded={isFolded} />
    {/each}
  </div>
</Panel>

<style>
  .grid { display: grid; grid-template-columns: 230px minmax(0, 1fr); row-gap: 1px; background: var(--gap); }
  .ruler-pad { background: var(--head); }
  .ruler { display: flex; justify-content: space-between; align-items: center; height: 18px; padding: 0 3px; background: var(--chart);
           font-size: var(--fs-s); color: var(--dim); }
  .label { display: grid; grid-template-columns: auto auto minmax(0, 1fr); align-content: start; align-items: center; gap: 2px 6px;
           height: 58px; padding: 4px 10px 4px 3px; background: var(--panel); }
  .label.folded { height: 24px; align-content: center; padding-top: 0; padding-bottom: 0; }
  .val { grid-column: 3; justify-self: end; }
  .fold { display: flex; align-items: center; gap: 6px; min-width: 0; height: 20px; padding: 0 4px; border: 0; border-radius: var(--r);
          background: none; color: var(--dim); font-size: var(--fs-m); white-space: nowrap; cursor: pointer; }
  .fold:hover { background: var(--cell); color: var(--text); }
  .q { display: inline-grid; place-items: center; width: 14px; height: 14px; border-radius: 50%; background: var(--cell); color: var(--dim);
       font-size: 10px; text-decoration: none; }
  .tri { flex: none; width: 0; height: 0; border-top: 4px solid transparent; border-bottom: 4px solid transparent; border-left: 5px solid var(--off);
         transition: transform 120ms; }
  .tri.open { transform: rotate(90deg); }
  .sub { grid-column: 1 / -1; padding-left: 4px; font-size: var(--fs-s); color: var(--dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  @media (max-width: 640px) {
    .grid { grid-template-columns: minmax(0, 1fr); }
    .ruler-pad { display: none; }
    .label { height: auto; padding: 3px 8px 3px 3px; }
    .label.folded { height: 26px; }
    .grid :global(.spark) { height: 46px; }
    .grid :global(.spark.folded) { display: none; }
  }
</style>
