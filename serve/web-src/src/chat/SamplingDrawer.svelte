<script>
  // This chat's own thinking and sampling settings (kept in this browser). Server-wide defaults for other apps are in
  // Setup.
  import {chat, saveSettings, DEFAULTS} from "../lib/chat.svelte.js";
  import {server, projectionLoaded} from "../lib/server.svelte.js";
  import {toast} from "../lib/ui.svelte.js";
  import {fmt} from "../lib/format.js";
  import SamplingFields from "../components/SamplingFields.svelte";
  import Icon from "../components/Icon.svelte";

  let {open = $bindable(false)} = $props();
  let draft = $state({...DEFAULTS});
  let wasOpen = false;
  $effect(() => {
    if (open && !wasOpen) draft = {...chat.settings};   // a fresh copy each time it opens
    wasOpen = open;
  });
  const dirty = $derived(JSON.stringify(draft) !== JSON.stringify(chat.settings));
  const tools = $derived(server.mcp.tools || 0);
  const readyNames = $derived((server.mcp.servers || []).filter((s) => s.status === "ready" || s.status === "stopped").map((s) => s.name));

  function apply() {
    saveSettings({...draft, max: String(draft.max ?? "").trim(), seed: String(draft.seed ?? "").trim()});
    open = false;
    toast("success", "Chat settings saved", +draft.temperature === 0 ? "Greedy: the same question gives the same answer." : "");
  }
</script>

<svelte:window onkeydown={(e) => { if (e.key === "Escape" && open) open = false; }} />

{#if open}<div class="scrim" onclick={() => (open = false)} aria-hidden="true"></div>{/if}
<aside class="st-drawer" data-open={open} aria-label="Chat settings" aria-hidden={!open} inert={!open}>
  <div class="st-drawer__head"><span class="card-title">Chat settings</span>
    <button class="st-btn st-btn--icon" aria-label="Close" title="Close" onclick={() => (open = false)}><Icon name="close" /></button></div>
  <div class="st-drawer__body">
    <SamplingFields bind:s={draft} projection={projectionLoaded()} id="chat" />
    <div class="toggle-row"><span>Show thinking<br><span class="muted small">expanded while it streams</span></span>
      <button class="st-toggle" role="switch" aria-checked={!!draft.show} aria-label="Show thinking" onclick={() => (draft.show = !draft.show)}></button></div>
    {#if (server.mcp.servers || []).length}
      <div class="toggle-row"><span>Use tools from MCP servers<br><span class="muted small">{tools
        ? `${fmt(tools)} tools from ${readyNames.join(", ")}; the model calls them when it decides to`
        : "no server is connected yet (see the Monitor)"}</span></span>
        <button class="st-toggle" role="switch" aria-checked={draft.mcp !== false} aria-label="Use tools from MCP servers"
                onclick={() => (draft.mcp = draft.mcp === false)}></button></div>
    {/if}
    <p class="muted small">Only this page's chat uses these. Defaults for other apps (omp, Pi, scripts) are under Setup.</p>
    <div class="drawer-actions">
      <button class="st-btn st-btn--secondary" type="button" onclick={() => (draft = {...DEFAULTS})}>Reset</button>
      <button class="st-btn st-btn--primary" type="button" onclick={apply} disabled={!dirty}>{dirty ? "Apply" : "Saved"}</button>
    </div>
  </div>
</aside>

<style>
  .st-drawer { z-index: 40; width: 380px; }
  .st-drawer__head { padding: 10px 12px 10px 16px; }
  .st-drawer__body { padding: 16px; gap: 18px; }
  .scrim { position: fixed; inset: 0; background: rgba(0, 0, 0, .35); z-index: 39; }
  .drawer-actions { display: grid; grid-template-columns: 1fr 1fr; gap: var(--st-s-3); }
  .drawer-actions .st-btn { height: 34px; box-shadow: none; }
  p { margin: 0; }
</style>
