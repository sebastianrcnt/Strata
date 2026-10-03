<script>
  import {copyText} from "../lib/ui.svelte.js";
  import Icon from "./Icon.svelte";
  let {rows} = $props();   // [label, value, copyable]
</script>

<dl class="facts">
  {#each rows.filter((r) => r[1] != null && r[1] !== "") as [k, v, copy]}
    <dt>{k}</dt>
    <dd>{#if copy}<code>{v}</code><button class="st-btn st-btn--icon" aria-label="Copy {k}" onclick={() => copyText(v)}><Icon name="copy" /></button>{:else}{v}{/if}</dd>
  {/each}
</dl>

<style>
  .facts { display: grid; grid-template-columns: minmax(130px, max-content) minmax(0, 1fr); gap: 6px 16px; margin: 0; font-size: 12px; }
  dt { color: var(--st-ink-muted); }
  dd { margin: 0; color: var(--st-ink-soft); display: flex; align-items: center; gap: 6px; overflow-wrap: anywhere; }
  code { font: 400 12px var(--st-font-mono); }
  .st-btn--icon { width: 26px; height: 26px; }
  .st-btn--icon :global(.st-icon) { width: 15px; height: 15px; }
  @media (max-width: 640px) { .facts { grid-template-columns: 1fr; gap: 2px; } dd { margin-bottom: 6px; } }
</style>
