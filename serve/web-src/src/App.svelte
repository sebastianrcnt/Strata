<script>
  import {Tabs, Tooltip} from "bits-ui";
  import {ui, showTab} from "./lib/ui.svelte.js";
  import {server, start} from "./lib/server.svelte.js";
  import Header from "./components/Header.svelte";
  import Toasts from "./components/Toasts.svelte";
  import ChatView from "./chat/ChatView.svelte";
  import MonitorView from "./monitor/MonitorView.svelte";
  import SetupView from "./setup/SetupView.svelte";
  import ExpertsView from "./experts/ExpertsView.svelte";

  start();
  // the server's "web_chat": false hides the Chat tab (the page then opens on the Monitor)
  const chatOn = $derived(server.health.web_chat !== false);
  const tabs = $derived([
    ...(chatOn ? [{id: "chat", label: "Chat"}] : []),
    {id: "monitor", label: "Monitor"},
    {id: "experts", label: "Experts"},
    {id: "about", label: "Setup"},
  ]);
  $effect(() => { if (server.healthLoaded) showTab(location.hash.slice(1), chatOn); });
</script>

<svelte:window onhashchange={() => showTab(location.hash.slice(1), chatOn)} />

<Tooltip.Provider delayDuration={350}>
  <Tabs.Root value={ui.tab} onValueChange={(v) => showTab(v, chatOn)} class="app">
    <Header {tabs} />
    {#if server.healthLoaded}
      {#if chatOn}
        <Tabs.Content value="chat" class="view view--chat"><ChatView active={ui.tab === "chat"} /></Tabs.Content>
      {/if}
      <Tabs.Content value="monitor" class="view view--scroll">{#if ui.tab === "monitor"}<MonitorView />{/if}</Tabs.Content>
      <Tabs.Content value="experts" class="view view--scroll">{#if ui.tab === "experts"}<ExpertsView />{/if}</Tabs.Content>
      <Tabs.Content value="about" class="view view--scroll">{#if ui.tab === "about"}<SetupView />{/if}</Tabs.Content>
    {/if}
  </Tabs.Root>
</Tooltip.Provider>
<Toasts />

<style>
  :global(.app) { flex: 1; min-height: 0; display: flex; flex-direction: column; }
  :global(.view) { flex: 1; min-height: 0; display: flex; flex-direction: column; outline: none; }
  :global(.view--scroll) { overflow-y: auto; }
  :global(.view--chat) { background: var(--panel); }
</style>
