<script>
  // Context fill (the running request, else the last one) and the memory the engine holds.
  import {fmt, kfmt, ctxfmt, gb} from "../lib/format.js";
  import Panel from "../ui/Panel.svelte";
  import Value from "../ui/Value.svelte";
  let {metrics} = $props();

  const d = $derived.by(() => {
    const live = metrics?.live || {}, hw = metrics?.hardware || {}, eng = metrics?.engine || {};
    const last = metrics?.requests?.[0];
    const ctx = eng.max_context || 0;
    const running = live.state === "reading" || live.state === "generating";
    const used = running ? (live.prompt_tokens || 0) + (live.generated || 0) : last ? (last.prompt_tokens || 0) + (last.output_tokens || 0) : 0;
    const cacheBytes = (eng.expert_cache_mib || 0) * 1048576;
    return {
      running, pct: ctx ? Math.min(100, (100 * used) / ctx) : 0, used: ctx ? `${kfmt(used)} / ${ctxfmt(ctx)}` : "–",
      bars: [
        {label: "Experts in VRAM", text: eng.expert_slots ? `${fmt(eng.expert_slots)} · ${gb(cacheBytes)} GB` : "–",
         pct: hw.gpu_mem_total ? Math.min(100, (100 * cacheBytes) / hw.gpu_mem_total) : 0},
        {label: "System RAM", text: hw.ram_total ? `${gb(hw.ram_used)} / ${gb(hw.ram_total, 0)} GB` : "–",
         pct: hw.ram_total ? (100 * hw.ram_used) / hw.ram_total : 0, danger: 92},
        {label: "GPU temperature", text: hw.gpu_temp == null ? "–" : `${fmt(hw.gpu_temp)} °C`, pct: hw.gpu_temp == null ? 0 : Math.min(100, hw.gpu_temp), danger: 85},
      ],
    };
  });
</script>

<Panel title="Context" lamp={null}>
  <div class="fill"><Value value={`${Math.round(d.pct)}%`} large active={d.running} /><span class="muted">{d.used}</span></div>
  <div class="bar"><i style:width="{d.pct}%"></i></div>
  {#each d.bars as b (b.label)}
    <div class="line"><span class="muted">{b.label}</span><span>{b.text}</span></div>
    <div class="bar" class:danger={b.danger && b.pct > b.danger}><i style:width="{b.pct}%"></i></div>
  {/each}
</Panel>

<style>
  .fill { display: flex; align-items: baseline; gap: 8px; margin-bottom: 4px; font-size: var(--fs-s); }
  .line { display: flex; justify-content: space-between; gap: 8px; margin-top: 9px; font-size: var(--fs-s); }
  .bar { height: 4px; margin-top: 3px; border-radius: 2px; background: var(--well); overflow: hidden; }
  .bar i { display: block; height: 100%; background: var(--value); transition: width 300ms ease; }
  .bar.danger i { background: var(--danger); }
</style>
