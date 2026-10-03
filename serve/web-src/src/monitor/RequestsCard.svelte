<script>
  // The last requests (from every client). A row opens its details right under it. Rows are keyed by their start
  // time, so the table updates in place every second and a selection or an open row stays put.
  import {monitor, refreshMetrics} from "../lib/server.svelte.js";
  import {fmt, clock} from "../lib/format.js";
  import * as inputMetrics from "../lib/inputMetrics.js";

  let {metrics} = $props();

  const BADGE = {stop: ["", "Done"], length: ["st-badge--queued", "Max tokens"], cancel: ["st-badge--queued", "Stopped"],
                 disconnect: ["st-badge--queued", "Closed"], error: ["st-badge--error", "Error"]};
  const requests = $derived(metrics?.requests || []);
  const shown = $derived(monitor.showAll ? requests : requests.slice(0, 5));
  const kept = $derived(metrics?.requests_kept ?? requests.length);
  let open = $state(null);   // the open row's start time

  function toggleAll() {
    monitor.showAll = !monitor.showAll;
    refreshMetrics();
  }
  const pctOf = (a, b) => (a != null && b ? `${fmt((100 * a) / b, 1)}%` : "–");
  function details(r) {
    const input = inputMetrics.summary(r);
    return [
      ["Started", new Date(r.time * 1000).toLocaleString([], {hourCycle: "h23"})],
      ["Finished as", (BADGE[r.finish] || ["", r.finish || "–"])[1] + (r.finish ? ` (${r.finish})` : "")],
      ["Input tokens", fmt(r.prompt_tokens)],
      ["New / reused", `${fmt(input.fresh)} new · ${fmt(r.reused)} reused from the cache`],
      ["Input preparation", input.ms == null ? "–" : `${fmt(input.ms / 1000, 2)} s · ${input.rate == null ? "–" : `${fmt(input.rate)} tok/s effective`}`],
      ["Output tokens", fmt(r.output_tokens) + (r.engine_generated != null && r.engine_generated !== r.output_tokens ? ` (engine generated ${fmt(r.engine_generated)})` : "")],
      ["Decode", `${fmt(r.decode_tok_s, 1)} tok/s${r.decode_ms != null ? ` over ${fmt(r.decode_ms / 1000, 1)} s` : ""}`],
      ["MTP drafts accepted", r.drafts_offered ? `${fmt(r.drafts_accepted)} of ${fmt(r.drafts_offered)} · ${pctOf(r.drafts_accepted, r.drafts_offered)}` : null],
      ["Experts already in VRAM", r.hit_rate == null ? null : `${(r.hit_rate * 100).toFixed(1)}% of lookups while writing`],
      ["Experts read from", r.ram_blobs == null ? null : `RAM ${fmt(r.ram_blobs)} · disk ${fmt(r.file_blobs)} (${fmt(r.file_mb, 1)} MB)`],
      ["Speed projection", r.projection == null ? null : r.projection ? "on" : "off (stock model)"],
      ["Duration", `${fmt(r.duration_s, 2)} s`],
    ].filter((x) => x[1] != null);
  }
  const totals = $derived.by(() => {
    const t = metrics?.totals;
    if (!t || !t.requests) return "";
    const since = new Date(t.since * 1000).toLocaleString([], {weekday: "short", hour: "2-digit", minute: "2-digit", hourCycle: "h23"});
    const read = t.prompt_tokens - t.reused;
    const pSpeed = t.prompt_ms > 0 && read > 0 ? ` at ${fmt(read / (t.prompt_ms / 1000))} tok/s effective` : "";
    const oSpeed = t.decode_ms > 0 && t.output_tokens > 0 ? ` at ${fmt(t.output_tokens / (t.decode_ms / 1000), 1)} tok/s` : "";
    const mtp = t.drafts_offered ? ` · MTP drafts ${pctOf(t.drafts_accepted, t.drafts_offered)} accepted` : "";
    return `Since ${since}: ${fmt(t.requests)} requests · ${fmt(read)} new input tokens${pSpeed} (${fmt(t.reused)} reused) · ` +
           `${fmt(t.output_tokens)} written${oSpeed}${mtp}`;
  });
  const onKey = (e, r) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); open = open === r.time ? null : r.time; } };
</script>

<div class="st-card requests">
  <div class="panel-head"><span class="card-title">Recent requests</span>
    {#if kept > 5}<button class="st-btn st-btn--secondary" onclick={toggleAll}>{monitor.showAll ? "Show fewer" : `Show all (${kept})`}</button>{/if}
  </div>
  <div class="table-wrap" class:all={monitor.showAll}>
    <table class="st-table">
      <thead><tr><th>Time</th><th>Status</th><th class="num">Prompt</th><th class="num opt">Reused</th>
        <th class="num opt" title={inputMetrics.explanation}>Input prep</th><th class="num">Output</th><th class="num">Decode tok/s</th>
        <th class="num opt" title="While writing the answer: the share of the experts looked up that were already in VRAM (experts copied over PCIe are not counted)">Hit rate</th>
        <th class="num">Duration</th></tr></thead>
      <tbody>
        {#each shown as r (r.time)}
          {@const input = inputMetrics.summary(r)}
          {@const b = BADGE[r.finish] || ["", r.finish || "–"]}
          <tr class="row" class:open={open === r.time} tabindex="0" aria-expanded={open === r.time}
              onclick={() => (open = open === r.time ? null : r.time)} onkeydown={(e) => onKey(e, r)}>
            <td>{clock(r.time)}</td>
            <td><span class="st-badge {b[0]}">{b[1]}</span>{#if r.projection != null}{" "}<span class="st-badge {r.projection ? 'st-badge--reading' : ''}" title="experimental speed projection {r.projection ? 'on' : 'off'}">{r.projection ? "ESP" : "stock"}</span>{/if}</td>
            <td class="num">{fmt(r.prompt_tokens)}<span class="sub">{fmt(input.fresh)} new</span></td>
            <td class="num opt">{fmt(r.reused)}</td>
            <td class="num opt">{input.ms == null ? "–" : `${fmt(input.ms / 1000, 2)} s`}<span class="sub">{input.rate == null ? "–" : `${fmt(input.rate)} tok/s effective`}</span></td>
            <td class="num">{fmt(r.output_tokens)}</td>
            <td class="num">{fmt(r.decode_tok_s, 1)}</td>
            <td class="num opt">{r.hit_rate == null ? "–" : `${(r.hit_rate * 100).toFixed(1)}%`}</td>
            <td class="num">{fmt(r.duration_s, 1)} s</td>
          </tr>
          {#if open === r.time}
            <tr class="details"><td colspan="9">
              <dl>{#each details(r) as [k, v]}<dt>{k}</dt><dd>{v}</dd>{/each}</dl>
            </td></tr>
          {/if}
        {:else}
          <tr><td colspan="9" class="muted">No requests yet</td></tr>
        {/each}
      </tbody>
    </table>
  </div>
  <p class="panel-note">Input effective = new tokens ÷ input preparation time, including cache handling. Short inputs can show
    lower rates despite less waiting. Live values are provisional.</p>
  <p class="panel-note">{totals}</p>
</div>

<style>
  .requests { padding: 0; border: 0; min-width: 0; }
  .table-wrap { overflow-x: auto; }
  .table-wrap.all { max-height: 420px; overflow-y: auto; }
  .row { cursor: pointer; }
  .row:hover td, .row.open td { background: var(--st-accent-tint); }
  .sub { display: block; font-size: 10px; color: var(--st-ink-muted); white-space: nowrap; }
  .details td { white-space: normal; background: var(--st-bg); padding: 8px 10px 10px; }
  dl { display: grid; grid-template-columns: max-content minmax(0, 1fr); gap: 4px 16px; margin: 0; font-size: 11px; }
  dt { color: var(--st-ink-muted); }
  dd { margin: 0; overflow-wrap: anywhere; }
  @media (min-width: 1100px) { dl { grid-template-columns: repeat(2, max-content minmax(0, 1fr)); } }
  @media (max-width: 640px) {
    .opt { display: none; }
    .st-table th, .st-table td { padding: 4px 5px; }
  }
</style>
