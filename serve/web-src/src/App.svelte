<script>
  import {ui, showTab} from "./lib/ui.svelte.js";
  import {server, start} from "./lib/server.svelte.js";
  import Header from "./components/Header.svelte";
  import Toasts from "./components/Toasts.svelte";
  import ChatView from "./chat/ChatView.svelte";
  import MonitorView from "./monitor/MonitorView.svelte";
  import SetupView from "./setup/SetupView.svelte";

  start();
  // the server's "web_chat": false hides the Chat tab (the page then opens on the Monitor)
  const chatOn = $derived(server.health.web_chat !== false);
  const tabs = $derived([
    ...(chatOn ? [{id: "chat", label: "Chat", icon: "chat"}] : []),
    {id: "monitor", label: "Monitor", icon: "activity"},
    {id: "about", label: "Setup", icon: "info"},
  ]);
  $effect(() => { if (server.healthLoaded) showTab(location.hash.slice(1), chatOn); });
  const onTab = (id) => showTab(id, chatOn);
</script>

<svelte:window onhashchange={() => showTab(location.hash.slice(1), chatOn)} />

<Header {tabs} {onTab} />
<main>
  {#if server.healthLoaded}
    {#if chatOn}
      <div class="view view--chat" id="view-chat" role="tabpanel" aria-labelledby="tab-btn-chat" hidden={ui.tab !== "chat"}>
        <ChatView active={ui.tab === "chat"} />
      </div>
    {/if}
    {#if ui.tab === "monitor"}
      <div class="view view--scroll" id="view-monitor" role="tabpanel" aria-labelledby="tab-btn-monitor">
        <MonitorView />
      </div>
    {:else if ui.tab === "about"}
      <div class="view view--scroll" id="view-about" role="tabpanel" aria-labelledby="tab-btn-about">
        <SetupView />
      </div>
    {/if}
  {/if}
</main>
<Toasts />
