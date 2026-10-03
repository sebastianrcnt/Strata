<script>
  import {tick} from "svelte";
  import {chat, send} from "../lib/chat.svelte.js";
  import {server, loadMcp} from "../lib/server.svelte.js";
  import Message from "./Message.svelte";
  import Composer from "./Composer.svelte";
  import ChatSettings from "./ChatSettings.svelte";

  let {active} = $props();
  let scroller = $state(), settingsOpen = $state(false);

  // follow the answer while the reader is at the bottom
  const nearBottom = () => scroller && scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 120;
  $effect.pre(() => {
    const last = chat.messages[chat.messages.length - 1];
    last?.text; last?.reasoning; last?.tools?.length; chat.messages.length;
    if (!nearBottom()) return;
    tick().then(() => { if (scroller) scroller.scrollTop = scroller.scrollHeight; });
  });
  $effect(() => { if (active) tick().then(() => { if (scroller) scroller.scrollTop = scroller.scrollHeight; }); });

  // /?q=... starts a chat (a shortcut)
  const startQuestion = new URLSearchParams(location.search).get("q");
  if (startQuestion) {
    history.replaceState(null, "", location.pathname + location.hash);
    loadMcp().then(() => send(startQuestion));
  }
</script>

<div class="scroll" bind:this={scroller}>
  <div class="chat">
    {#if !chat.messages.length}
      <div class="empty">
        <div class="empty__title">Ask anything</div>
        <div class="muted">{server.health.model} runs on this PC. Nothing leaves it.</div>
      </div>
    {/if}
    {#each chat.messages as m, i (m.time + ":" + i)}
      <Message {m} streaming={chat.busy?.msg === m} />
    {/each}
  </div>
</div>
<Composer {active} onSettings={() => { settingsOpen = true; loadMcp(); }} />
<ChatSettings bind:open={settingsOpen} />

<style>
  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .chat { max-width: 820px; margin: 0 auto; padding: 24px 20px; display: flex; flex-direction: column; gap: 18px; font-size: 13px; }
  .empty { margin: 14vh auto 0; text-align: center; display: flex; flex-direction: column; gap: 6px; }
  .empty__title { font-size: var(--fs-l); font-weight: var(--fw-b); }
  @media (max-width: 640px) { .chat { padding: 16px 12px; } }
</style>
