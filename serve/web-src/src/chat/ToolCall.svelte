<script>
  // One MCP tool call in an answer: name, state and a one-line preview; it opens to the arguments and the result as
  // the model read it. The body is built only while open: a result can be 20,000 characters.
  import {fmt} from "../lib/format.js";
  import Icon from "../ui/Icon.svelte";
  import Badge from "../ui/Badge.svelte";
  let {t} = $props();

  const STATE = {writing: ["value", "Writing"], running: ["on", "Running"], done: null, error: ["error", "Error"], skipped: ["warn", "Not run"]};
  const st = $derived(t.state in STATE ? STATE[t.state] : ["neutral", t.state]);
  const args = $derived(t.arguments == null ? "" : JSON.stringify(t.arguments, null, 2));
  const preview = $derived((t.result != null ? t.result : args.replace(/\s+/g, " ")).slice(0, 200));
</script>

<details class="tool" data-state={t.state} open={t.open}
         ontoggle={(e) => { if (e.currentTarget.open !== !!t.open) t.open = e.currentTarget.open; }}>
  <summary>
    <span class="tri" aria-hidden="true"></span><Icon name="tool" small />
    <span class="name mono" title={t.name || ""}>{t.tool || t.name || "tool"}</span>
    {#if t.server}<span class="muted small">{t.server}</span>{/if}
    <span class="preview muted">{preview}</span>
    {#if st}<Badge tone={st[0]}>{st[1]}</Badge>{/if}
    {#if t.ms != null && t.state !== "skipped"}<span class="muted small">{fmt(t.ms / 1000, 1)} s</span>{/if}
  </summary>
  {#if t.open}
    <div class="body">
      <div class="label">Arguments</div><pre>{args || "(being written)"}</pre>
      {#if t.result != null}
        <div class="label">{t.ok ? "Result" : "Error"}{t.chars ? ` · ${fmt(t.chars)} characters` : ""}{t.truncated ? ", cut for the model" : ""}</div>
        <pre>{t.result}</pre>
      {/if}
    </div>
  {/if}
</details>

<style>
  .tool { margin: 8px 0; border-radius: var(--r); background: var(--cell); }
  .tool[data-state="error"] { box-shadow: inset 2px 0 0 var(--danger); }
  summary { display: flex; align-items: center; gap: 7px; height: 26px; padding: 0 8px; list-style: none; cursor: pointer; min-width: 0; }
  summary::-webkit-details-marker { display: none; }
  .tri { flex: none; width: 0; height: 0; border-top: 4px solid transparent; border-bottom: 4px solid transparent; border-left: 5px solid var(--off);
         transition: transform 120ms; }
  .tool[open] .tri { transform: rotate(90deg); }
  .name { font-size: var(--fs-m); white-space: nowrap; }
  .preview { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: var(--fs-s); }
  .body { padding: 0 8px 8px; }
  .label { margin: 6px 0 3px; font-size: var(--fs-s); color: var(--dim); }
  pre { margin: 0; padding: 8px; max-height: 320px; overflow: auto; white-space: pre-wrap; overflow-wrap: anywhere; border-radius: var(--r);
        font: var(--fs-m)/1.5 var(--mono); background: var(--well); }
</style>
