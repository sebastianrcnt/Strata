<script>
  import {tick} from "svelte";
  import {chat, send, stop, addFiles} from "../lib/chat.svelte.js";
  import {server} from "../lib/server.svelte.js";
  import Button from "../ui/Button.svelte";
  import Icon from "../ui/Icon.svelte";

  let {active, onSettings} = $props();
  let text = $state(""), input = $state(), fileInput = $state(), dragging = $state(false);
  // a touch screen (a phone keyboard has no Shift): Enter starts a new line, the button sends, and opening the tab
  // does not bring the keyboard up over half the screen
  const touch = matchMedia("(hover: none)").matches;

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
    if (e.key === "Enter" && !e.shiftKey && !e.isComposing && !touch) { e.preventDefault(); submit(); }
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
  $effect(() => { if (active && !touch) input?.focus(); });
</script>

<svelte:window ondragover={(e) => { if (active && hasFiles(e)) { e.preventDefault(); dragging = true; } }}
               ondragleave={(e) => { if (!e.relatedTarget) dragging = false; }}
               ondrop={(e) => { if (active && hasFiles(e)) drop(e); }} />

<div class="wrap">
  <form class="composer" class:dragging onsubmit={submit}>
    {#if chat.attachments.length}
      <div class="attachments">
        {#each chat.attachments as a, i}
          <span class="chip"><Icon name={a.kind === "file" ? "attach" : "image"} small />{a.name}
            <button type="button" class="iconbtn" aria-label="Remove {a.name}" onclick={() => chat.attachments.splice(i, 1)}><Icon name="close" small /></button></span>
        {/each}
      </div>
    {/if}
    <textarea bind:this={input} bind:value={text} rows="2" placeholder="Ask anything…" aria-label="Message"
              oninput={autosize} onkeydown={keydown} onpaste={paste}></textarea>
    <div class="bar">
      <Button variant="ghost" icon="attach" aria-label="Attach a file" onclick={() => fileInput.click()}
              title={server.health.images ? "Attach a text file or a picture (or drop it here)" : "Attach a text file (or drop it here)"} />
      <input type="file" multiple hidden bind:this={fileInput} onchange={() => { addFiles(fileInput.files); fileInput.value = ""; }}>
      <Button variant="ghost" icon="settings" title="Chat settings" aria-label="Chat settings" onclick={onSettings} />
      <span class="spacer"></span>
      {#if chat.busy}
        <Button icon="stop" onclick={stop}>Stop</Button>
      {:else}
        <span class="hint">Shift+Enter: new line</span>
      {/if}
      <Button type="submit" variant="primary" icon="send" aria-label="Send" disabled={!!chat.busy || (!text.trim() && !chat.attachments.length)} />
    </div>
  </form>
</div>

<style>
  .wrap { flex: none; padding: 0 20px 16px; }
  .composer { max-width: 820px; margin: 0 auto; display: flex; flex-direction: column; gap: 4px; padding: 6px; border-radius: var(--r);
              background: var(--well); box-shadow: inset 0 0 0 1px var(--line); }
  .composer:focus-within { box-shadow: inset 0 0 0 1.5px var(--value); }
  .composer.dragging { box-shadow: inset 0 0 0 1.5px var(--accent); }
  textarea { min-height: 40px; max-height: 40vh; padding: 4px 6px; border: 0; outline: 0; resize: none; background: transparent; font-size: 13px; line-height: 1.5; }
  .bar { display: flex; align-items: center; gap: 2px; }
  .spacer { flex: 1; }
  @media (max-width: 640px), (hover: none) { textarea { font-size: 16px; } }   /* under 16px, iOS zooms in on focus */
  .hint { margin-right: 6px; font-size: var(--fs-s); color: var(--off); }
  .attachments { display: flex; flex-wrap: wrap; gap: 4px; padding: 2px; }
  .attachments .iconbtn { width: 16px; height: 16px; }
  @media (hover: none) { .hint { display: none; } }
  @media (max-width: 640px) { .wrap { padding: 0 8px 8px; } }
</style>
