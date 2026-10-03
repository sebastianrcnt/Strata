<script>
  // The engine's conversation cache: the live conversation in the GPU, parked ones in RAM, saved ones on disk.
  import {fmt, gb, dateTime} from "../lib/format.js";
  let {c} = $props();

  const gbs = (b) => (b == null ? "–" : `${gb(b)} GB`);
  const rows = $derived.by(() => {
    if (!c || !c.slots) return [];
    const files = new Map((c.files || []).map((f) => [f.key, f]));
    const out = [];
    if (c.live_tokens > 0) out.push({id: "live", slot: "Live", state: [["st-badge--ok", "In GPU"]], tokens: c.live_tokens, ram: null, disk: null, saved: "on shutdown"});
    const parked = (c.parked || []).slice().reverse();   // most recently active first
    parked.forEach((p, i) => {
      const f = files.get(p.key);
      files.delete(p.key);
      const next = i === parked.length - 1 && parked.length >= c.slots;
      out.push({id: p.key, slot: `${i + 1} / ${c.slots}`, state: [["", "Parked"], ...(next ? [["st-badge--queued", "next out"]] : [])],
                tokens: p.tokens, ram: p.bytes, disk: f?.bytes, saved: f ? dateTime(f.mtime) : "–"});
    });
    for (let i = parked.length; i < c.slots; i++) out.push({id: `empty-${i}`, slot: `${i + 1} / ${c.slots}`, state: [], tokens: null, ram: null, disk: null, saved: "–"});
    for (const f of [...files.values()].reverse())
      out.push({id: `file-${f.key}`, slot: "–", state: [["", "Disk only"]], tokens: f.tokens, ram: null, disk: f.bytes, saved: dateTime(f.mtime)});
    return out;
  });
  const budget = $derived(c ? c.budget_mib * 1048576 : 0);
  const disk = $derived((c?.files || []).reduce((a, f) => a + f.bytes, 0));
</script>

{#if c && c.slots}
  <div class="st-card conv">
    <div class="panel-head"><span class="card-title">Conversation cache</span>
      <span class="muted small">{fmt((c.parked || []).length)} of {fmt(c.slots)} parked · {gbs(c.bytes)} of {gbs(budget)} RAM</span></div>
    <div class="st-progress"><div class="st-progress__bar" style:width="{budget ? Math.min(100, (100 * c.bytes) / budget) : 0}%"></div></div>
    <div class="table-wrap">
      <table class="st-table">
        <thead><tr><th>Slot</th><th>State</th><th class="num">Tokens</th><th class="num">RAM</th><th class="num">On disk</th><th>Saved</th></tr></thead>
        <tbody>
          {#each rows as r (r.id)}
            <tr><td>{r.slot}</td>
              <td>{#each r.state as [cls, text]}<span class="st-badge {cls}" title={text === "next out" ? "evicted first when a new conversation needs the room" : undefined}>{text}</span>{" "}{:else}<span class="muted">Empty</span>{/each}</td>
              <td class="num">{fmt(r.tokens)}</td><td class="num">{gbs(r.ram)}</td><td class="num">{gbs(r.disk)}</td><td>{r.saved}</td></tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="panel-note">{c.reported ? "" : "Waiting for the engine's first report. "}Evicted since start: {fmt(c.evictions)}{c.dir
      ? ` · disk: ${(c.files || []).length} files, ${gbs(disk)} in ${c.dir}` : " · no --conversation-dir"}</p>
  </div>
{/if}

<style>
  .conv { padding: 0; border: 0; min-width: 0; }
  .table-wrap { overflow-x: auto; }
</style>
