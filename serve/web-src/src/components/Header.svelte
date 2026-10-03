<script>
  import {ui, flipTheme} from "../lib/ui.svelte.js";
  import {server, auth} from "../lib/server.svelte.js";
  import {fmt} from "../lib/format.js";
  import Icon from "./Icon.svelte";

  let {tabs, onTab} = $props();

  // what the server is doing right now (for every client, not only this page's chat)
  const pill = $derived.by(() => {
    if (auth.needed) return {state: "queued", text: "API key needed"};
    if (!server.reachable) return {state: "queued", text: "Server not reachable"};
    const live = server.metrics?.live;
    if (!live) return {state: "idle", text: "Connecting…"};
    if (live.queued > 0) return {state: "queued", text: `${live.queued} queued`};
    if (live.state === "reading") {
      const pct = live.prompt_total ? Math.round((100 * live.prompt_read) / live.prompt_total) : null;
      return {state: "reading", text: pct != null ? `Reading prompt · ${pct}%` : "Reading prompt"};
    }
    if (live.state === "generating") return {state: "generating", text: `Generating · ${fmt(live.tok_s, 1)} tok/s`};
    if (live.state === "unloaded") return {state: "idle", text: "Unloaded"};
    return {state: "idle", text: "Idle"};
  });
</script>

<header class="st-header">
  <span class="st-header__brand">Strata</span>
  <div class="st-tabs" role="tablist" aria-label="Views">
    {#each tabs as t (t.id)}
      <button class="st-tab" role="tab" id="tab-btn-{t.id}" aria-controls="view-{t.id}" aria-selected={ui.tab === t.id}
              onclick={() => onTab(t.id)}>
        <Icon name={t.icon} size="sm" />{t.label}</button>
    {/each}
  </div>
  <span class="st-header__spacer"></span>
  <span class="st-pill" data-state={pill.state} role="status" aria-live="polite" title="What the server is doing, for every app that uses it">
    <span class="st-pill__dot"></span><span class="pill-text">{pill.text}</span></span>
  <button class="st-btn st-btn--icon" onclick={flipTheme} title="Light / dark" aria-label="Switch between light and dark">
    <Icon name={ui.theme === "dark" ? "sun" : "moon"} /></button>
</header>

<style>
  .st-header { position: relative; z-index: 2; flex: none; height: 36px; min-height: 36px; padding: 0 8px; gap: 10px; }
  .st-header__brand { font-size: 13px; }
  .st-tabs { margin-left: var(--st-s-2); padding: 2px; }
  .st-tab { font-size: 12px; padding: 2px 12px; height: 26px; min-height: 0; border-radius: 2px; }
  .st-tab :global(.st-icon) { display: none; }
  .st-pill { font-size: 11px; height: 24px; padding: 0 9px; max-width: 46vw; overflow: hidden; }
  .pill-text { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .st-btn--icon { height: 26px; width: 28px; }
  @media (max-width: 640px) {
    .st-header { gap: 6px; padding: 0 6px; }
    .st-tabs { margin-left: 0; }
    .st-tab { padding: 2px 8px; }
    .st-pill { max-width: 38vw; }
  }
</style>
