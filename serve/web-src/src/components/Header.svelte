<script>
  import {Tabs} from "bits-ui";
  import {ui, flipTheme} from "../lib/ui.svelte.js";
  import {server, auth, monitor} from "../lib/server.svelte.js";
  import {fmt} from "../lib/format.js";
  import Button from "../ui/Button.svelte";

  let {tabs} = $props();

  // what the server is doing right now (for every client, not only this page's chat)
  const status = $derived.by(() => {
    if (auth.needed) return {lamp: "error", text: "API key needed"};
    if (!server.reachable) return {lamp: "error", text: "Server not reachable"};
    let live = server.metrics?.live;
    if (!live) return {lamp: "", text: "Connecting…"};
    // on the Monitor the 5-a-second stream is fresher than the 1-a-second metrics: both should say the same
    const s = server.stream;
    if (ui.tab === "monitor" && monitor.live && s?.state)
      live = {...live, state: s.state, tok_s: s.tok_s ?? live.tok_s, prompt_read: s.prompt_read, prompt_total: s.prompt_total};
    if (live.queued > 0) return {lamp: "on", text: `${live.queued} queued`};
    if (live.state === "reading") {
      const pct = live.prompt_total ? Math.round((100 * live.prompt_read) / live.prompt_total) : null;
      return {lamp: "value", text: pct != null ? `Reading · ${pct}%` : "Reading"};
    }
    if (live.state === "generating") return {lamp: "on", text: `Generating · ${fmt(live.tok_s, 1)} tok/s`};
    if (live.state === "unloaded") return {lamp: "", text: "Unloaded"};
    return {lamp: "", text: "Idle"};
  });
</script>

<header class="bar">
  <span class="brand">Strata</span>
  <Tabs.List class="tabs" aria-label="Views">
    {#each tabs as t (t.id)}<Tabs.Trigger value={t.id} id="tab-btn-{t.id}" class="tab">{t.label}</Tabs.Trigger>{/each}
  </Tabs.List>
  <span class="status" role="status" aria-live="polite" title="What the server is doing, for every app that uses it">
    <span class="lamp" data-lamp={status.lamp}></span><span class="status__text">{status.text}</span></span>
  <Button variant="ghost" icon={ui.theme === "dark" ? "sun" : "moon"} onclick={flipTheme} title="Light / dark" aria-label="Switch between light and dark" />
</header>

<style>
  .bar { display: flex; align-items: center; gap: 10px; flex: none; height: 30px; padding: 0 4px 0 10px; background: var(--head);
         border-bottom: var(--pgap) solid var(--gap); }
  .brand { font-weight: var(--fw-b); font-size: var(--fs-m); }
  :global(.tabs) { display: flex; gap: 2px; }
  :global(.tab) { height: var(--ctl-h); padding: 0 10px; border: 0; border-radius: var(--r); background: transparent; color: var(--dim);
                  font-size: var(--fs-m); cursor: pointer; }
  :global(.tab:hover) { color: var(--text); background: var(--cell); }
  :global(.tab[data-state="active"]) { background: var(--cell); color: var(--text); font-weight: var(--fw-b); box-shadow: inset 0 -2px 0 var(--value); }
  .status { margin-left: auto; display: flex; align-items: center; gap: 6px; min-width: 0; font-size: var(--fs-s); color: var(--dim); }
  .status__text { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .lamp { flex: none; width: 8px; height: 8px; border-radius: 50%; background: var(--off); }
  .lamp[data-lamp="on"] { background: var(--accent); }
  .lamp[data-lamp="value"] { background: var(--value); }
  .lamp[data-lamp="error"] { background: var(--danger); }
  @media (max-width: 640px) { .bar { gap: 6px; padding-left: 6px; } :global(.tab) { padding: 0 7px; } .status { max-width: 40vw; } }
</style>
