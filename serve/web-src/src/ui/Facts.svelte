<script>
  import {copyText} from "../lib/ui.svelte.js";
  let {rows} = $props();   // [label, value, copyable]
</script>

<dl class="facts">
  {#each rows.filter((r) => r[1] != null && r[1] !== "") as [k, v, copy]}
    <dt>{k}</dt>
    <dd>{#if copy}<code>{v}</code><button class="iconbtn" aria-label="Copy {k}" onclick={() => copyText(v)}><svg class="icon icon--s" aria-hidden="true"><use href="web/sprite.svg#i-copy" /></svg></button>{:else}{v}{/if}</dd>
  {/each}
</dl>

<style>
  .facts { display: grid; grid-template-columns: minmax(110px, max-content) minmax(0, 1fr); gap: 5px 14px; margin: 0; }
  dt { color: var(--dim); }
  dd { margin: 0; display: flex; align-items: center; gap: 4px; overflow-wrap: anywhere; }
  code { font: var(--fs-m) var(--mono); }
  @media (max-width: 640px) { .facts { grid-template-columns: 1fr; gap: 1px; } dd { margin-bottom: 6px; } }
</style>
