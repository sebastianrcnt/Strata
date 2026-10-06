<script>
  // The engine's conversation cache: the live conversation in the GPU, parked ones in RAM, saved ones on disk.
  import {fmt, gb, dateTime} from "../lib/format.js";
  import Panel from "../ui/Panel.svelte";
  import Button from "../ui/Button.svelte";
  import Badge from "../ui/Badge.svelte";
  let {c} = $props();

  let showAll = $state(false);
  const gbs = (b) => (b == null ? "–" : `${gb(b)} GB`);
  const rows = $derived.by(() => {
    if (!c || !c.slots) return [];
    const files = new Map((c.files || []).map((f) => [f.key, f]));
    const out = [];
    if (c.live_tokens > 0) out.push({id: "live", slot: "Live", state: [["on", "In GPU"]], tokens: c.live_tokens, saved: "on shutdown"});
    const parked = (c.parked || []).slice().reverse();   // most recently active first
    parked.forEach((p, i) => {
      const f = files.get(p.key);
      files.delete(p.key);
      const next = i === parked.length - 1 && parked.length >= c.slots;
      out.push({id: p.key, slot: `${i + 1} / ${c.slots}`, state: [["neutral", "Parked"], ...(next ? [["warn", "next out"]] : [])],
                tokens: p.tokens, ram: p.bytes, disk: f?.bytes, saved: f ? dateTime(f.mtime) : "–"});
    });
    for (let i = parked.length; i < c.slots; i++) out.push({id: `empty-${i}`, slot: `${i + 1} / ${c.slots}`, state: [], saved: "–"});
    if (c.disk) {
      // the engine's disk tier (most recently used first); a file it does not list is a delta's parent, kept for it
      for (const d of c.disk.slice().reverse()) {
        if ((c.parked || []).some((p) => p.key === d.key)) continue;   // parked as well: its row is above
        const f = files.get(d.key);
        out.push({id: `disk-${d.key}`, slot: "–", state: [["neutral", "Disk only"]], tokens: d.tokens, disk: d.bytes,
                  saved: f ? dateTime(f.mtime) : "–"});
      }
    } else {
      for (const f of [...files.values()].reverse())
        out.push({id: `file-${f.key}`, slot: "–", state: [["neutral", "Disk only"]], tokens: f.tokens, disk: f.bytes, saved: dateTime(f.mtime)});
    }
    return out;
  });
  // files the disk tier keeps only as the parent of a delta (no conversation of their own)
  const parents = $derived(c?.disk ? (c.files || []).filter((f) => !c.disk.some((d) => d.key === f.key)).length : 0);
  const spill = $derived(c?.spill ? ` · spilled ${fmt(c.spill.spills)}, restored from disk ${fmt(c.spill.restores)}` +
    (c.spill.pending_bytes ? `, ${gbs(c.spill.pending_bytes)} waiting to be written` : "") : "");
  const shown = $derived(showAll ? rows : rows.slice(0, 5));
  const budget = $derived(c ? c.budget_mib * 1048576 : 0);
  const disk = $derived((c?.files || []).reduce((a, f) => a + f.bytes, 0));
</script>

{#if c && c.slots}
  <Panel title="Conversation cache" lamp={c.live_tokens > 0 ? "on" : ""} flush>
    {#snippet tools()}<span>{fmt((c.parked || []).length)} of {fmt(c.slots)} parked · {gbs(c.bytes)} of {gbs(budget)} RAM</span>
      {#if rows.length > 5}<Button aria-expanded={showAll} onclick={() => showAll = !showAll}>{showAll ? "Show fewer" : `Show all ${rows.length}`}</Button>{/if}
    {/snippet}
    <div class="bar"><i style:width="{budget ? Math.min(100, (100 * c.bytes) / budget) : 0}%"></i></div>
    <div class="wrap" class:all={showAll}>
      <table class="tbl">
        <thead><tr><th>Slot</th><th>State</th><th class="num">Tokens</th><th class="num">RAM</th><th class="num">On disk</th><th>Saved</th></tr></thead>
        <tbody>
          {#each shown as r (r.id)}
            <tr><td>{r.slot}</td>
              <td>{#each r.state as [tone, text]}<Badge {tone} title={text === "next out" ? "evicted first when a new conversation needs the room" : undefined}>{text}</Badge>{" "}{:else}<span class="muted">Empty</span>{/each}</td>
              <td class="num">{fmt(r.tokens)}</td><td class="num">{gbs(r.ram)}</td><td class="num">{gbs(r.disk)}</td><td>{r.saved}</td></tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="note">{c.reported ? "" : "Waiting for the engine's first report. "}Evicted since start: {fmt(c.evictions)}{spill}{c.dir
      ? ` · disk: ${c.disk ? `${fmt(c.disk.length)} conversations, ` : ""}${(c.files || []).length} files${parents
        ? ` (${fmt(parents)} kept as delta parents)` : ""}, ${gbs(disk)} in ${c.dir}` : " · no --conversation-dir"}</p>
  </Panel>
{/if}

<style>
  .bar { height: 2px; background: var(--well); }
  .bar i { display: block; height: 100%; background: var(--value); }
  .wrap { overflow-x: auto; }
  .wrap.all { max-height: 420px; overflow-y: auto; }
  .note { margin: 0; padding: 6px 10px; font-size: var(--fs-s); color: var(--dim); border-top: 1px solid var(--gap); }
</style>
