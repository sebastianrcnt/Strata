<script>
  import {tick} from "svelte";
  import {chat, send, newChat} from "../lib/chat.svelte.js";
  import {server} from "../lib/server.svelte.js";
  import Message from "./Message.svelte";
  import Composer from "./Composer.svelte";
  import ChatSettings from "./ChatSettings.svelte";
  import Button from "../ui/Button.svelte";

  let {active} = $props();
  let scroller = $state(), settingsOpen = $state(false);

  // follow the answer while the reader is at the bottom
  const nearBottom = () => scroller && scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 120;
  $effect.pre(() => {
    const last = chat.messages[chat.messages.length - 1];
    last?.text; last?.reasoning; chat.messages.length;
    if (!nearBottom()) return;
    tick().then(() => { if (scroller) scroller.scrollTop = scroller.scrollHeight; });
  });
  $effect(() => { if (active) tick().then(() => { if (scroller) scroller.scrollTop = scroller.scrollHeight; }); });

  // /?q=... starts a chat (a shortcut)
  const startQuestion = new URLSearchParams(location.search).get("q");
  if (startQuestion) {
    history.replaceState(null, "", location.pathname + location.hash);
    send(startQuestion);
  }
</script>

{#if chat.messages.length}
  <div class="top">
    <span class="muted">{server.health.model}</span>
    <Button variant="ghost" icon="new-chat" onclick={newChat} disabled={!!chat.busy}>New chat</Button>
  </div>
{/if}
<div class="scroll" bind:this={scroller}>
  <div class="chat">
    {#if !chat.messages.length}
      <div class="empty">
        <div class="empty__title">Ask anything</div>
        <div class="muted">{server.health.model} runs on this PC. Nothing leaves it.</div>
      </div>
    {/if}
    {#each chat.messages as m, i (m.time + ":" + i)}
      <Message {m} streaming={chat.busy?.msg === m} last={i === chat.messages.length - 1} />
    {/each}
  </div>
</div>
<Composer {active} onSettings={() => (settingsOpen = true)} />
<ChatSettings bind:open={settingsOpen} />

<style>
  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .top { flex: none; display: flex; align-items: center; justify-content: space-between; gap: 8px; width: 100%; max-width: 860px;
         margin: 0 auto; padding: 6px 20px 0; font-size: var(--fs-s); }
  .chat { max-width: 820px; margin: 0 auto; padding: 24px 20px; display: flex; flex-direction: column; gap: 18px; font-size: 13px; }
  .empty { margin: 14vh auto 0; text-align: center; display: flex; flex-direction: column; gap: 6px; }
  .empty__title { font-size: var(--fs-l); font-weight: var(--fw-b); }
  /* phones: text that reads at arm's length */
  @media (max-width: 640px), (hover: none) { .chat { font-size: 15px; } }
  @media (max-width: 640px) { .chat { padding: 16px 12px; } .top { padding: 6px 8px 0 12px; } }
</style>
