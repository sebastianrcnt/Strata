<script>
  import {chat} from "../lib/chat.svelte.js";
  import {answerParts} from "../lib/markdown.js";
  import {fmt} from "../lib/format.js";
  import {copyText} from "../lib/ui.svelte.js";
  import Icon from "../ui/Icon.svelte";
  import ToolCall from "./ToolCall.svelte";

  let {m, streaming = false} = $props();

  const time = (t) => new Date(t).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", hourCycle: "h23"});
  const thinkingNow = $derived(streaming && !m.text);
  // open while it streams (if wanted), closed once the answer starts - unless the user toggled it themselves
  const thinkOpen = $derived(m.thinkTouched ? !!m.thinkOpen : thinkingNow && chat.settings.show);
  const parts = $derived(m.role === "assistant" && !m.error ? answerParts(m) : []);

  function onAnswerClick(e) {
    const b = e.target.closest("[data-code-copy]");
    if (b) copyText(b.closest(".code").querySelector("pre").textContent);
  }
</script>

{#if m.role === "user"}
  <div class="msg msg--user">
    {#if m.files?.length || m.images?.length}
      <div class="attached">
        {#each m.files || [] as f}<span class="chip"><Icon name="attach" small />{f.name}</span>{/each}
        {#each m.images || [] as im}
          {#if im.url}<img src={im.url} alt={im.name || "image"}>{:else}<span class="chip"><Icon name="image" small />{im.name || "image"}</span>{/if}
        {/each}
      </div>
    {/if}
    <div class="bubble">{m.text}</div>
    <div class="meta">You · {time(m.time)}</div>
  </div>
{:else}
  <div class="msg msg--assistant">
    {#if m.reasoning}
      <details class="think" open={thinkOpen}
               ontoggle={(e) => { if (e.currentTarget.open !== thinkOpen) { m.thinkTouched = true; m.thinkOpen = e.currentTarget.open; } }}>
        <summary><span class="tri" aria-hidden="true"></span>{thinkingNow ? "Thinking…" : m.thinkSecs != null ? `Thought for ${fmt(m.thinkSecs, 1)} s` : "Thoughts"}</summary>
        {#if thinkOpen || m.thinkOpen}<div class="think__body">{m.reasoning}</div>{/if}
      </details>
    {/if}
    <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
    <div class="answer" class:cursor={streaming && (m.text || m.tools?.length)} onclick={onAnswerClick}>
      {#if m.error}
        <div class="msg-error">{m.error}</div>
      {:else if !m.text && streaming && !m.tools?.length}
        {#if m.reasoning}<span class="muted cursor">Writing</span>{:else}<span class="cursor"></span>{/if}
      {:else}
        {#each parts as p}{#if p.tool}<ToolCall t={p.tool} />{:else}{@html p.html}{/if}{/each}
      {/if}
    </div>
    <div class="meta">
      <span>{m.meta || (streaming ? "" : m.stopped ? "Stopped" : "")}</span>
      {#if !streaming && m.text}<button class="iconbtn" aria-label="Copy the answer" title="Copy" onclick={() => copyText(m.text)}><Icon name="copy" small /></button>{/if}
    </div>
  </div>
{/if}

<style>
  .msg { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
  .msg--user { align-self: flex-end; align-items: flex-end; max-width: 80%; }
  .bubble { padding: 7px 11px; border-radius: var(--r); background: var(--value-tint); white-space: pre-wrap; overflow-wrap: anywhere; line-height: 1.5; }
  .attached { display: flex; flex-wrap: wrap; gap: 4px; justify-content: flex-end; }
  .attached img { max-width: 220px; max-height: 180px; border-radius: var(--r); }
  .meta { display: flex; align-items: center; gap: 6px; min-height: 18px; font-size: var(--fs-s); color: var(--dim); }
  .think { border-radius: var(--r); background: var(--cell); }
  .think summary { display: flex; align-items: center; gap: 7px; height: 26px; padding: 0 8px; list-style: none; cursor: pointer;
                   font-size: var(--fs-s); color: var(--dim); }
  .think summary::-webkit-details-marker { display: none; }
  .tri { width: 0; height: 0; border-top: 4px solid transparent; border-bottom: 4px solid transparent; border-left: 5px solid var(--off); transition: transform 120ms; }
  .think[open] .tri { transform: rotate(90deg); }
  .think__body { max-height: 420px; overflow-y: auto; padding: 0 10px 10px; white-space: pre-wrap; overflow-wrap: anywhere;
                 font-size: var(--fs-m); line-height: 1.55; color: var(--dim); }
  .answer { line-height: 1.6; overflow-wrap: anywhere; }

  /* the answer's Markdown ({@html}) */
  .answer :global(> :first-child) { margin-top: 0; }
  .answer :global(> :last-child) { margin-bottom: 0; }
  .answer :global(:is(p, ul, ol)) { margin: 0 0 10px; }
  .answer :global(:is(ul, ol)) { padding-left: 20px; }
  .answer :global(:is(h3, h4)) { margin: 14px 0 6px; font-size: var(--fs-l); font-weight: var(--fw-b); }
  .answer :global(h4) { font-size: var(--fs-m); }
  .answer :global(code.inline) { font: .92em var(--mono); background: var(--cell); border-radius: 2px; padding: 1px 4px; }
  .answer :global(blockquote) { margin: 0 0 10px; padding-left: 10px; box-shadow: inset 2px 0 0 var(--line); color: var(--dim); }
  .answer :global(hr) { border: 0; border-top: 1px solid var(--line); margin: 14px 0; }
  .answer :global(table) { display: block; overflow-x: auto; border-collapse: collapse; margin: 0 0 10px; }
  .answer :global(:is(th, td)) { padding: 4px 8px; border-bottom: 1px solid var(--line); text-align: left; }
  .answer :global(th) { color: var(--dim); font-weight: var(--fw); }
  .cursor::after { content: ""; display: inline-block; width: 7px; height: 1em; margin-left: 2px; vertical-align: text-bottom;
                   background: var(--accent); animation: blink 1s steps(2) infinite; }
  @keyframes blink { 50% { opacity: 0; } }
</style>
