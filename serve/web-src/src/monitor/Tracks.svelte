<script>
  // The hardware and speed tracks: one row each, a label with the current value and a line over the last minutes.
  // Folded rows keep only a thin line; the choice is kept in this browser.
  import {monitor} from "../lib/server.svelte.js";
  import {store} from "../lib/storage.js";
  import {fmt, gb} from "../lib/format.js";
  import * as inputMetrics from "../lib/inputMetrics.js";
  import Spark from "./Spark.svelte";

  let {metrics} = $props();

  const COLORS = ["#bfd38a", "#c9f0d5", "#b4d2a2", "#9bc7dc", "#d1ae55", "#b788ba", "#baa6c6", "#91abbf", "#b5b5ac", "#d29e8e"];
  let folded = $state(store.get("folded", ["gpu", "power", "pcie", "cpu", "disk", "ram"]));
  const fold = (key) => {
    folded = folded.includes(key) ? folded.filter((k) => k !== key) : [...folded, key];
    store.set("folded", folded);
  };
  const allFolded = $derived(folded.length >= 10);
  const foldAll = () => { folded = allFolded ? [] : rows.map((r) => r.key); store.set("folded", folded); };

  const rows = $derived.by(() => {
    const m = metrics || {};
    const live = m.live || {}, hw = m.hardware || {}, st = m.hardware_static || {}, eng = m.engine || {}, h = m.history || {};
    const last = (m.requests || [])[0];
    // a model split across several cards (issue #112): the cards show their total / mean / hottest, and each card's own
    const multi = (hw.gpus || []).length > 1;
    const per = (f) => (hw.gpus || []).map((g) => `GPU ${g.index} ${f(g)}`).join(" · ");
    const speed = live.state === "generating" ? live.tok_s : last ? last.decode_tok_s : null;
    const input = inputMetrics.card(live, last, fmt);
    const gen = hw.gpu_pcie_gen_max || hw.gpu_pcie_gen;
    const bigDisk = hw.disk_read_mb >= 1000;
    return [
      {key: "speed", label: "Decode", value: speed == null ? null : fmt(speed, 1), unit: "t/s", series: h.tok_s, spark: "t/s",
       sub: live.state === "generating" ? "Decode now" : last ? "Decode last request" : "Decode"},
      {key: "prefill", label: "Input effective", value: input.rate == null ? null : fmt(input.rate), unit: "t/s", series: h.prefill_tok_s_mean,
       spark: "t/s", sub: input.label, detail: input.detail, title: inputMetrics.explanation},
      {key: "gpu", label: "GPU load", value: hw.gpu_util == null ? null : fmt(hw.gpu_util), unit: "%", series: h.gpu_util, max: 100, spark: "%",
       sub: multi ? per((g) => (g.util == null ? "–" : `${fmt(g.util)}%`)) : st.gpu_name || ""},
      {key: "vram", label: "VRAM", value: hw.gpu_mem_used == null ? null : gb(hw.gpu_mem_used), unit: hw.gpu_mem_total ? `/ ${gb(hw.gpu_mem_total, 0)} GB` : "GB",
       series: h.gpu_mem_used?.map((b) => (b == null ? null : b / 1073741824)), max: hw.gpu_mem_total ? hw.gpu_mem_total / 1073741824 : 0, spark: "GB",
       sub: multi ? per((g) => (g.mem_used == null ? "–" : `${gb(g.mem_used)} GB`)) : eng.expert_slots ? `${fmt(eng.expert_slots)} experts cached` : ""},
      {key: "temp", label: "GPU temp", value: hw.gpu_temp == null ? null : fmt(hw.gpu_temp), unit: "°C", series: h.gpu_temp, max: 90, spark: "°C",
       sub: multi ? per((g) => (g.temp == null ? "–" : `${fmt(g.temp)}°`)) : ""},
      {key: "power", label: "Power", value: hw.gpu_power == null ? null : fmt(hw.gpu_power), unit: "W", series: h.gpu_power, max: hw.gpu_power_limit, spark: "W",
       sub: hw.gpu_power_limit ? `of ${fmt(hw.gpu_power_limit)} W limit` : ""},
      {key: "pcie", label: "PCIe", value: gen ? `Gen${gen}` : null, unit: hw.gpu_pcie_width ? `x${hw.gpu_pcie_width}` : "", series: h.gpu_pcie_rx_mb, spark: "MB/s",
       sub: hw.gpu_pcie_rx_mb == null ? "" : `to GPU ${fmt(hw.gpu_pcie_rx_mb, hw.gpu_pcie_rx_mb < 10 ? 1 : 0)} MB/s` +
            (hw.gpu_pcie_gen && gen && hw.gpu_pcie_gen < gen ? ` · idle Gen${hw.gpu_pcie_gen}` : "")},
      {key: "cpu", label: "CPU", value: hw.cpu == null ? null : fmt(hw.cpu), unit: "%", series: h.cpu, max: 100, spark: "%",
       sub: st.threads ? `${st.cores ? `${st.cores} cores · ` : ""}${st.threads} threads` : ""},
      {key: "disk", label: "Disk read", value: hw.disk_read_mb == null ? null : bigDisk ? fmt(hw.disk_read_mb / 1024, 2) : fmt(hw.disk_read_mb, hw.disk_read_mb < 10 ? 1 : 0),
       unit: hw.disk_read_mb == null ? "" : bigDisk ? "GB/s" : "MB/s", series: h.disk_read_mb, spark: "MB/s",
       sub: hw.disk_read_mb == null ? (st.psutil ? "" : "needs psutil (setup installs it)") : hw.disk_write_mb == null ? "" : `write ${fmt(hw.disk_write_mb, 1)} MB/s`},
      {key: "ram", label: "System RAM", value: hw.ram_used == null ? null : gb(hw.ram_used), unit: "GB",
       series: h.ram_used?.map((b) => (b == null ? null : b / 1073741824)), max: hw.ram_total ? hw.ram_total / 1073741824 : 0, spark: "GB",
       sub: hw.ram_total ? `of ${gb(hw.ram_total)} GB` : ""},
    ].map((r, i) => ({...r, color: COLORS[i]}));
  });
  const meter = (r) => {
    const v = (r.series || []).filter((x) => x != null);
    if (!v.length) return 0;
    return Math.min(100, (v[v.length - 1] / Math.max(r.max || 0, ...v.slice(-monitor.range), 1e-9)) * 100);
  };
  const ticks = $derived([monitor.range, (monitor.range * 3) / 4, monitor.range / 2, monitor.range / 4].map((s) => `−${s >= 60 && s % 60 === 0 ? `${s / 60} min` : `${s} s`}`));
</script>

<div class="toolbar">
  <div class="controls">
    <select aria-label="Graph time range" bind:value={monitor.range}>
      <option value={30}>30 s</option><option value={60}>1 min</option><option value={300}>5 min</option>
    </select>
    <button class="st-btn" onclick={foldAll}>{allFolded ? "Unfold all" : "Fold all"}</button>
    <button class="st-btn live" aria-pressed={monitor.live} onclick={() => monitor.pause(monitor.live)}>{monitor.live ? "● Live" : "■ Paused"}</button>
  </div>
  <div class="ruler" aria-hidden="true">{#each ticks as t}<span>{t}</span>{/each}<span>now</span></div>
</div>
<div class="tracks">
  {#each rows as r (r.key)}
    {@const isFolded = folded.includes(r.key)}
    <div class="track" class:folded={isFolded} style:--track-color={r.color}>
      <button class="track__label" aria-expanded={!isFolded} onclick={() => fold(r.key)} title={r.title}>
        <span class="track__name">{isFolded ? "▸" : "▾"} {r.label}</span>
        <span class="track__value">{r.value ?? "–"}{#if r.value != null && r.unit}<small>{r.unit}</small>{/if}</span>
        {#if r.sub}<span class="track__sub">{r.sub}</span>{/if}
        {#if r.detail}<span class="track__sub">{r.detail}</span>{/if}
      </button>
      <Spark values={r.series} max={r.max} range={monitor.range} label={r.label} unit={r.spark} />
      <div class="track__meter"><i style:height="{meter(r)}%"></i></div>
    </div>
  {/each}
</div>

<style>
  .toolbar { display: grid; grid-template-columns: 260px minmax(0, 1fr) 12px; height: 26px; background: var(--st-surface-2); }
  .controls { display: flex; align-items: center; gap: 5px; padding: 0 6px; border-right: 1px solid var(--st-line); }
  .controls .st-btn, .controls select { height: 18px; min-height: 0; padding: 0 6px; font: 11px var(--st-font); border: 1px solid var(--st-line);
                                        background: var(--st-bg); color: var(--st-ink); border-radius: 2px; }
  .controls .live[aria-pressed="true"] { background: #bfd38a; color: #242424; }
  .ruler { display: flex; justify-content: space-between; align-items: center; font-size: 10px; color: var(--st-ink-muted); }
  .ruler span:first-child { padding-left: 3px; }
  .tracks { display: flex; flex-direction: column; gap: 1px; }
  .track { display: grid; grid-template-columns: 260px minmax(0, 1fr) 12px; height: 74px; background: var(--st-surface); overflow: hidden; }
  .track.folded { height: 28px; }
  .track__label { display: grid; grid-template-columns: minmax(0, 1fr) auto; align-content: start; gap: 4px 6px; text-align: left;
                  cursor: pointer; border: 0; border-right: 1px solid var(--st-line); background: var(--st-surface-2); color: var(--st-ink);
                  padding: 5px 8px; font: 12px var(--st-font); overflow: hidden; }
  .track__name { background: var(--track-color); color: #242424; padding: 1px 5px; justify-self: start; white-space: nowrap; }
  .track__value { font-size: 14px; font-weight: 600; line-height: 18px; white-space: nowrap; }
  .track__value small { font-size: 10px; margin-left: 3px; font-weight: 400; color: var(--st-ink-muted); }
  .track__sub { grid-column: 1 / -1; font-size: 10px; color: var(--st-ink-muted); line-height: 1.3; overflow: hidden; text-overflow: ellipsis; }
  .folded .track__sub { display: none; }
  .track__meter { background: var(--st-bg); border-left: 1px solid var(--st-line); padding: 0 3px; display: flex; align-items: flex-end; }
  .track__meter i { display: block; width: 100%; background: var(--track-color); }

  /* phones: the label sits above the line, the line takes the full width */
  @media (max-width: 640px) {
    .toolbar { grid-template-columns: 1fr; height: auto; }
    .controls { padding: 4px 6px; border-right: 0; border-bottom: 1px solid var(--st-line); }
    .ruler { padding: 2px 6px; }
    .track { grid-template-columns: minmax(0, 1fr); grid-template-rows: auto 44px; height: auto; }
    .track.folded { grid-template-rows: auto 0; height: auto; }
    .track__label { border-right: 0; grid-template-columns: auto minmax(0, 1fr) auto; align-items: center; padding: 4px 8px; }
    .track__value { grid-column: 3; grid-row: 1; }
    .track__sub { grid-column: 2; grid-row: 1; white-space: nowrap; text-align: right; }
    .track__sub + .track__sub { display: none; }
    .track__meter { display: none; }
  }
</style>
