<script>
  // Context fill (the running request, else the last one) and the memory the engine holds.
  import {fmt, kfmt, ctxfmt, gb} from "../lib/format.js";
  let {metrics} = $props();

  const d = $derived.by(() => {
    const live = metrics?.live || {}, hw = metrics?.hardware || {}, eng = metrics?.engine || {};
    const last = metrics?.requests?.[0];
    const ctx = eng.max_context || 0;
    let used = 0;
    if (live.state && live.state !== "idle" && live.state !== "unloaded") used = (live.prompt_tokens || 0) + (live.generated || 0);
    else if (last) used = (last.prompt_tokens || 0) + (last.output_tokens || 0);
    const cacheBytes = (eng.expert_cache_mib || 0) * 1048576;
    const ramPct = hw.ram_total ? (100 * hw.ram_used) / hw.ram_total : 0;
    return {
      pct: ctx ? Math.min(100, (100 * used) / ctx) : 0, used: ctx ? `${kfmt(used)} / ${ctxfmt(ctx)}` : "–",
      slots: eng.expert_slots ? `${fmt(eng.expert_slots)} · ${gb(cacheBytes)} GB` : "–",
      slotsPct: hw.gpu_mem_total ? Math.min(100, (100 * cacheBytes) / hw.gpu_mem_total) : 0,
      ram: hw.ram_total ? `${gb(hw.ram_used)} / ${gb(hw.ram_total, 0)} GB` : "–", ramPct,
      temp: hw.gpu_temp == null ? "–" : `${fmt(hw.gpu_temp)} °C`, tempPct: hw.gpu_temp == null ? 0 : Math.min(100, hw.gpu_temp),
    };
  });
</script>

<div class="st-card ctx">
  <div class="ctx__head"><span class="card-title">Context fill</span></div>
  <div class="ctx__fill"><span class="ctx__pct">{Math.round(d.pct)}%</span><span class="muted">{d.used}</span></div>
  <div class="st-progress"><div class="st-progress__bar" style:width="{d.pct}%"></div></div>
  <div class="bar-row"><span>Experts in VRAM</span><span class="muted">{d.slots}</span></div>
  <div class="st-progress"><div class="st-progress__bar" style:width="{d.slotsPct}%"></div></div>
  <div class="bar-row"><span>System RAM</span><span class="muted">{d.ram}</span></div>
  <div class="st-progress" data-tone={d.ramPct > 92 ? "danger" : undefined}><div class="st-progress__bar" style:width="{d.ramPct}%"></div></div>
  <div class="bar-row"><span>GPU temperature</span><span class="muted">{d.temp}</span></div>
  <div class="st-progress" data-tone="warn"><div class="st-progress__bar" style:width="{d.tempPct}%"></div></div>
</div>

<style>
  .ctx { display: flex; flex-direction: column; gap: 3px; padding: 6px 10px 10px; border: 0; }
  .ctx__fill { display: flex; align-items: baseline; gap: 10px; font-size: 11px; margin: 2px 0; }
  .ctx__pct { font-size: 18px; font-weight: var(--st-fw-black); }
  .bar-row { display: flex; justify-content: space-between; gap: 8px; font-size: 11px; margin-top: 5px; }
</style>
