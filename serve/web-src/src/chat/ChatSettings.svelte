<script>
  // This chat's own thinking and sampling settings (kept in this browser), in a side sheet. Server-wide defaults
  // for other apps are in Setup.
  import {chat, saveSettings, exportChat, DEFAULTS} from "../lib/chat.svelte.js";
  import {projectionLoaded} from "../lib/server.svelte.js";
  import {toast} from "../lib/ui.svelte.js";
  import Sheet from "../ui/Sheet.svelte";
  import Button from "../ui/Button.svelte";
  import Check from "../ui/Check.svelte";
  import SamplingFields from "../components/SamplingFields.svelte";

  let {open = $bindable(false)} = $props();
  let draft = $state({...DEFAULTS});
  let wasOpen = false;
  $effect(() => {
    if (open && !wasOpen) draft = {...chat.settings};   // a fresh copy each time it opens
    wasOpen = open;
  });
  const dirty = $derived(JSON.stringify(draft) !== JSON.stringify(chat.settings));

  function apply() {
    saveSettings({...draft, max: String(draft.max ?? "").trim(), seed: String(draft.seed ?? "").trim()});
    open = false;
    toast("success", "Chat settings saved", +draft.temperature === 0 ? "Greedy: the same question gives the same answer." : "");
  }
</script>

<Sheet bind:open title="Chat settings">
  <SamplingFields bind:s={draft} projection={projectionLoaded()} id="chat" />
  <Check id="chat-show" checked={!!draft.show} onchange={(v) => (draft.show = v)} label="Show thinking" hint="expanded while it streams" />
  <Button icon="download" onclick={exportChat} disabled={!chat.messages.length}>Save this chat as Markdown</Button>
  <p class="muted small">Only this page's chat uses these. Defaults for other apps (omp, Pi, scripts) are under Setup.</p>
  <div class="actions">
    <Button onclick={() => (draft = {...DEFAULTS})}>Reset</Button>
    <Button variant="primary" onclick={apply} disabled={!dirty}>{dirty ? "Apply" : "Saved"}</Button>
  </div>
</Sheet>

<style>
  p { margin: 0; }
  .actions { display: grid; grid-template-columns: 1fr 1fr; gap: 4px; }
</style>
