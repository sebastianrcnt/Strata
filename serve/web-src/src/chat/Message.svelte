<script>
  import {chat} from "../lib/chat.svelte.js";
  import {answerParts} from "../lib/markdown.js";
  import {fmt} from "../lib/format.js";
  import {copyText} from "../lib/ui.svelte.js";
  import Icon from "../components/Icon.svelte";
  import ToolCall from "./ToolCall.svelte";

  let {m, streaming = false} = $props();

  const time = (t) => new Date(t).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", hourCycle: "h23"});
  const thinkingNow = $derived(streaming && !m.text);
  // open while it streams (if wanted), closed once the answer starts - unless the user toggled it themselves
  const thinkOpen = $derived(m.thinkTouched ? !!m.thinkOpen : thinkingNow && chat.settings.show);
  const parts = $derived(m.role === "assistant" && !m.error ? answerParts(m) : []);

  function onBubbleClick(e) {
    const b = e.target.closest("[data-code-copy]");
    if (b) copyText(b.closest(".st-code").querySelector("pre").textContent);
  }
</script>

{#if m.role === "user"}
  <div class="st-msg st-msg--user">
    {#if m.files?.length || m.images?.length}
      <div class="msg-images">
        {#each m.files || [] as f}<span class="chip"><Icon name="attach" size="sm" />{f.name}</span>{/each}
        {#each m.images || [] as im}
          {#if im.url}<img src={im.url} alt={im.name || "image"}>{:else}<span class="chip"><Icon name="image" size="sm" />{im.name || "image"}</span>{/if}
        {/each}
      </div>
    {/if}
    <div class="st-bubble">{m.text}</div>
    <div class="st-msg__meta">You · {time(m.time)}</div>
  </div>
{:else}
  <div class="st-msg st-msg--assistant">
    {#if m.reasoning}
      <details class="st-collapse think" open={thinkOpen}
               ontoggle={(e) => { if (e.currentTarget.open !== thinkOpen) { m.thinkTouched = true; m.thinkOpen = e.currentTarget.open; } }}>
        <summary><Icon name="thinking" size="sm" />
          <span>{thinkingNow ? "Thinking…" : m.thinkSecs != null ? `Thought for ${fmt(m.thinkSecs, 1)} s` : "Thoughts"}</span>
          <svg class="st-icon st-icon--sm st-chev" aria-hidden="true"><use href="web/sprite.svg#i-chevron" /></svg></summary>
        {#if thinkOpen || m.thinkOpen}<div class="st-collapse__body thinking">{m.reasoning}</div>{/if}
      </details>
    {/if}
    <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
    <div class="st-bubble" class:cursor={streaming && (m.text || m.tools?.length)} onclick={onBubbleClick}>
      {#if m.error}
        <div class="msg-error">{m.error}</div>
      {:else if !m.text && streaming && !m.tools?.length}
        {#if m.reasoning}<span class="muted cursor">Writing</span>{:else}<span class="cursor"></span>{/if}
      {:else}
        {#each parts as p}
          {#if p.tool}<ToolCall t={p.tool} />{:else}{@html p.html}{/if}
        {/each}
      {/if}
    </div>
    <div class="st-msg__meta">
      <span>{m.meta || (streaming ? "" : m.stopped ? "Stopped" : "")}</span>
      {#if !streaming && m.text}
        <button class="st-btn st-btn--icon" aria-label="Copy the answer" title="Copy" onclick={() => copyText(m.text)}><Icon name="copy" /></button>
      {/if}
    </div>
  </div>
{/if}

<style>
  .st-msg { width: 100%; }
  .st-msg--assistant { align-self: flex-start; }
  .st-msg--assistant .st-bubble { width: 100%; }
  .st-msg--user .st-bubble { white-space: pre-wrap; overflow-wrap: anywhere; }
  .st-msg .st-collapse { width: 100%; }
  .thinking { white-space: pre-wrap; overflow-wrap: anywhere; max-height: 420px; overflow-y: auto; }
  .st-msg__meta .st-btn--icon { width: 28px; height: 28px; }
  .st-msg__meta .st-btn--icon :global(.st-icon) { width: 16px; height: 16px; }
  .msg-images { display: flex; flex-wrap: wrap; gap: var(--st-s-2); justify-content: flex-end; }
  .msg-images img { max-width: 220px; max-height: 180px; border-radius: var(--st-r-md); border: 1px solid var(--st-line); }

  /* the answer's Markdown ({@html}) */
  .st-bubble :global(> :first-child) { margin-top: 0; }
  .st-bubble :global(> :last-child) { margin-bottom: 0; }
  .st-bubble :global(:is(p, ul, ol)) { margin: 0 0 var(--st-s-3); }
  .st-bubble :global(:is(ul, ol)) { padding-left: var(--st-s-6); }
  .st-bubble :global(li + li) { margin-top: var(--st-s-1); }
  .st-bubble :global(h3) { font-size: var(--st-fs-lg); margin: var(--st-s-4) 0 var(--st-s-2); color: var(--st-ink); }
  .st-bubble :global(h4) { font-size: var(--st-fs-base); margin: var(--st-s-3) 0 var(--st-s-2); color: var(--st-ink); }
  .st-bubble :global(code.inline) { font: 400 .9em var(--st-font-mono); background: var(--st-surface-2); border: 1px solid var(--st-line-soft);
                                     border-radius: 4px; padding: 1px 5px; }
  .st-bubble :global(.st-code) { margin: var(--st-s-3) 0; }
  .st-bubble :global(blockquote) { margin: 0 0 var(--st-s-3); padding-left: var(--st-s-3); border-left: 3px solid var(--st-line);
                                   color: var(--st-ink-muted); }
  .st-bubble :global(hr) { border: 0; border-top: 1px solid var(--st-line); margin: var(--st-s-4) 0; }
  .st-bubble :global(table) { border-collapse: collapse; margin: 0 0 var(--st-s-3); font-size: var(--st-fs-md); display: block; overflow-x: auto; }
  .st-bubble :global(:is(th, td)) { border: 1px solid var(--st-line); padding: 6px 10px; text-align: left; }
  .cursor::after { content: ""; display: inline-block; width: 8px; height: 1em; vertical-align: text-bottom; margin-left: 2px;
                   border-radius: 2px; background: var(--st-accent); animation: blink 1s steps(2) infinite; }
  @keyframes blink { 50% { opacity: 0; } }
</style>
