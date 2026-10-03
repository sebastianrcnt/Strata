<script>
  import {tick} from "svelte";
  import {chat, send, stop, newChat, exportChat, addFiles} from "../lib/chat.svelte.js";
  import {server} from "../lib/server.svelte.js";
  import Icon from "../components/Icon.svelte";

  let {active, onSettings} = $props();
  let text = $state(""), input = $state(), fileInput = $state(), dragging = $state(false);

  function autosize() {
    if (!input) return;
    input.style.height = "auto";
    input.style.height = `${Math.min(input.scrollHeight, innerHeight * 0.4)}px`;
  }
  async function submit(e) {
    e?.preventDefault();
    const t = text;
    if (chat.busy || (!t.trim() && !chat.attachments.length)) return;
    text = "";
    await tick(); autosize();
    send(t);
  }
  function keydown(e) {
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); submit(); }
  }
  function paste(e) {
    if (!server.health.images) return;
    const files = [...(e.clipboardData?.files || [])].filter((f) => f.type.startsWith("image/"));
    if (files.length) { e.preventDefault(); addFiles(files); }
  }
  const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes("Files");
  function drop(e) {
    dragging = false;
    if (!e.dataTransfer?.files.length) return;
    e.preventDefault();
    addFiles(e.dataTransfer.files);
    input?.focus();
  }
  $effect(() => { if (active) input?.focus(); });
</script>

<svelte:window ondragover={(e) => { if (active && hasFiles(e)) { e.preventDefault(); dragging = true; } }}
               ondragleave={(e) => { if (!e.relatedTarget) dragging = false; }}
               ondrop={(e) => { if (active && hasFiles(e)) drop(e); }} />

<div class="composer-wrap">
  <form class="st-composer" class:dragging onsubmit={submit}>
    {#if chat.attachments.length}
      <div class="attachments">
        {#each chat.attachments as a, i}
          <span class="chip"><Icon name={a.kind === "file" ? "attach" : "image"} size="sm" />{a.name}
            <button type="button" class="st-btn st-btn--icon" aria-label="Remove {a.name}" onclick={() => chat.attachments.splice(i, 1)}><Icon name="trash" /></button></span>
        {/each}
      </div>
    {/if}
    <textarea bind:this={input} bind:value={text} rows="2" placeholder="Ask anything…" aria-label="Message"
              oninput={autosize} onkeydown={keydown} onpaste={paste}></textarea>
    <div class="st-composer__bar">
      <button type="button" class="st-btn st-btn--icon" aria-label="Attach a file" onclick={() => fileInput.click()}
              title={server.health.images ? "Attach a text file or a picture (or drop it here)" : "Attach a text file (or drop it here)"}>
        <Icon name="attach" /></button>
      <input type="file" multiple hidden bind:this={fileInput} onchange={() => { addFiles(fileInput.files); fileInput.value = ""; }}>
      <button type="button" class="st-btn st-btn--icon" title="New chat" aria-label="New chat" onclick={newChat}><Icon name="new-chat" /></button>
      <button type="button" class="st-btn st-btn--icon" title="Save this chat as Markdown" aria-label="Save this chat" onclick={exportChat}><Icon name="download" /></button>
      <button type="button" class="st-btn st-btn--icon" title="Sampling and thinking" aria-label="Sampling and thinking" onclick={onSettings}><Icon name="settings" /></button>
      <span class="spacer"></span>
      {#if chat.busy}
        <button type="button" class="st-btn st-btn--secondary" onclick={stop}><Icon name="stop" size="sm" />Stop</button>
      {:else}
        <span class="hint">Shift+Enter: new line</span>
      {/if}
      <button type="submit" class="st-btn st-btn--primary send" aria-label="Send" disabled={!!chat.busy || (!text.trim() && !chat.attachments.length)}>
        <Icon name="send" /></button>
    </div>
  </form>
</div>

<style>
  .composer-wrap { flex: none; padding: 0 var(--st-s-6) var(--st-s-6); }
  .st-composer { max-width: 860px; margin: 0 auto; }
  .st-composer textarea { max-height: 40vh; font-size: 14px; }
  .st-composer.dragging { outline: 2px dashed var(--st-accent); outline-offset: 3px; }
  .send { width: 42px; padding: 0; box-shadow: none; }
  .hint { font-size: var(--st-fs-xs); color: var(--st-ink-muted); margin-right: var(--st-s-2); }
  .attachments { display: flex; flex-wrap: wrap; gap: var(--st-s-2); padding: 2px 4px 0; }
  .st-composer__bar .st-btn--secondary { height: 34px; }
  @media (hover: none) { .hint { display: none; } }
  @media (max-width: 640px) { .composer-wrap { padding: 0 var(--st-s-3) var(--st-s-3); } }
</style>
