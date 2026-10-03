<script>
  import {tick} from "svelte";
  import {chat, send} from "../lib/chat.svelte.js";
  import {server, loadMcp} from "../lib/server.svelte.js";
  import Message from "./Message.svelte";
  import Composer from "./Composer.svelte";
  import SamplingDrawer from "./SamplingDrawer.svelte";

  let {active} = $props();
  let scroller = $state(), drawerOpen = $state(false);

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

<div class="chat-scroll" bind:this={scroller}>
  <div class="chat">
    {#if !chat.messages.length}
      <div class="chat-empty">
        <div class="chat-empty__title">Ask anything</div>
        <div class="chat-empty__sub">{server.health.model} runs on this PC. Nothing leaves it.</div>
      </div>
    {/if}
    {#each chat.messages as m, i (m.time + ":" + i)}
      <Message {m} streaming={chat.busy?.msg === m} />
    {/each}
  </div>
</div>
<Composer {active} onSettings={() => { drawerOpen = true; loadMcp(); }} />
<SamplingDrawer bind:open={drawerOpen} />

<style>
  .chat-scroll { flex: 1; min-height: 0; overflow-y: auto; font-size: 14px; }
  .chat { max-width: 860px; margin: 0 auto; padding: var(--st-s-8) var(--st-s-6) var(--st-s-6); display: flex;
          flex-direction: column; gap: var(--st-s-6); }
  .chat-empty { margin: 12vh auto 0; text-align: center; }
  .chat-empty__title { font-weight: var(--st-fw-black); font-size: var(--st-fs-display); line-height: var(--st-lh-tight); letter-spacing: -.02em; }
  .chat-empty__sub { margin-top: var(--st-s-3); color: var(--st-ink-muted); }
  .chat :global(.st-bubble) { font-size: 14px; }
  @media (max-width: 640px) { .chat { padding: var(--st-s-5) var(--st-s-4); } }
</style>
