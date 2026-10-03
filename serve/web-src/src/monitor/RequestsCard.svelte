<script>
  // The last requests (from every client). A row opens its details right under it. Rows are keyed by their start
  // time, so the table updates in place every second and a selection or an open row stays put. Only the
  // exceptions get a badge: a normal finish is the quiet default.
  import {monitor, refreshMetrics} from "../lib/server.svelte.js";
  import {fmt, clock} from "../lib/format.js";
  import * as inputMetrics from "../lib/inputMetrics.js";
  import Panel from "../ui/Panel.svelte";
  import Button from "../ui/Button.svelte";
  import Badge from "../ui/Badge.svelte";
  import Tip from "../ui/Tip.svelte";

  let {metrics} = $props();

  const FINISH = {stop: null, length: ["warn", "Max tokens"], cancel: ["warn", "Stopped"],
                  disconnect: ["warn", "Closed"], error: ["error", "Error"]};
  const requests = $derived(metrics?.requests || []);
  const shown = $derived(monitor.showAll ? requests : requests.slice(0, 5));
  const kept = $derived(metrics?.requests_kept ?? requests.length);
  let open = $state(null);   // the open row's start time

  function toggleAll() {
    monitor.showAll = !monitor.showAll;
    refreshMetrics();
  }
  const pctOf = (a, b, d = 1) => (a != null && b ? `${fmt((100 * a) / b, d)}%` : "–");
  const finishOf = (r) => (r.finish in FINISH ? FINISH[r.finish] : ["neutral", r.finish || "–"]);
  function details(r) {
    const input = inputMetrics.summary(r);
    const f = finishOf(r);
    return [
      ["Started", new Date(r.time * 1000).toLocaleString([], {hourCycle: "h23"})],
      ["Finished", f ? `${f[1]} (${r.finish})` : "normally (stop)"],
      ["Input tokens", `${fmt(r.prompt_tokens)} · ${fmt(input.fresh)} new · ${fmt(r.reused)} reused from the cache (${pctOf(r.reused, r.prompt_tokens)})`],
      ["Input preparation", input.ms == null ? "–" : `${fmt(input.ms / 1000, 2)} s · ${input.rate == null ? "–" : `${fmt(input.rate)} tok/s effective`}`],
      ["Output tokens", fmt(r.output_tokens) + (r.engine_generated != null && r.engine_generated !== r.output_tokens ? ` (engine generated ${fmt(r.engine_generated)})` : "")],
      ["Decode", `${fmt(r.decode_tok_s, 1)} tok/s${r.decode_ms != null ? ` over ${fmt(r.decode_ms / 1000, 1)} s` : ""}`],
      ["MTP drafts accepted", r.drafts_offered ? `${fmt(r.drafts_accepted)} of ${fmt(r.drafts_offered)} · ${pctOf(r.drafts_accepted, r.drafts_offered)}` : null],
      ["Expert cache hit", r.hit_rate == null ? null : `${(r.hit_rate * 100).toFixed(1)}% of the expert lookups were in VRAM while writing (the Experts tab has the rest)`],
      ["Experts read from disk", r.file_blobs ? `${fmt(r.file_blobs)} (${fmt(r.file_mb, 1)} MB) · RAM ${fmt(r.ram_blobs)}` : null],
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
    return `Since ${since}: ${fmt(t.requests)} requests · input ${pctOf(t.reused, t.prompt_tokens, 0)} cached, ${fmt(read)} new tokens${pSpeed} · ` +
           `${fmt(t.output_tokens)} written${oSpeed}${mtp}`;
  });
  const flip = (r) => (open = open === r.time ? null : r.time);
  const onKey = (e, r) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); flip(r); } };
</script>

<Panel title="Recent requests" lamp={null} flush>
  {#snippet tools()}
    {#if kept > 5}<Button onclick={toggleAll}>{monitor.showAll ? "Show fewer" : `Show all ${kept}`}</Button>{/if}
  {/snippet}
  <div class="wrap" class:all={monitor.showAll}>
    <table class="tbl">
      <thead><tr><th>Time</th><th class="num"><Tip text="Input tokens; below, the new ones and the share reused from the prompt cache">Input</Tip></th>
        <th class="num opt"><Tip text={inputMetrics.explanation}>Input prep</Tip></th><th class="num">Output</th>
        <th class="num">Duration</th></tr></thead>
      <tbody>
        {#each shown as r (r.time)}
          {@const input = inputMetrics.summary(r)}
          {@const f = finishOf(r)}
          <tr class="row" class:open={open === r.time} tabindex="0" aria-expanded={open === r.time} onclick={() => flip(r)} onkeydown={(e) => onKey(e, r)}>
            <td>{clock(r.time)}{#if f}{" "}<Badge tone={f[0]}>{f[1]}</Badge>{/if}{#if r.projection}{" "}<Badge tone="value" title="experimental speed projection on">ESP</Badge>{/if}</td>
            <td class="num">{fmt(r.prompt_tokens)}<span class="sub">{fmt(input.fresh)} new{r.reused != null && r.prompt_tokens ? ` · ${pctOf(r.reused, r.prompt_tokens, 0)} cached` : ""}</span></td>
            <td class="num opt">{input.ms == null ? "–" : `${fmt(input.ms / 1000, 2)} s`}<span class="sub">{input.rate == null ? "–" : `${fmt(input.rate)} tok/s`}</span></td>
            <td class="num">{fmt(r.output_tokens)}<span class="sub">{fmt(r.decode_tok_s, 1)} tok/s</span></td>
            <td class="num">{fmt(r.duration_s, 1)} s</td>
          </tr>
          {#if open === r.time}
            <tr class="details"><td colspan="5"><dl>{#each details(r) as [k, v]}<dt>{k}</dt><dd>{v}</dd>{/each}</dl></td></tr>
          {/if}
        {:else}
          <tr><td colspan="5" class="muted">No requests yet</td></tr>
        {/each}
      </tbody>
    </table>
  </div>
  {#if totals}<p class="note">{totals}</p>{/if}
</Panel>

<style>
  .wrap { overflow-x: auto; }
  .wrap.all { max-height: 420px; overflow-y: auto; }
  .row { cursor: pointer; }
  .row:hover td { background: var(--cell); }
  .row.open td { background: var(--value-tint); }
  .row.open td:first-child { box-shadow: inset 2px 0 0 var(--value); }
  .details td { white-space: normal; padding: 8px 10px 10px; background: var(--well); border-top: 0; }
  dl { display: grid; grid-template-columns: max-content minmax(0, 1fr); gap: 3px 14px; margin: 0; }
  dt { color: var(--dim); }
  dd { margin: 0; overflow-wrap: anywhere; }
  @media (min-width: 1100px) { dl { grid-template-columns: repeat(2, max-content minmax(0, 1fr)); } }
  .note { margin: 0; padding: 6px 10px; font-size: var(--fs-s); color: var(--dim); border-top: 1px solid var(--gap); }
  @media (max-width: 640px) { .opt { display: none; } .wrap :global(.tbl :is(td, th)) { padding-left: 5px; padding-right: 5px; } }
</style>
