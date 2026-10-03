<script>
  // One MCP tool call in an answer: name, state and a one-line preview; it opens to the arguments and the result as
  // the model read it. The body is built only while open: a result can be 20,000 characters.
  import {fmt} from "../lib/format.js";
  import Icon from "../components/Icon.svelte";
  let {t} = $props();

  const STATE = {writing: ["st-badge--reading", "Writing"], running: ["st-badge--generating", "Running"], done: ["", "Done"],
                 error: ["st-badge--error", "Error"], skipped: ["st-badge--queued", "Not run"]};
  const st = $derived(STATE[t.state] || ["", t.state]);
  const args = $derived(t.arguments == null ? "" : JSON.stringify(t.arguments, null, 2));
  const preview = $derived((t.result != null ? t.result : args.replace(/\s+/g, " ")).slice(0, 200));
</script>

<details class="st-collapse tool-call" data-state={t.state} open={t.open}
         ontoggle={(e) => { if (e.currentTarget.open !== !!t.open) t.open = e.currentTarget.open; }}>
  <summary>
    <Icon name="tool" size="sm" />
    <span class="name" title={t.name || ""}>{t.tool || t.name || "tool"}</span>
    {#if t.server}<span class="muted small">{t.server}</span>{/if}
    <span class="preview muted">{preview}</span>
    <span class="st-badge {st[0]}">{st[1]}</span>
    {#if t.ms != null && t.state !== "skipped"}<span class="muted small">{fmt(t.ms / 1000, 1)} s</span>{/if}
    <svg class="st-icon st-icon--sm st-chev" aria-hidden="true"><use href="web/sprite.svg#i-chevron" /></svg>
  </summary>
  <div class="st-collapse__body">
    {#if t.open}
      <div class="label">Arguments</div><pre>{args || "(being written)"}</pre>
      {#if t.result != null}
        <div class="label">{t.ok ? "Result" : "Error"}{t.chars ? ` · ${fmt(t.chars)} characters` : ""}{t.truncated ? ", cut for the model" : ""}</div>
        <pre>{t.result}</pre>
      {/if}
    {/if}
  </div>
</details>

<style>
  .tool-call { margin: var(--st-s-3) 0; }
  summary { min-width: 0; padding: 8px 12px; }
  .name { font: 500 var(--st-fs-sm)/1.2 var(--st-font-mono); color: var(--st-ink); white-space: nowrap; }
  .preview { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: var(--st-fs-xs); }
  .st-badge, summary .small { flex: none; }
  .tool-call[data-state="error"] { border-color: color-mix(in srgb, var(--st-danger) 35%, var(--st-line)); }
  .tool-call[data-state="running"] .st-badge::before, .tool-call[data-state="writing"] .st-badge::before { animation: blink 1s steps(2) infinite; }
  .label { font-size: var(--st-fs-xs); font-weight: var(--st-fw-medium); color: var(--st-ink-muted); margin: var(--st-s-2) 0 var(--st-s-1); }
  pre { margin: 0; padding: 10px 12px; max-height: 320px; overflow: auto; white-space: pre-wrap; overflow-wrap: anywhere;
        font: 400 12.5px/1.5 var(--st-font-mono); color: var(--st-ink-soft); background: var(--st-surface);
        border: 1px solid var(--st-line-soft); border-radius: var(--st-r-sm); }
  @keyframes blink { 50% { opacity: 0; } }
</style>
