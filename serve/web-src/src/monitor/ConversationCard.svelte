<script>
  // The engine's conversation cache: the live conversation in the GPU, parked ones in RAM, saved ones on disk.
  import {fmt, gb, dateTime} from "../lib/format.js";
  import Panel from "../ui/Panel.svelte";
  import Badge from "../ui/Badge.svelte";
  let {c} = $props();

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
    for (const f of [...files.values()].reverse())
      out.push({id: `file-${f.key}`, slot: "–", state: [["neutral", "Disk only"]], tokens: f.tokens, disk: f.bytes, saved: dateTime(f.mtime)});
    return out;
  });
  const budget = $derived(c ? c.budget_mib * 1048576 : 0);
  const disk = $derived((c?.files || []).reduce((a, f) => a + f.bytes, 0));
</script>

{#if c && c.slots}
  <Panel title="Conversation cache" lamp={c.live_tokens > 0 ? "on" : ""} flush>
    {#snippet tools()}<span>{fmt((c.parked || []).length)} of {fmt(c.slots)} parked · {gbs(c.bytes)} of {gbs(budget)} RAM</span>{/snippet}
    <div class="bar"><i style:width="{budget ? Math.min(100, (100 * c.bytes) / budget) : 0}%"></i></div>
    <div class="wrap">
      <table class="tbl">
        <thead><tr><th>Slot</th><th>State</th><th class="num">Tokens</th><th class="num">RAM</th><th class="num">On disk</th><th>Saved</th></tr></thead>
        <tbody>
          {#each rows as r (r.id)}
            <tr><td>{r.slot}</td>
              <td>{#each r.state as [tone, text]}<Badge {tone} title={text === "next out" ? "evicted first when a new conversation needs the room" : undefined}>{text}</Badge>{" "}{:else}<span class="muted">Empty</span>{/each}</td>
              <td class="num">{fmt(r.tokens)}</td><td class="num">{gbs(r.ram)}</td><td class="num">{gbs(r.disk)}</td><td>{r.saved}</td></tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="note">{c.reported ? "" : "Waiting for the engine's first report. "}Evicted since start: {fmt(c.evictions)}{c.dir
      ? ` · disk: ${(c.files || []).length} files, ${gbs(disk)} in ${c.dir}` : " · no --conversation-dir"}</p>
  </Panel>
{/if}

<style>
  .bar { height: 2px; background: var(--well); }
  .bar i { display: block; height: 100%; background: var(--value); }
  .wrap { overflow-x: auto; }
  .note { margin: 0; padding: 6px 10px; font-size: var(--fs-s); color: var(--dim); border-top: 1px solid var(--gap); }
</style>
